#include "device_status.h"
#include "config_mode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "boot_guard.h"
#include "commands.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "http_api.h"
#include "mb_core.h"
#include "mdns.h"
#include "nvs.h"
#include "program_access.h"
#include "sdkconfig.h"
#include "setup_code.h"
#include "setup_dns.h"
#if CONFIG_OPENTHREAD_ENABLED
#include "thread_dataset.h"
#endif

namespace config_mode {

namespace {

const char* TAG = "config";
const char* NVS_NAMESPACE = "cfgmode";
// The app's name in configuration mode: <hostname>.local
const char* hostname() { return mb_core::app().hostname; }

constexpr int DEFAULT_MINUTES = 15;
constexpr int MAX_MINUTES = 60;
constexpr int64_t MINUTE_US = 60 * 1000000LL;
constexpr int64_t CONNECT_LIMIT_US = MINUTE_US;   // no Wi-Fi by then: back to Thread
constexpr int64_t HARD_LIMIT_US = 60 * MINUTE_US;  // however busy the page is
constexpr int64_t CHECK_INTERVAL_US = 10 * 1000000LL;

struct Network {
    std::string ssid;
    std::string password;
    const char* source = "none";  // "page" (settings page) or "none"
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

// The network from the settings page, never from the firmware itself: without a network from the page, the bridge opens its setup access point
Network stored_network() {
    Network network;
    network.ssid = stored_text("ssid");
    network.password = stored_text("pass");
    if (!network.ssid.empty()) {
        network.source = "page";
    }
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
    device_status::note_restart("asked for: setup network, Wi-Fi change or fetching an update");
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
// The setup access point: no Wi-Fi network set, or it does not connect and
// the bridge has no Thread network to go back to
volatile bool ap_mode = false;
bool force_ap = false;  // recovery: the setup access point even with a Wi-Fi network
char ap_ssid[33] = "";
int last_reason = 0;  // wifi_err_reason_t of the last disconnect

// What works without Matter (the app's config_mode_start: beds, connections).
// Started once Wi-Fi is up, not before: Wi-Fi and Bluetooth share the radio,
// and two bed links next to the join made the WPA handshake time out
// (reason 15), which looked like a wrong password
bool app_started = false;

void start_app_task(void*) {
    if (mb_core::app().config_mode_start != nullptr) {
        mb_core::app().config_mode_start();
    }
    vTaskDelete(nullptr);
}

void start_app_once() {
    if (!app_started) {
        app_started = true;
        xTaskCreate(start_app_task, "cfg_app", 6144, nullptr, 5, nullptr);
    }
}

// Why the last configuration mode ended early, for the settings page
void store_result(const char* why) {
    store("result", std::string(why) + " " + std::to_string(last_reason));
}

void start_mdns();

void start_http() {
    if (!http_started) {
        http_started = true;
        start_mdns();
        http_api::start(mb_core::app().http_token_fallback);
    }
}

// Somewhere to go back to when Wi-Fi does not connect. A Wi-Fi device has
// only Wi-Fi: it stays on its setup access point until the network is fixed.
bool has_thread_network() {
#if CONFIG_OPENTHREAD_ENABLED
    uint8_t tlvs[254];
    const char* source = nullptr;
    return thread_dataset::wanted(tlvs, sizeof(tlvs), &source) > 0;
#else
    return false;
#endif
}

// What the device runs on normally: Thread, or Wi-Fi itself
#if CONFIG_OPENTHREAD_ENABLED
constexpr bool has_thread_radio() { return true; }
#else
constexpr bool has_thread_radio() { return false; }
#endif

#if CONFIG_OPENTHREAD_ENABLED
const char* NORMAL = "Thread";
#else
const char* NORMAL = "normal";
#endif

// The app's prefix and the last four hex digits of the access point's MAC
std::string setup_ap_name() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[33];
    std::snprintf(name, sizeof(name), "%.27s-%02X%02X", mb_core::app().ap_prefix, mac[4], mac[5]);
    return name;
}

// An open network "<ap_prefix>-XXXX" at 192.168.4.1, with DNS answering every
// name with that address so phones open the page by themselves
void start_ap() {
    std::strncpy(ap_ssid, setup_ap_name().c_str(), sizeof(ap_ssid) - 1);
    ap_mode = true;
    device_status::set_setup(true);
    esp_wifi_disconnect();
    esp_wifi_stop();
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.ap.ssid), ap_ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = std::strlen(ap_ssid);
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.max_connection = 3;
    config.ap.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));  // the station side only scans (wifi scan)
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    std::strcpy(ip_text, "192.168.4.1");
    setup_dns::start(esp_ip4addr_aton(ip_text));
    start_http();
    http_api::redirect_unknown_to("http://192.168.4.1/");
    start_app_once();  // an open network: no handshake for Bluetooth to disturb
    ESP_LOGW(TAG, "Setup access point '%s': join it and open http://192.168.4.1/", ap_ssid);
}

