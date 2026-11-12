// Network side of a Wi-Fi device (the classic ESP32, the S3 and C3, and the
// ESP32-C6 when Wi-Fi was chosen over Thread). The Matter stack owns Wi-Fi;
// this hands it the network set on the settings page and starts the HTTP API
// once there is an address.
//
// Where the network comes from:
// - the settings page (on the setup access point of a new device), or
// - pairing over Bluetooth: the controller gives the network with the pairing.
// Both end up in the same place: a network that came with pairing is also
// stored as the page's network (config_mode::store_network), so a restart,
// configuration mode and the page all show it; one set on the page is used
// from the next start.
//
// The page answers at http://<hostname>.local/ (mb_core::App) as on Thread,
// a name of its own next to the one Matter announces: Matter owns mDNS on
// Wi-Fi and its host name, so the page's name is a delegated host there.
//
// No network at all, and not waiting to be paired over Bluetooth: the device
// restarts on its setup access point. A network that does not connect within
// three minutes: the same (configuration mode tries it once more first).

#include "net.h"

#include <cstring>
#include <string>

#include <platform/CHIPDeviceLayer.h>

#include "config_mode.h"
#include "device_status.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "http_api.h"
#include "mb_core.h"
#include "mdns.h"

namespace net {

namespace {

const char* TAG = "net";

constexpr int64_t CONNECT_LIMIT_US = 3 * 60 * 1000000LL;
constexpr int FALLBACK_MINUTES = 15;

char ip_text[16] = "-";
volatile bool connected = false;
int disconnects = 0;
int last_reason = 0;

void to_setup_mode(const char* why) {
    if (config_mode::request(FALLBACK_MINUTES)) {
        ESP_LOGW(TAG, "%s: restarting in setup mode", why);
        device_status::note_restart(why);
        esp_restart();
    }
    ESP_LOGE(TAG, "%s, and setup mode could not be requested", why);
}

// Waiting for a phone to pair it over Bluetooth, which also brings the network
bool waiting_for_ble_pairing() {
    const auto pairing = mb_core::app().ble_pairing;
    return pairing != nullptr && pairing() && !chip::DeviceLayer::ConnectivityMgr().IsWiFiStationProvisioned();
}

std::string text(const uint8_t* bytes, size_t size) {
    return std::string(reinterpret_cast<const char*>(bytes), strnlen(reinterpret_cast<const char*>(bytes), size));
}

// The network in use becomes the page's network when the page has none (it
// came with pairing over Bluetooth). One set on the page is never
// overwritten here: it may be a new one, used from the next start.
void keep_network() {
    wifi_config_t current = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) != ESP_OK) {
        return;
    }
    const std::string ssid = text(current.sta.ssid, sizeof(current.sta.ssid));
    const std::string password = text(current.sta.password, sizeof(current.sta.password));
    std::string page_ssid;
    std::string page_password;
    config_mode::page_network(page_ssid, page_password);
    if (!ssid.empty() && page_ssid.empty()) {
        config_mode::store_network(ssid, password);
        ESP_LOGI(TAG, "Wi-Fi '%s' kept as the page's network", ssid.c_str());
    }
}

// <hostname>.local for the page, once Matter has started mDNS (it fails to
// start when someone else did it first, so this never starts it): retried
// every few seconds, and on a new address moved to it
esp_ip4_addr_t page_ip = {};
esp_timer_handle_t name_timer = nullptr;
int name_tries = 0;

