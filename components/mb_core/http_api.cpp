#include "http_api.h"

#include "sdkconfig.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "boot_guard.h"
#include "cJSON.h"
#include "commands.h"
#include "device_status.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mb_core.h"
#include "nvs.h"
#include "program_access.h"

namespace http_api {

namespace {

const char* TAG = "http";
constexpr size_t MAX_COMMAND_LEN = 600;  // "thread dataset <hex>" fits
constexpr size_t OTA_CHUNK = 4096;

constexpr size_t MIN_PASSWORD_LEN = 8;
constexpr size_t MAX_PASSWORD_LEN = 63;
const char* NVS_NAMESPACE = "http";
const char* NVS_KEY = "password";

// The PIN: quick access for looking and operating (a viewer), never for
// changing; the password stays needed for that. 4 to 6 digits, kept in NVS.
const char* NVS_PIN_KEY = "pin";
char pin[7] = {};
// Guessing: after 5 wrong PINs it locks for a minute, each next lock twice
// as long (at most 15 minutes); a right PIN starts the count again
int pin_failures = 0;
int64_t pin_locked_until_us = 0;
int64_t pin_lock_us = 60 * 1000000LL;

void reset_pin_limit() {
    pin_failures = 0;
    pin_locked_until_us = 0;
    pin_lock_us = 60 * 1000000LL;
}

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
    size = sizeof(pin);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_PIN_KEY, pin, &size) != ESP_OK) pin[0] = '\0';
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

bool store_pin(const char* value) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = value[0] == '\0' ? nvs_erase_key(nvs, NVS_PIN_KEY) : nvs_set_str(nvs, NVS_PIN_KEY, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

enum class Who { nobody, owner, program, viewer, locked };

// The page password is the owner; a linked program has a token of its own
Who who(httpd_req_t* req) {
    char token[MAX_PASSWORD_LEN + 1] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Octo-Token", token, sizeof(token)) != ESP_OK) {
        char given[7] = {};
        if (password[0] == '\0' || pin[0] == '\0' ||
            httpd_req_get_hdr_value_str(req, "X-Octo-Pin", given, sizeof(given)) != ESP_OK) {
            return Who::nobody;
        }
        const int64_t now = esp_timer_get_time();
        if (now < pin_locked_until_us) return Who::locked;
        if (std::strcmp(given, pin) == 0) {
            reset_pin_limit();
            return Who::viewer;
        }
        if (++pin_failures >= 5) {
            pin_failures = 0;
            pin_locked_until_us = now + pin_lock_us;
            ESP_LOGW(TAG, "Wrong PIN 5 times: locked for %lld s", pin_lock_us / 1000000);
            pin_lock_us = pin_lock_us * 2 > 900 * 1000000LL ? 900 * 1000000LL : pin_lock_us * 2;
            return Who::locked;
        }
        return Who::nobody;
    }
    if (password[0] != '\0' && std::strcmp(token, password) == 0) {
        return Who::owner;
    }
    return program_access::token_valid(token) ? Who::program : Who::nobody;
}

// Sends the 401 itself when it returns false. Changing the password and
// linking programs need the password itself.
bool authorized(httpd_req_t* req, bool owner_only = false) {
    const Who caller = who(req);
    if (caller == Who::owner || (caller == Who::program && !owner_only)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, password[0] == '\0' && caller == Who::nobody ? "no password set\n"
                                                                         : "missing or wrong X-Octo-Token\n");
    return false;
}

// Reads the whole body into `out` (at most `max` bytes)
bool read_body(httpd_req_t* req, char* out, size_t max) {
    if (req->content_len > max) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, out + got, req->content_len - got);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    out[got] = '\0';
    return true;
}

// Phones probe a fixed address when they join a network; sending them to the
// page makes them show it as a sign-in page
esp_err_t handle_unknown(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    return httpd_resp_send(req, nullptr, 0);
}

