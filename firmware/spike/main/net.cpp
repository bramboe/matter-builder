#include "net.h"

#include <cstring>
#include <memory>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "secrets.h"

namespace net {

namespace {

const char* TAG = "net";
constexpr const char* HOSTNAME = "octo-matter";
constexpr size_t MAX_COMMAND_LEN = 256;
constexpr size_t OTA_CHUNK = 4096;

esp_netif_t* netif = nullptr;
httpd_handle_t server = nullptr;
char ip_text[16] = "-";
int wifi_disconnects = 0;

bool authorized(httpd_req_t* req) {
    char token[64] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Octo-Token", token, sizeof(token)) == ESP_OK &&
        std::strcmp(token, OCTO_HTTP_TOKEN) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "missing or wrong X-Octo-Token\n");
    return false;
}

esp_err_t handle_cmd(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > MAX_COMMAND_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body must be one command line\n");
    }
    char line[MAX_COMMAND_LEN + 1] = {};
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, line + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, run_command(line).c_str());
}

void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t ota_fail(httpd_req_t* req, esp_ota_handle_t handle, const char* why) {
    if (handle != 0) {
        esp_ota_abort(handle);
    }
    ESP_LOGE(TAG, "OTA failed: %s", why);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, why);
    return ESP_OK;
}

esp_err_t handle_ota(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        return ota_fail(req, 0, "no firmware slot to write\n");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return ota_fail(req, 0, "image empty or larger than the firmware slot\n");
    }
    bed_link::run_in_host([] { bed_link::stop_all(); });
    ESP_LOGW(TAG, "OTA: writing %u bytes to %s", static_cast<unsigned>(req->content_len), target->label);

    std::unique_ptr<char[]> buf(new char[OTA_CHUNK]);
    esp_ota_handle_t handle = 0;
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf.get(), OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ota_fail(req, handle, "upload interrupted\n");
        }
        if (received == 0) {
            if (static_cast<uint8_t>(buf[0]) != 0xE9) {
                return ota_fail(req, 0, "not an ESP32 app image (use the OTA/app .bin, not a factory image)\n");
            }
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
                return ota_fail(req, 0, "cannot start writing the firmware slot\n");
            }
        }
        if (esp_ota_write(handle, buf.get(), r) != ESP_OK) {
            return ota_fail(req, handle, "writing flash failed\n");
        }
        received += r;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        return ota_fail(req, 0, "image did not validate\n");
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return ota_fail(req, 0, "cannot select the new image\n");
    }
    httpd_resp_sendstr(req, "OK, rebooting into the new firmware\n");
    xTaskCreate(restart_task, "restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

void start_server() {
    if (server != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.max_open_sockets = 3;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    httpd_uri_t cmd = {};
    cmd.uri = "/cmd";
    cmd.method = HTTP_POST;
    cmd.handler = handle_cmd;
    httpd_register_uri_handler(server, &cmd);

    httpd_uri_t ota = {};
    ota.uri = "/ota";
    ota.method = HTTP_POST;
    ota.handler = handle_ota;
    httpd_register_uri_handler(server, &ota);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    }
    ESP_LOGI(TAG, "HTTP console on http://%s/ and http://%s.local/", ip_text, HOSTNAME);
    boot_guard::network_ok();
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
