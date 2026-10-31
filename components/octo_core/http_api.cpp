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
#include "nvs.h"

namespace http_api {

namespace {

const char* TAG = "http";
constexpr size_t MAX_COMMAND_LEN = 600;  // "thread dataset <hex>" fits
constexpr size_t OTA_CHUNK = 4096;

constexpr size_t MIN_PASSWORD_LEN = 8;
constexpr size_t MAX_PASSWORD_LEN = 63;
const char* NVS_NAMESPACE = "http";
const char* NVS_KEY = "password";

httpd_handle_t server = nullptr;
char redirect_url[32] = "";  // see redirect_unknown_to()
// The password of the page: chosen on the page and kept in NVS. Empty until
// then; only setting it works without one.
char password[MAX_PASSWORD_LEN + 1] = {};

void load_password(const char* fallback) {
    nvs_handle_t nvs;
    size_t size = sizeof(password);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_KEY, password, &size) != ESP_OK) {
            password[0] = '\0';
        }
        nvs_close(nvs);
    }
    // A token built in by an older firmware keeps working until a new one is set
    if (password[0] == '\0' && fallback != nullptr && std::strcmp(fallback, "change-me") != 0) {
        std::strncpy(password, fallback, MAX_PASSWORD_LEN);
    }
}

bool store_password(const char* value) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = value[0] == '\0' ? nvs_erase_key(nvs, NVS_KEY) : nvs_set_str(nvs, NVS_KEY, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

bool authorized(httpd_req_t* req) {
    if (password[0] == '\0') {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "no password set\n");
        return false;
    }
    char token[MAX_PASSWORD_LEN + 1] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Octo-Token", token, sizeof(token)) == ESP_OK &&
        std::strcmp(token, password) == 0) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "missing or wrong X-Octo-Token\n");
    return false;
}

// Phones probe a fixed address when they join a network; sending them to the
// page makes them show it as a sign-in page
esp_err_t handle_unknown(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t handle_auth_get(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, password[0] != '\0' ? "{\"password\":true}" : "{\"password\":false}");
}

// Body = the new password. The first one needs no token; changing it needs
// the current one.
esp_err_t handle_auth_post(httpd_req_t* req) {
    if (password[0] != '\0' && !authorized(req)) {
        return ESP_OK;
    }
    if (req->content_len < MIN_PASSWORD_LEN || req->content_len > MAX_PASSWORD_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "password must be 8 to 63 characters\n");
    }
    char value[MAX_PASSWORD_LEN + 1] = {};
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, value + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    for (size_t i = 0; i < got; ++i) {
        if (static_cast<unsigned char>(value[i]) < 0x20 || value[i] == 0x7f) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "password must be printable characters\n");
        }
    }
    if (!store_password(value)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "cannot store the password\n");
    }
    std::strcpy(password, value);
    ESP_LOGI(TAG, "Password of the page set");
    return httpd_resp_sendstr(req, "OK\n");
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

void start(const char* fallback_token) {
    if (server != nullptr) {
        return;
    }
    load_password(fallback_token);
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

    httpd_uri_t auth_get = {};
    auth_get.uri = "/auth";
    auth_get.method = HTTP_GET;
    auth_get.handler = handle_auth_get;
    httpd_register_uri_handler(server, &auth_get);

    httpd_uri_t auth_post = {};
    auth_post.uri = "/auth";
    auth_post.method = HTTP_POST;
    auth_post.handler = handle_auth_post;
    httpd_register_uri_handler(server, &auth_post);

    httpd_uri_t ota = {};
    ota.uri = "/ota";
    ota.method = HTTP_POST;
    ota.handler = handle_ota;
    httpd_register_uri_handler(server, &ota);

    if (password[0] == '\0') {
        ESP_LOGW(TAG, "No password yet: choose one on the settings page");
    }
    ESP_LOGI(TAG, "HTTP API running on port 80");
    boot_guard::network_ok();
}

std::string reset_password() {
    if (!store_password("")) {
        return "cannot clear the password\n";
    }
    password[0] = '\0';
    return "password cleared: choose a new one on the settings page\n";
}

void redirect_unknown_to(const char* url) {
    if (server == nullptr) {
        return;
    }
    std::strncpy(redirect_url, url, sizeof(redirect_url) - 1);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_unknown);
}

}  // namespace http_api