// Who this is, for the Home Assistant add-on looking for devices it can
// manage: no names or settings, those need the password
esp_err_t handle_info(httpd_req_t* req) {
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BASE);
    char out[256];
    // The chip and the network it runs on: the add-on picks updates by them
    // and sends them straight over Wi-Fi to a Wi-Fi device
    std::snprintf(out, sizeof(out),
                  "{\"kind\":\"matter-builder\",\"app\":\"%s\",\"version\":\"%s\",\"id\":\"%02X%02X%02X%02X%02X%02X\","
                  "\"spec\":%s,\"chip\":\"%s\",\"network\":\"%s\",\"slot\":%lu}",
                  app->project_name, app->version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  mb_core::app().spec_start != nullptr ? "true" : "false", CONFIG_IDF_TARGET,
#if CONFIG_OPENTHREAD_ENABLED
                  "thread",
#else
                  "wifi",
#endif
                  // The update slot's size: the add-on refuses a larger image before sending it
                  static_cast<unsigned long>(slot != nullptr ? slot->size : 0));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// The spec the firmware was generated from (owner or a linked program)
esp_err_t handle_spec(httpd_req_t* req) {
    if (!authorized(req)) {
        return ESP_OK;
    }
    const mb_core::App& app = mb_core::app();
    if (app.spec_start == nullptr) {
        return httpd_resp_send_404(req);
    }
    httpd_resp_set_type(req, "text/yaml; charset=utf-8");
    return httpd_resp_send(req, app.spec_start, app.spec_end - app.spec_start);
}

esp_err_t handle_auth_get(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    char out[64];
    // pin: its length (the page shows a box per digit), 0 when there is none
    std::snprintf(out, sizeof(out), "{\"password\":%s,\"pin\":%d}", password[0] != '\0' ? "true" : "false",
                  password[0] != '\0' ? static_cast<int>(std::strlen(pin)) : 0);
    return httpd_resp_sendstr(req, out);
}

