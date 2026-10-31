#include "config_mode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mdns.h"
#include "nvs.h"
#include "secrets.h"

namespace config_mode {

namespace {

const char* TAG = "config";
const char* NVS_NAMESPACE = "cfgmode";
const char* HOSTNAME = "octo-bridge";

constexpr int DEFAULT_MINUTES = 15;
constexpr int MAX_MINUTES = 60;
constexpr int64_t MINUTE_US = 60 * 1000000LL;
constexpr int64_t CONNECT_LIMIT_US = MINUTE_US;   // no Wi-Fi by then: back to Thread
constexpr int64_t HARD_LIMIT_US = 60 * MINUTE_US;  // however busy the page is
constexpr int64_t CHECK_INTERVAL_US = 10 * 1000000LL;

struct Network {
    std::string ssid;
    std::string password;
    const char* source = "none";  // "page" (settings page), "secrets" (secrets.h) or "none"
};

std::string nvs_string(nvs_handle_t handle, const char* key) {
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
        return "";
    }
    std::string value(length, '\0');
    nvs_get_str(handle, key, value.data(), &length);
    value.resize(length - 1);
    return value;
}

std::string stored_text(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return "";
    }
    std::string value = nvs_string(handle, key);
    nvs_close(handle);
    return value;
}

// The network from the settings page; secrets.h only when none was set there
Network stored_network() {
    Network network;
    network.ssid = stored_text("ssid");
    network.password = stored_text("pass");
    if (!network.ssid.empty()) {
        network.source = "page";
    }
#ifdef OCTO_WIFI_SSID
    if (network.ssid.empty() && std::strcmp(OCTO_WIFI_SSID, "your-ssid") != 0) {
        network.ssid = OCTO_WIFI_SSID;
        network.password = OCTO_WIFI_PASSWORD;
        network.source = "secrets";
    }
#endif
    return network;
}

bool store(const char* key, const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = value.empty() ? nvs_erase_key(handle, key) : nvs_set_str(handle, key, value.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// The words from `first` on, joined with spaces (an SSID may contain them)
std::string rest(const std::vector<std::string>& args, size_t first) {
    std::string text;
    for (size_t i = first; i < args.size(); ++i) {
        text += (i > first ? " " : "") + args[i];
    }
    return text;
}

void restart_soon(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));  // let the reply go out first
    esp_restart();
}

void restart_in_background() {
    xTaskCreate(restart_soon, "restart", 2048, nullptr, 5, nullptr);
}

// ------------------------------------------------------- configuration mode

// Back to normal after this long without a command from the page (its
// polling does not count), and after HARD_LIMIT_US in any case
int64_t idle_limit_us = DEFAULT_MINUTES * MINUTE_US;
volatile int64_t last_activity_us = 0;
char ip_text[16] = "";
bool http_started = false;
int last_reason = 0;  // wifi_err_reason_t of the last disconnect

// Why the last configuration mode ended early, for the settings page
void store_result(const char* why) {
    store("result", std::string(why) + " " + std::to_string(last_reason));
}

void on_check(void*) {
    const int64_t now = esp_timer_get_time();
    if (ip_text[0] == '\0' && !http_started && now > CONNECT_LIMIT_US) {
        ESP_LOGE(TAG, "No Wi-Fi connection after %lld s (reason %d), back to Thread", CONNECT_LIMIT_US / 1000000,
                 last_reason);
        // 15/202/204: the password was refused; 201: network not found
        store_result(last_reason == WIFI_REASON_NO_AP_FOUND ? "notfound"
                     : last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || last_reason == WIFI_REASON_AUTH_FAIL ||
                             last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT
                         ? "password"
                         : "noconnect");
        esp_restart();
    }
    if (now - last_activity_us > idle_limit_us || now > HARD_LIMIT_US) {
        ESP_LOGW(TAG, "Time limit reached, back to Thread");
        esp_restart();
    }
}

void start_mdns() {
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: use the IP address");
        return;
    }
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("Octo bridge (configuration mode)");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ip_text[0] = '\0';
        last_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_ip4addr_ntoa(&static_cast<ip_event_got_ip_t*>(data)->ip_info.ip, ip_text, sizeof(ip_text));
        ESP_LOGI(TAG, "Wi-Fi connected: http://%s.local/ or http://%s/", HOSTNAME, ip_text);
        if (!http_started) {
            http_started = true;
            store("result", "");
            start_mdns();
            http_api::start(OCTO_HTTP_TOKEN);
        }
    }
}

// The beds' state plus "config_mode": the settings page shows the beds and
// a configuration banner, and hides what needs Matter
std::string state_json(const Network& network) {
    std::string beds;
    bed_link::run_in_host([&] { beds = bed_link::state_json(); });
    wifi_ap_record_t ap = {};
    const bool up = ip_text[0] != '\0' && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const int64_t now = esp_timer_get_time();
    const int64_t until = std::min(last_activity_us + idle_limit_us, HARD_LIMIT_US);
    std::string out = "{\"config_mode\":{";
    out += "\"ssid\":" + json_text(network.ssid);
    out += ",\"ip\":" + json_text(ip_text);
    out += ",\"host\":" + json_text(std::string(HOSTNAME) + ".local");
    out += ",\"home\":" + json_text(stored_text("home"));
    out += ",\"rssi\":" + std::to_string(up ? ap.rssi : 0);
    out += ",\"remaining\":" + std::to_string(until > now ? (until - now) / 1000000 : 0);
    out += ",\"version\":" + json_text(esp_app_get_description()->version);
    out += "}," + beds.substr(1);  // beds is "{...}"
    return out;
}