// Trying a Wi-Fi network from the setup network, before it is stored: the
// station side joins it while the setup network stays up. Only when that
// works are the name and password kept; a wrong password shows at once
// instead of after a restart and minutes of waiting.
std::string try_state;  // "", "trying", "ok", "password", "notfound", "noconnect"
std::string try_ssid, try_password;
int64_t try_started_us = 0;
constexpr int64_t TRY_LIMIT_US = 20 * 1000000LL;

void end_try(const char* result) {
    try_state = result;
    esp_wifi_disconnect();
    ESP_LOGW(TAG, "Trying Wi-Fi '%s': %s", try_ssid.c_str(), result);
    try_password.clear();
}

std::string start_try(const std::string& ssid, const std::string& password) {
    if (!ap_mode) return "error: only from the setup network\n";
    if (try_state == "trying") return "error: still trying the last one\n";
    try_ssid = ssid;
    try_password = password;
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid.c_str(), sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password.c_str(), sizeof(config.sta.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    last_reason = 0;
    try_started_us = esp_timer_get_time();
    try_state = "trying";
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        end_try("noconnect");
        return "error: could not start\n";
    }
    return "trying\n";
}

void on_check(void*) {
    const int64_t now = esp_timer_get_time();
    if (try_state == "trying" && now - try_started_us > TRY_LIMIT_US) {
        end_try(last_reason == WIFI_REASON_NO_AP_FOUND ? "notfound" : "noconnect");
    }
    if (!ap_mode && ip_text[0] == '\0' && !http_started && now > CONNECT_LIMIT_US) {
        ESP_LOGE(TAG, "No Wi-Fi connection after %lld s (reason %d)", CONNECT_LIMIT_US / 1000000, last_reason);
        // 15/202/204: the password was refused; 201: network not found
        store_result(last_reason == WIFI_REASON_NO_AP_FOUND ? "notfound"
                     : last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || last_reason == WIFI_REASON_AUTH_FAIL ||
                             last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT
                         ? "password"
                         : "noconnect");
        if (has_thread_network()) {
            esp_restart();
        }
        start_ap();  // nowhere else to go: set the bridge up on its own network
        return;
    }
    if (now - last_activity_us > idle_limit_us || now > HARD_LIMIT_US) {
        ESP_LOGW(TAG, "Time limit reached, back to %s", NORMAL);
        device_status::note_restart("setup network closed: unused for a while");
        esp_restart();
    }
}

void start_mdns() {
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: use the IP address");
        return;
    }
    mdns_hostname_set(hostname());
    static char instance[64];
    std::snprintf(instance, sizeof(instance), "%.40s (configuration mode)", mb_core::app().product_name);
    mdns_instance_name_set(instance);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

// ------------------------------------------------------- fetching an update
// "wifi fetch <url> <sha256>" (asked on Thread): this boot downloads the
// image itself as soon as Wi-Fi is up, from Home Assistant's own address.
// The device fetching it works across subnets and VLANs, where Home
// Assistant could not find the device in configuration mode (mDNS stays in
// one subnet). The hash comes over Thread from the add-on, which checked
// the release signature; nothing that does not match it is booted.
std::string fetch_url;
std::string fetch_url2;  // optional: where else (GitHub, for a release)
std::string fetch_sha;

[[noreturn]] void fetch_done(const std::string& result) {
    store("result", result);
    ESP_LOGW(TAG, "Fetching the firmware: %s", result.c_str());
    device_status::note_restart(("firmware fetched over Wi-Fi: " + result).substr(0, 60).c_str());
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();  // the new firmware, or back to Thread with the old one
    while (true) vTaskDelay(portMAX_DELAY);
}