// Body = the new password. The first one needs no token; changing it needs
// the current one.
esp_err_t handle_auth_post(httpd_req_t* req) {
    if (password[0] != '\0' && !authorized(req, true)) {
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

// What a viewer (the PIN) may send: looking, and operating what is
// connected; nothing that changes settings, access, the network or firmware
enum class ViewerReply { forbidden, text, masked_json };

ViewerReply viewer_reply(const std::vector<std::string>& args) {
    if (args.empty()) return ViewerReply::forbidden;
    const auto& command = args[0];
    if ((args.size() == 1 && command == "json") ||
        (args.size() == 2 && args[1] == "json" &&
         (command == "system" || command == "matter" || command == "led" || command == "rule" || command == "bledev")) ||
        (args.size() == 2 && command == "preset" && args[1] == "list")) {
        return ViewerReply::masked_json;
    }
    if (args.size() == 1 && command == "language") return ViewerReply::text;
    if (args.size() == 4 && command == "io" && args[1] == "test" &&
        args[2].size() == 1 && args[2][0] >= '0' && args[2][0] <= '7' &&
        (args[3] == "on" || args[3] == "off")) {
        return ViewerReply::text;
    }
    if (args.size() < 2 || (args[1] != "0" && args[1] != "1")) return ViewerReply::forbidden;
    if ((args.size() == 2 && command == "stop") ||
        (args.size() == 3 && command == "light" && (args[2] == "on" || args[2] == "off")) ||
        (args.size() == 3 && command == "preset" &&
         (args[2] == "flat" || args[2] == "match" || args[2] == "1" || args[2] == "2" || args[2] == "3"))) {
        return ViewerReply::text;
    }
    if (args.size() == 5 && command == "move" &&
        (args[2] == "head" || args[2] == "feet" || args[2] == "legs" || args[2] == "both") &&
        (args[3] == "up" || args[3] == "down")) {
        char* end = nullptr;
        const float seconds = std::strtof(args[4].c_str(), &end);
        if (*end == '\0' && std::isfinite(seconds) && seconds >= 0.3f) return ViewerReply::text;
    }
    return ViewerReply::forbidden;
}

// What a viewer gets left out: addresses, network names, pairing and sharing
// codes. The page shows these masked; the password reveals them.
const char* const HIDDEN[] = {"mac", "ip", "ssid", "qr", "qr_path", "qr_size", "manual", "share", "network", "dataset",
                              "passcode", "discriminator", "token", "hostname", "address", "parent", "neighbors"};
void hide(cJSON* node) {
    for (cJSON* child = node->child; child != nullptr;) {
        cJSON* next = child->next;
        bool hidden = false;
        if (cJSON_IsObject(node)) {
            for (const char* key : HIDDEN) {
                if (std::strcmp(child->string, key) != 0) continue;
                // Keep the network kind reported by system json.
                hidden = std::strcmp(key, "network") != 0 || !cJSON_IsString(child) ||
                         (std::strcmp(child->valuestring, "wifi") != 0 && std::strcmp(child->valuestring, "thread") != 0);
                break;
            }
        }
        if (hidden) {
            cJSON_Delete(cJSON_DetachItemViaPointer(node, child));
        } else {
            hide(child);
        }
        child = next;
    }
}

esp_err_t send_viewer_json(httpd_req_t* req, const std::string& reply) {
    cJSON* root = cJSON_ParseWithOpts(reply.c_str(), nullptr, true);
    char* text = nullptr;
    if (cJSON_IsObject(root)) {
        hide(root);
        text = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (text == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "masked response unavailable\n");
    }
    const esp_err_t result = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return result;
}

// "pin json" | "pin set <4-6 digits>" | "pin clear": only with the password
std::string pin_command(const char* line) {
    if (std::strcmp(line, "pin json") == 0) return pin[0] != '\0' ? "{\"set\":true}\n" : "{\"set\":false}\n";
    if (std::strcmp(line, "pin clear") == 0) {
        if (!store_pin("")) return "storing failed\n";
        pin[0] = '\0';
        reset_pin_limit();
        return "ok\n";
    }
    if (std::strncmp(line, "pin set ", 8) == 0) {
        const char* value = line + 8;
        const size_t len = std::strlen(value);
        bool digits = len >= 4 && len <= 6;
        for (size_t i = 0; digits && i < len; ++i) digits = value[i] >= '0' && value[i] <= '9';
        if (!digits) return "error: a PIN is 4 to 6 digits\n";
        if (!store_pin(value)) return "storing failed\n";
        std::strcpy(pin, value);
        reset_pin_limit();
        return "ok\n";
    }
    return "pin json | pin set <4-6 digits> | pin clear\n";
}

esp_err_t handle_cmd(httpd_req_t* req) {
    const Who caller = who(req);
    if (caller == Who::locked) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "PIN locked for a while after wrong tries: try again later or use the password\n");
    }
    if (caller == Who::nobody) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, password[0] == '\0' ? "no password set\n" : "missing or wrong X-Octo-Token\n");
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
    if (std::memchr(line, '\0', got) != nullptr) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body must be one command line\n");
    }
    const auto args = parse_command(line);
    // Which programs have access is for the owner to change
    if (caller == Who::program && !args.empty() && args[0] == "access" &&
        !(args.size() == 2 && args[1] == "json")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "only with the page password\n");
    }
    httpd_resp_set_type(req, "text/plain");
    if (std::strncmp(line, "pin", 3) == 0 && (line[3] == ' ' || line[3] == '\0')) {
        if (caller != Who::owner) {
            httpd_resp_set_status(req, "403 Forbidden");
            return httpd_resp_sendstr(req, "only with the page password\n");
        }
        return httpd_resp_sendstr(req, pin_command(line).c_str());
    }
    if (caller == Who::viewer) {
        const ViewerReply mode = viewer_reply(args);
        if (mode == ViewerReply::forbidden) {
            httpd_resp_set_status(req, "403 Forbidden");
            return httpd_resp_sendstr(req, "the password is needed for this\n");
        }
        const std::string reply = run_command(args);
        return mode == ViewerReply::masked_json ? send_viewer_json(req, reply) : httpd_resp_sendstr(req, reply.c_str());
    }
    return httpd_resp_sendstr(req, run_command(args).c_str());
}