// Offered every command first. The bed commands go on to the built-ins (an
// empty reply); Matter and Thread are off, so their commands are answered here.
std::string command_in_config_mode(const std::vector<std::string>& args) {
    static const Network network = stored_network();
    const std::string cmd = args.empty() ? "help" : args[0];
    if (cmd == "json") {
        return state_json(network);  // the page's polling: not activity
    }
    last_activity_us = esp_timer_get_time();
    if (cmd == "system") {
        return boot_guard::describe() + "configuration mode on Wi-Fi '" + network.ssid + "', ip " +
               (ip_text[0] ? ip_text : "-") + "\n";
    }
    if (cmd == "wifi" && args.size() == 2 && args[1] == "exit") {
        bed_link::run_in_host([] { bed_link::stop_all(); });
        restart_in_background();
        return "back to normal (Thread)\n";
    }
    if (cmd == "wifi" && args.size() >= 2 && args[1] == "config") {
        return "already in configuration mode\n";
    }
    if (cmd == "wifi") {
        return command(args);
    }
    if (cmd == "matter" || cmd == "preset" || cmd == "language") {
        return "not available in configuration mode: Matter is off (wifi exit to go back)\n";
    }
    if (cmd == "help") {
        return "Configuration mode on Wi-Fi: the bed commands work, Matter and Thread are off.\n"
               "system | wifi exit (back to normal) | POST /ota for a firmware update\n";
    }
    return "";
}

}  // namespace

bool requested() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    uint8_t minutes = 0;
    const bool asked = nvs_get_u8(handle, "boot", &minutes) == ESP_OK && minutes > 0;
    if (asked) {
        // Clear it first: whatever happens in this boot, the next one is normal
        nvs_erase_key(handle, "boot");
        nvs_commit(handle);
        idle_limit_us = static_cast<int64_t>(minutes) * MINUTE_US;
    }
    nvs_close(handle);
    return asked;
}

void run() {
    const Network network = stored_network();
    ESP_LOGW(TAG, "Configuration mode on Wi-Fi '%s', back to normal after %lld idle minutes", network.ssid.c_str(),
             idle_limit_us / MINUTE_US);
    last_activity_us = esp_timer_get_time();
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_check;
    timer_args.name = "cfg_check";
    esp_timer_handle_t timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, CHECK_INTERVAL_US));
    set_command_override(command_in_config_mode);

    // The beds stay usable: adding, calibrating and presets need no Matter
    bed_link::start();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), network.ssid.c_str(), sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), network.password.c_str(), sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // keep NVS as it is
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // required while BLE is on

    console_start();
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

std::string command(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[1] == "ssid") {
        return store("ssid", rest(args, 2)) ? "ok\n" : "storing failed\n";
    }
    if (args.size() >= 2 && args[1] == "password") {
        return store("pass", rest(args, 2)) ? "ok\n" : "storing failed\n";
    }
    if (args.size() == 2 && args[1] == "clear") {
        return store("ssid", "") && store("pass", "") ? "ok\n" : "storing failed\n";
    }
    // The first-run wizard: "skip" hides it, "home" is the page's normal
    // address, so configuration mode can link back to it
    if (args.size() == 2 && args[1] == "skip") {
        return store("skip", "1") ? "ok\n" : "storing failed\n";
    }
    if (args.size() == 3 && args[1] == "home") {
        return store("home", args[2]) ? "ok\n" : "storing failed\n";
    }
    const Network network = stored_network();
    if (args.size() == 2 && args[1] == "json") {
        return "{\"ssid\":" + json_text(network.ssid) + ",\"source\":\"" + network.source +
               "\",\"password_set\":" + (network.password.empty() ? "false" : "true") +
               ",\"skipped\":" + (stored_text("skip").empty() ? "false" : "true") +
               ",\"last_result\":" + json_text(stored_text("result")) +
               ",\"minutes\":" + std::to_string(DEFAULT_MINUTES) +
               ",\"host\":" + json_text(std::string(HOSTNAME) + ".local") + "}";
    }
    if (args.size() >= 2 && args[1] == "config") {
        if (network.ssid.empty()) {
            return "set a Wi-Fi network first: wifi ssid <name>, wifi password <password>\n";
        }
        int minutes = args.size() == 3 ? std::atoi(args[2].c_str()) : DEFAULT_MINUTES;
        if (minutes < 1 || minutes > MAX_MINUTES) {
            return "minutes: 1-" + std::to_string(MAX_MINUTES) + "\n";
        }
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            return "storing failed\n";
        }
        const bool stored =
            nvs_set_u8(handle, "boot", static_cast<uint8_t>(minutes)) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
        if (!stored) {
            return "storing failed\n";
        }
        bed_link::run_in_host([] { bed_link::stop_all(); });
        restart_in_background();
        return "restarting in configuration mode on Wi-Fi '" + network.ssid + "', back after " +
               std::to_string(minutes) + " idle minutes: http://" + HOSTNAME + ".local/\n";
    }
    return "wifi: '" + network.ssid + "' (" + network.source + ")" +
           (network.password.empty() ? ", no password" : ", password set") + "\n";
}

}  // namespace config_mode