// One address: download, check against the hash, and select the image.
// "" when it is ready to boot, else what went wrong. GitHub's download links
// redirect to a storage host, so redirects are followed.
std::string fetch_from(const std::string& url) {
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) return "no firmware slot";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;  // GitHub's redirect carries a long URL
    config.crt_bundle_attach = esp_crt_bundle_attach;  // https: the usual root certificates
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return "no HTTP client";
    int64_t length = -1;
    int status = 0;
    for (int hop = 0; hop < 5; ++hop) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            esp_http_client_cleanup(client);
            return url.rfind("https", 0) == 0 ? "the internet not reachable from this Wi-Fi"
                                             : "Home Assistant not reachable from this Wi-Fi";
        }
        length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) break;
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
    }
    if (status != 200 || length <= 0 || length > static_cast<int64_t>(target->size)) {
        esp_http_client_cleanup(client);
        return "HTTP " + std::to_string(status) + ", " + std::to_string(length) + " bytes";
    }
    ESP_LOGW(TAG, "Fetching the firmware: %lld bytes from %.40s... to %s", static_cast<long long>(length),
             url.c_str(), target->label);
    std::vector<char> buf(4096);
    esp_ota_handle_t handle = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    int64_t got = 0;
    std::string error;
    while (got < length && error.empty()) {
        const int r = esp_http_client_read(client, buf.data(), buf.size());
        if (r <= 0) {
            error = "the download stopped at " + std::to_string(got) + " bytes";
            break;
        }
        if (got == 0) {
            if (static_cast<uint8_t>(buf[0]) != 0xE9) error = "not an app image";
            else if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) error = "cannot write the slot";
            if (!error.empty()) break;
        }
        if (esp_ota_write(handle, buf.data(), r) != ESP_OK) {
            error = "writing flash failed";
            break;
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(buf.data()), r);
        got += r;
    }
    esp_http_client_cleanup(client);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (error.empty() && fetch_sha != hex) error = "the image does not match its hash";
    if (!error.empty()) {
        if (handle != 0) esp_ota_abort(handle);
        return error;
    }
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) return "the image did not validate";
    return "";
}

// Home Assistant's address first, then (for a release) its download on GitHub,
// for a Wi-Fi network that may not reach Home Assistant (an IoT network)
void fetch_task(void*) {
    std::string error;
    for (const std::string* url : {&fetch_url, &fetch_url2}) {
        if (url->empty()) continue;
        error = fetch_from(*url);
        if (error.empty()) fetch_done(url == &fetch_url ? "fetch ok" : "fetch ok (from the internet)");
        ESP_LOGW(TAG, "Fetching from %.48s failed: %s", url->c_str(), error.c_str());
    }
    fetch_done("fetch failed: " + error);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (ap_mode && try_state == "trying") {
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            last_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
            // 15/202/204: the password was refused; 201: not found. Anything
            // else may pass: tried again until the limit
            if (last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || last_reason == WIFI_REASON_AUTH_FAIL ||
                last_reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                end_try("password");
            } else if (last_reason == WIFI_REASON_NO_AP_FOUND) {
                end_try("notfound");
            } else {
                esp_wifi_connect();
            }
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            const bool kept = store("ssid", try_ssid) && store("pass", try_password);
            end_try(kept ? "ok" : "noconnect");
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && !ap_mode) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && !ap_mode) {
        ip_text[0] = '\0';
        last_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_ip4addr_ntoa(&static_cast<ip_event_got_ip_t*>(data)->ip_info.ip, ip_text, sizeof(ip_text));
        ESP_LOGI(TAG, "Wi-Fi connected: http://%s.local/ or http://%s/", hostname(), ip_text);
        if (!http_started) {
            store("result", "");
            start_http();
        }
        static bool fetching = false;
        if (!fetch_url.empty()) {
            // Fetching: nothing else on the radio meanwhile, the beds stay off
            if (!fetching) {
                fetching = true;
                xTaskCreate(fetch_task, "fetch", 8192, nullptr, 5, nullptr);
            }
            return;
        }
        start_app_once();
    }
}

// The page's address on Thread: the one it was opened on, else the name the
// bridge registers there (its 802.15.4 EUI-64), for a bridge set up from the
// access point
std::string home_host() {
    std::string home = stored_text("home");
    uint8_t eui[8] = {};
    if (home.empty() && esp_read_mac(eui, ESP_MAC_IEEE802154) == ESP_OK) {
        char text[24];
        std::snprintf(text, sizeof(text), "%02X%02X%02X%02X%02X%02X%02X%02X.local", eui[0], eui[1], eui[2], eui[3],
                      eui[4], eui[5], eui[6], eui[7]);
        home = text;
    }
    return home;
}

