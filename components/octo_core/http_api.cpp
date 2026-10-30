#include "http_api.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace http_api {

namespace {

const char* TAG = "http";
constexpr size_t MAX_COMMAND_LEN = 256;
constexpr size_t OTA_CHUNK = 4096;

httpd_handle_t server = nullptr;
const char* expected_token = "";

bool authorized(httpd_req_t* req) {
    char token[64] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Octo-Token", token, sizeof(token)) == ESP_OK &&
        std::strcmp(token, expected_token) == 0) {
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

// The settings page (web/index.html, embedded at build time). It holds no
// secrets: everything it does goes through /cmd with the token.
extern const char index_html_start[] asm("_binary_index_html_gz_start");
extern const char index_html_end[] asm("_binary_index_html_gz_end");

// The page changes only with the firmware, so the firmware's hash is its
// ETag: a browser that has it gets "304 Not Modified" instead of the page.
const char* page_etag() {
    static char etag[20];
    if (etag[0] == '\0') {
        const uint8_t* sha = esp_app_get_description()->app_elf_sha256;
        std::snprintf(etag, sizeof(etag), "\"%02x%02x%02x%02x%02x%02x\"", sha[0], sha[1], sha[2], sha[3], sha[4], sha[5]);
    }
    return etag;
}

esp_err_t handle_index(httpd_req_t* req) {
    char seen[24] = {};
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", page_etag());
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", seen, sizeof(seen)) == ESP_OK &&
        std::strcmp(seen, page_etag()) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
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

}  // namespace

void start(const char* token) {
    if (server != nullptr) {
        return;
    }
    expected_token = token;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    // Browsers keep several connections open; close the oldest idle one
    // instead of letting new requests wait for a free slot
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    httpd_uri_t index = {};
    index.uri = "/";
    index.method = HTTP_GET;
    index.handler = handle_index;
    httpd_register_uri_handler(server, &index);

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

    ESP_LOGI(TAG, "HTTP API running on port 80");
    boot_guard::network_ok();
}

}  // namespace http_api