void on_name_timer(void*) {
    mdns_ip_addr_t address = {};
    address.addr.type = ESP_IPADDR_TYPE_V4;
    address.addr.u_addr.ip4 = page_ip;
    const char* name = mb_core::app().hostname;
    esp_err_t err = mdns_delegate_hostname_set_address(name, &address);
    if (err != ESP_OK) {
        err = mdns_delegate_hostname_add(name, &address);
        if (err == ESP_OK) {
            mdns_service_add_for_host(mb_core::app().product_name, "_http", "_tcp", name, 80, nullptr, 0);
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Settings page: http://%s.local/ or http://%s/", name, ip_text);
    } else if (++name_tries < 24) {
        esp_timer_start_once(name_timer, 5 * 1000000LL);
    }
}

void announce_page(const esp_ip4_addr_t& ip) {
    page_ip = ip;
    name_tries = 0;
    if (name_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = on_name_timer;
        args.name = "page_name";
        if (esp_timer_create(&args, &name_timer) != ESP_OK) return;
    }
    esp_timer_stop(name_timer);
    esp_timer_start_once(name_timer, 3 * 1000000LL);
}

void got_ip(const esp_netif_ip_info_t& info) {
    esp_ip4addr_ntoa(&info.ip, ip_text, sizeof(ip_text));
    connected = true;
    ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip_text);
    keep_network();
    http_api::start(mb_core::app().http_token_fallback);
    announce_page(info.ip);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (connected) {
            disconnects++;
        }
        connected = false;
        last_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        std::strcpy(ip_text, "-");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        got_ip(static_cast<ip_event_got_ip_t*>(data)->ip_info);
    }
}

void on_connect_limit(void*) {
    if (!connected && !waiting_for_ble_pairing()) {
        to_setup_mode("No Wi-Fi connection after 3 minutes");
    }
}

// The page's network, when it differs from the one Matter has (a new device
// set up on its access point, or a network changed on the page)
void apply_page_network(intptr_t) {
    std::string ssid;
    std::string password;
    wifi_config_t current = {};
    esp_wifi_get_config(WIFI_IF_STA, &current);
    const std::string current_ssid = text(current.sta.ssid, sizeof(current.sta.ssid));
    if (!config_mode::page_network(ssid, password)) {
        if (current_ssid.empty() && waiting_for_ble_pairing()) {
            ESP_LOGW(TAG, "No Wi-Fi network yet: waiting to be paired over Bluetooth");
        } else if (current_ssid.empty()) {
            to_setup_mode("No Wi-Fi network set");
        }
        return;
    }
    if (ssid == current_ssid && password == text(current.sta.password, sizeof(current.sta.password))) {
        return;
    }
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid.c_str(), sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password.c_str(), sizeof(config.sta.password));
    // Several access points under one name: the strongest, not the first found
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not store the Wi-Fi network");
        return;
    }
    ESP_LOGI(TAG, "Joining Wi-Fi '%s' from the settings page", ssid.c_str());
    auto& connectivity = chip::DeviceLayer::ConnectivityMgr();
    connectivity.SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Disabled);
    connectivity.SetWiFiStationMode(chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Enabled);
}

}  // namespace

// Call after the Matter stack has started: it has set up Wi-Fi
void start() {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));
    // Modem sleep: required while Bluetooth is on, and it saves power
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(apply_page_network, 0);

    // Connected before the handler was registered?
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        got_ip(info);
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_connect_limit;
    timer_args.name = "connect_limit";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, CONNECT_LIMIT_US);
    }
}

bool up() { return connected; }

std::string describe() {
    wifi_ap_record_t ap = {};
    const bool up = connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    std::string out = std::string("wifi: ") + (up ? "connected to '" + text(ap.ssid, sizeof(ap.ssid)) + "'" : "not connected") +
                      ", ip " + ip_text;
    if (up) {
        out += ", rssi " + std::to_string(ap.rssi);
    } else if (last_reason != 0) {
        out += ", last reason " + std::to_string(last_reason);
    }
    out += ", disconnects " + std::to_string(disconnects) + "\n";
    if (up) {
        out += std::string("  settings page: http://") + mb_core::app().hostname + ".local/ or http://" + ip_text + "/\n";
    }
    return out;
}

std::string health_json() {
    wifi_ap_record_t ap = {};
    const bool up = connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    std::string out = std::string("{\"kind\":\"wifi\",\"attached\":") + (up ? "true" : "false") +
                      ",\"disconnects\":" + std::to_string(disconnects);
    if (up) {
        out += ",\"rssi\":" + std::to_string(ap.rssi) + ",\"channel\":" + std::to_string(ap.primary);
    } else if (last_reason != 0) {
        out += ",\"last_reason\":" + std::to_string(last_reason);
    }
    return out + "}";
}

}  // namespace net