// The app's state plus "config_mode": the settings page shows what still
// works and a configuration banner, and hides what needs Matter
std::string state_json(const Network& network) {
    const std::string app_state = mb_core::app().state_json != nullptr ? mb_core::app().state_json() : "{}";
    wifi_ap_record_t ap = {};
    const bool up = !ap_mode && ip_text[0] != '\0' && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const int64_t now = esp_timer_get_time();
    const int64_t until = std::min(last_activity_us + idle_limit_us, HARD_LIMIT_US);
    std::string out = "{\"config_mode\":{";
    out += "\"ssid\":" + json_text(ap_mode ? std::string(ap_ssid) : network.ssid);
    out += ",\"ap\":" + std::string(ap_mode ? "true" : "false");
    out += ",\"ip\":" + json_text(ip_text);
    out += ",\"host\":" + json_text(std::string(hostname()) + ".local");
    out += ",\"home\":" + json_text(home_host());
    out += ",\"rssi\":" + std::to_string(up ? ap.rssi : 0);
    out += ",\"remaining\":" + std::to_string(until > now ? (until - now) / 1000000 : 0);
    out += ",\"version\":" + json_text(esp_app_get_description()->version);
    out += "}";
    if (app_state.size() > 2) {
        out += "," + app_state.substr(1);  // app_state is "{...}"
    } else {
        out += "}";
    }
    return out;
}

// The pairing code for the page; pairing itself waits for Matter (Thread)
std::string pairing_json() {
    std::string qr;
    std::string manual;
    const bool have = setup_code::codes(qr, manual);
    int size = 0;
    const std::string path = have ? setup_code::qr_svg_path(qr.c_str(), size) : "";
    char ids[48];
    std::snprintf(ids, sizeof(ids), "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\"", CONFIG_DEVICE_VENDOR_ID,
                  CONFIG_DEVICE_PRODUCT_ID);
    return std::string("{\"config_mode\":true,\"product\":\"\",\"version\":") +
           json_text(esp_app_get_description()->version) + "," + ids + ",\"qr\":" + json_text(have ? qr : "") +
           ",\"manual\":" + json_text(have ? manual : "") + ",\"qr_size\":" + std::to_string(size) +
           ",\"qr_path\":" + json_text(path) + ",\"window_open\":false,\"fabrics\":[],\"network\":\"\"}\n";
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
    if (cmd == "system" && args.size() == 1) {  // "system json" goes on to the built-in
        if (ap_mode) {
            return boot_guard::describe() + "configuration mode on the setup access point '" + ap_ssid +
                   "', ip " + ip_text + "\n";
        }
        return boot_guard::describe() + "configuration mode on Wi-Fi '" + network.ssid + "', ip " +
               (ip_text[0] ? ip_text : "-") + "\n";
    }
    if (cmd == "wifi" && args.size() == 2 && args[1] == "exit") {
        mb_core::stop();
        restart_in_background();
        return std::string("back to normal (") + NORMAL + ")\n";
    }
    if (cmd == "wifi" && args.size() >= 2 && args[1] == "config" && !ap_mode) {
        return "already in configuration mode\n";
    }
    if (cmd == "wifi") {
        return command(args);
    }
#if CONFIG_OPENTHREAD_ENABLED
    if (cmd == "thread") {
        return thread_dataset::command(args);
    }
#endif
    if (cmd == "access") {
        return program_access::command(args);
    }
    if (cmd == "matter" && args.size() == 2 && args[1] == "json") {
        return pairing_json();
    }
    if (cmd == "matter" || cmd == "preset" || cmd == "language") {
        return "not available in configuration mode: Matter is off (wifi exit to go back)\n";
    }
    if (cmd == "help") {
#if CONFIG_OPENTHREAD_ENABLED
        return "Configuration mode on Wi-Fi: the connections work, Matter and Thread are off.\n"
               "system | wifi exit (back to normal) | thread dataset <hex> | POST /ota for a firmware update\n";
#else
        return "Setup mode on Wi-Fi: the connections work, Matter is off.\n"
               "system | wifi ssid-hex/password-hex | wifi exit (back to normal) | POST /ota for a firmware update\n";
#endif
    }
    return "";
}

}  // namespace

