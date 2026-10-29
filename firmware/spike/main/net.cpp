#include "net.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mdns.h"
#include "secrets.h"

namespace net {

namespace {

const char* TAG = "net";
constexpr const char* HOSTNAME = "octo-matter";

esp_netif_t* netif = nullptr;
char ip_text[16] = "-";
int wifi_disconnects = 0;

void start_server() {
    http_api::start(OCTO_HTTP_TOKEN);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    }
    ESP_LOGI(TAG, "HTTP console on http://%s/ and http://%s.local/", ip_text, HOSTNAME);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_disconnects++;
        std::strcpy(ip_text, "-");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_text, sizeof(ip_text));
        ESP_LOGI(TAG, "Wi-Fi connected, IP %s", ip_text);
        start_server();
    }
}

}  // namespace

void start() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), OCTO_WIFI_SSID, sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), OCTO_WIFI_PASSWORD, sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep is required while BLE is active
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
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