// Body = the program's name; the reply's id is polled with GET /link
esp_err_t handle_link_post(httpd_req_t* req) {
    if (!authorized(req, true)) {
        return ESP_OK;
    }
    char name[32] = {};
    if (!read_body(req, name, sizeof(name) - 1)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body must be the program's name\n");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, program_access::link_request(name).c_str());
}

esp_err_t handle_link_get(httpd_req_t* req) {
    if (!authorized(req, true)) {
        return ESP_OK;
    }
    char id[20] = {};
    httpd_req_get_hdr_value_str(req, "X-Octo-Link", id, sizeof(id));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, program_access::link_poll(id).c_str());
}

// The settings page comes from the app (mb_core::App). It holds no
// secrets: everything it does goes through /cmd with the token.

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
    const mb_core::App& app = mb_core::app();
    if (app.page_gz_start == nullptr) {
        return httpd_resp_send_404(req);
    }
    return httpd_resp_send(req, app.page_gz_start, app.page_gz_end - app.page_gz_start);
}

void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    device_status::note_restart("firmware update");
    esp_restart();
}

// `stopped`: the app was stopped for the upload (mb_core::stop); a restart
// brings it back as it was, instead of leaving it half stopped
esp_err_t ota_fail(httpd_req_t* req, esp_ota_handle_t handle, const char* why, bool stopped = false) {
    if (handle != 0) {
        esp_ota_abort(handle);
    }
    ESP_LOGE(TAG, "OTA failed: %s", why);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, why);
    if (stopped) {
        xTaskCreate(restart_task, "restart", 2048, nullptr, 5, nullptr);
    }
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
    mb_core::stop();
    ESP_LOGW(TAG, "OTA: writing %u bytes to %s", static_cast<unsigned>(req->content_len), target->label);

    std::unique_ptr<char[]> buf(new char[OTA_CHUNK]);
    esp_ota_handle_t handle = 0;
    size_t received = 0;
    int silent = 0;  // receive timeouts in a row (15 s each)
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf.get(), OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            // A sender that went away without closing would keep this
            // waiting for ever: two minutes of silence ends the upload
            if (++silent >= 8) {
                return ota_fail(req, handle, "upload stalled: nothing for two minutes\n", true);
            }
            continue;
        }
        silent = 0;
        if (r <= 0) {
            return ota_fail(req, handle, "upload interrupted\n", true);
        }
        if (received == 0) {
            if (static_cast<uint8_t>(buf[0]) != 0xE9) {
                return ota_fail(req, 0, "not an ESP32 app image (use the OTA/app .bin, not a factory image)\n", true);
            }
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
                return ota_fail(req, 0, "cannot start writing the firmware slot\n", true);
            }
        }
        if (esp_ota_write(handle, buf.get(), r) != ESP_OK) {
            return ota_fail(req, handle, "writing flash failed\n", true);
        }
        received += r;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        return ota_fail(req, 0, "image did not validate\n", true);
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return ota_fail(req, 0, "cannot select the new image\n", true);
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
    // Every command of the page and the add-on runs on this task. Measured on
    // the bed bridge: of 6144 bytes, 208 were left at the deepest. The classic
    // ESP32 has less memory to give.
#if CONFIG_IDF_TARGET_ESP32
    config.stack_size = 8192;
#else
    config.stack_size = 10240;
#endif
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    // Browsers keep several connections open; close the oldest idle one
    // instead of letting new requests wait for a free slot
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;  // 9 in use: the default (8) would be full
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

    httpd_uri_t info = {};
    info.uri = "/info";
    info.method = HTTP_GET;
    info.handler = handle_info;
    httpd_register_uri_handler(server, &info);

    httpd_uri_t spec = {};
    spec.uri = "/spec";
    spec.method = HTTP_GET;
    spec.handler = handle_spec;
    httpd_register_uri_handler(server, &spec);

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

    httpd_uri_t link_post = {};
    link_post.uri = "/link";
    link_post.method = HTTP_POST;
    link_post.handler = handle_link_post;
    httpd_register_uri_handler(server, &link_post);

    httpd_uri_t link_get = {};
    link_get.uri = "/link";
    link_get.method = HTTP_GET;
    link_get.handler = handle_link_get;
    httpd_register_uri_handler(server, &link_get);

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