bool running = false;  // run() was entered

bool active() { return running; }

bool requested() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    uint8_t minutes = 0;
    const bool asked = nvs_get_u8(handle, "boot", &minutes) == ESP_OK && minutes > 0;
    if (asked) {
        uint8_t ap = 0;
        force_ap = nvs_get_u8(handle, "ap", &ap) == ESP_OK && ap != 0;
        // Clear it first: whatever happens in this boot, the next one is normal
        fetch_url = nvs_string(handle, "fetch_url");
        fetch_sha = nvs_string(handle, "fetch_sha");
        fetch_url2 = nvs_string(handle, "fetch_url2");
        nvs_erase_key(handle, "boot");
        nvs_erase_key(handle, "ap");
        nvs_erase_key(handle, "fetch_url");  // once: a failing fetch never repeats by itself
        nvs_erase_key(handle, "fetch_sha");
        nvs_erase_key(handle, "fetch_url2");
        nvs_commit(handle);
        idle_limit_us = static_cast<int64_t>(minutes) * MINUTE_US;
    }
    nvs_close(handle);
    return asked;
}

void run() {
    running = true;
    device_status::set_setup(false);  // joining Wi-Fi, until the setup network opens
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

    // What works without Matter starts once Wi-Fi is up (start_app_once)

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));

    console_start();
    if (network.ssid.empty() || force_ap) {
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        start_ap();
        while (true) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), network.ssid.c_str(), sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), network.password.c_str(), sizeof(config.sta.password));
    // With several access points under one name, the first one found may be
    // a far one where the handshake times out (reason 15): take the strongest
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // keep NVS as it is
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // required while BLE is on

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

// The Wi-Fi networks in range, strongest first, each name once, as JSON for
// the page: choosing one is better than typing it. In configuration mode
// Wi-Fi is up already; on Thread it is started for the scan and stopped
// again (the radio is shared, so Thread pauses for those seconds).
std::string scan_networks() {
    wifi_mode_t mode = WIFI_MODE_NULL;
    const bool temporary = esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT;
    if (temporary) {
        if (esp_get_free_heap_size() < 90 * 1024) {
            return "error: not enough memory to look for Wi-Fi networks now\n";
        }
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&init) != ESP_OK) {
            return "error: Wi-Fi is not available\n";
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() != ESP_OK) {
            esp_wifi_deinit();
            return "error: Wi-Fi did not start\n";
        }
    }
    wifi_scan_config_t config = {};
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 50;
    config.scan_time.active.max = 120;
    const esp_err_t err = esp_wifi_scan_start(&config, true);
    uint16_t count = 24;
    std::vector<wifi_ap_record_t> found(count);
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&count, found.data());
    }
    found.resize(err == ESP_OK ? count : 0);
    if (temporary) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    if (err != ESP_OK) {
        return std::string("error: scanning failed (") + esp_err_to_name(err) + ")\n";
    }
    std::sort(found.begin(), found.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    std::string out = "{\"networks\":[";
    std::vector<std::string> seen;
    for (const wifi_ap_record_t& ap : found) {
        const std::string ssid(reinterpret_cast<const char*>(ap.ssid));
        if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) {
            continue;  // hidden, or the same network from another access point
        }
        seen.push_back(ssid);
        out += (seen.size() > 1 ? "," : "") + std::string("{\"ssid\":") + json_text(ssid) +
               ",\"rssi\":" + std::to_string(ap.rssi) + ",\"open\":" + (ap.authmode == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    return out + "]}\n";
}

// Text from hex: the page sends Wi-Fi names and passwords this way, so
// spaces, quotes and other characters arrive exactly as typed (a command
// line is split into words, which loses repeated or trailing spaces)
bool from_hex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const std::string pair = hex.substr(i, 2);
        const long byte = std::strtol(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) {
            return false;
        }
        out += static_cast<char>(byte);
    }
    return true;
}

