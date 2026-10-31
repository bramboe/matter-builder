// Network side of the bridge. The Matter stack owns Wi-Fi; this only hands
// it the credentials from secrets.h and starts the HTTP API once there is an
// IP address. Matter is commissioned over this network, not over BLE.

#include "net.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "http_api.h"
#include "secrets.h"

#ifndef OCTO_HTTP_TOKEN
#define OCTO_HTTP_TOKEN ""  // the password is chosen on the settings page
#endif

namespace net {

namespace {

const char* TAG = "net";

char ip_text[16] = "-";
int wifi_disconnects = 0;

void got_ip(const esp_netif_ip_info_t& info) {
    esp_ip4addr_ntoa(&info.ip, ip_text, sizeof(ip_text));
    ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip_text);
    http_api::start(OCTO_HTTP_TOKEN);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_disconnects++;
        std::strcpy(ip_text, "-");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        got_ip(static_cast<ip_event_got_ip_t*>(data)->ip_info);
    }
}

}  // namespace

// Call after matter_bridge::start(): the Matter stack has set up Wi-Fi.
void start() {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));

    wifi_config_t current = {};
    esp_wifi_get_config(WIFI_IF_STA, &current);
    if (std::strcmp(reinterpret_cast<char*>(current.sta.ssid), OCTO_WIFI_SSID) != 0 ||
        std::strcmp(reinterpret_cast<char*>(current.sta.password), OCTO_WIFI_PASSWORD) != 0) {
        wifi_config_t config = {};
        std::strncpy(reinterpret_cast<char*>(config.sta.ssid), OCTO_WIFI_SSID, sizeof(config.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(config.sta.password), OCTO_WIFI_PASSWORD, sizeof(config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
        ESP_LOGI(TAG, "Stored Wi-Fi credentials from secrets.h");
        esp_wifi_connect();
    }
    // Modem sleep is required while BLE is active
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // Already connected before the handler was registered?
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        got_ip(info);
    }
}

std::string describe() {
    wifi_ap_record_t ap = {};
    const bool up = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    std::string out = std::string("wifi: ") + (up ? "connected" : "not connected") + ", ip " + ip_text;
    if (up) {
        out += ", rssi " + std::to_string(ap.rssi);
    }
    out += ", disconnects " + std::to_string(wifi_disconnects) + "\n";
    return out;
}

}  // namespace net