std::string command(const std::vector<std::string>& args) {
    std::string value;
    if (args.size() == 2 && args[1] == "scan") {
        return scan_networks();
    }
    if (args.size() == 3 && args[1] == "ssid-hex") {
        if (!from_hex(args[2], value) || value.empty() || value.size() > 32) {
            return "error: the network name must be 1 to 32 bytes\n";
        }
        return store("ssid", value) ? "ok\n" : "storing failed\n";
    }
    if (args.size() == 3 && args[1] == "password-hex") {
        if (!from_hex(args[2], value) || value.size() < 8 || value.size() > 64) {
            return "error: a Wi-Fi password is 8 to 64 characters\n";
        }
        return store("pass", value) ? "ok\n" : "storing failed\n";
    }
    // From the setup network: try it first, keep it only when it works
    if ((args.size() == 3 || args.size() == 4) && args[1] == "try") {
        std::string password;
        if (!from_hex(args[2], value) || value.empty() || value.size() > 32) return "error: the network name must be 1 to 32 bytes\n";
        if (args.size() == 4 && (!from_hex(args[3], password) || password.size() < 8 || password.size() > 64)) {
            return "error: a Wi-Fi password is 8 to 64 characters\n";
        }
        return start_try(value, password);
    }
    // An empty value used to erase the stored one (an empty password field
    // then forgot the network's password); forgetting is "wifi clear" only
    if (args.size() >= 2 && (args[1] == "ssid" || args[1] == "password") && args.size() == 2) {
        return "error: no value given (wifi clear forgets the network)\n";
    }
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
               ",\"ap\":" + (ap_mode ? "true" : "false") + ",\"try\":" + json_text(try_state) +
               ",\"minutes\":" + std::to_string(DEFAULT_MINUTES) + ",\"fetch\":true" +
               ",\"network\":\"" + (has_thread_radio() ? "thread" : "wifi") + "\"" +
               ",\"host\":" + json_text(std::string(hostname()) + ".local") + "}";
    }
    if ((args.size() == 4 || args.size() == 5) && args[1] == "fetch") {
        const std::string& url = args[2];
        const std::string& sha = args[3];
        const std::string url2 = args.size() == 5 ? args[4] : "";
        if (url.rfind("http://", 0) != 0 || url.size() > 200) return "error: fetch <http://...> <sha256> [<https://...>]\n";
        if (!url2.empty() && (url2.rfind("https://", 0) != 0 || url2.size() > 300)) return "error: the second address is https://...\n";
        if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos) return "error: sha256 as 64 lower-case hex digits\n";
        if (network.ssid.empty()) return "error: no Wi-Fi network set\n";
        if (!store("fetch_url", url) || !store("fetch_sha", sha) || !store("fetch_url2", url2) || !request(5)) {
            return "storing failed\n";
        }
        mb_core::stop();
        restart_in_background();
        return "restarting on Wi-Fi '" + network.ssid + "' to fetch the firmware\n";
    }
    if (args.size() >= 2 && args[1] == "config") {
        int minutes = args.size() == 3 ? std::atoi(args[2].c_str()) : DEFAULT_MINUTES;
        if (minutes < 1 || minutes > MAX_MINUTES) {
            return "minutes: 1-" + std::to_string(MAX_MINUTES) + "\n";
        }
        if (!request(minutes)) {
            return "storing failed\n";
        }
        mb_core::stop();
        restart_in_background();
        if (network.ssid.empty()) {
            return "restarting in configuration mode on the setup access point '" + setup_ap_name() +
                   "', back after " + std::to_string(minutes) + " idle minutes: http://192.168.4.1/\n";
        }
        return "restarting in configuration mode on Wi-Fi '" + network.ssid + "', back after " +
               std::to_string(minutes) + " idle minutes: http://" + hostname() + ".local/\n";
    }
    return "wifi: '" + network.ssid + "' (" + network.source + ")" +
           (network.password.empty() ? ", no password" : ", password set") + "\n";
}

bool page_network(std::string& ssid, std::string& password) {
    const Network network = stored_network();
    ssid = network.ssid;
    password = network.password;
    return !ssid.empty();
}

bool store_network(const std::string& ssid, const std::string& password) {
    return store("ssid", ssid) && store("pass", password);
}

void forget_network() {
    store("ssid", "");
    store("pass", "");
#if CONFIG_OPENTHREAD_ENABLED
    thread_dataset::command({"thread", "clear"});
#endif
}

bool request(int minutes, bool setup_ap) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool stored = nvs_set_u8(handle, "boot", static_cast<uint8_t>(minutes)) == ESP_OK &&
                        nvs_set_u8(handle, "ap", setup_ap ? 1 : 0) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return stored;
}

}  // namespace config_mode
