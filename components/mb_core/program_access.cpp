#include "config_mode.h"
#include "device_status.h"
#include "matter_info.h"
#include "mb_core.h"
#include "program_access.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace program_access {

namespace {

const char* TAG = "access";
const char* NVS_NAMESPACE = "access";

constexpr int MAX_CLIENTS = 8;
constexpr size_t NAME_LEN = 24;
constexpr size_t TOKEN_BYTES = 24;  // 48 hex characters
constexpr int64_t SECOND_US = 1000000;
constexpr int64_t WINDOW_US = 120 * SECOND_US;
constexpr int POWER_ONS = 3;            // a press of the BOOT button (linking a program)
constexpr int RECOVERY_POWER_ONS = 6;   // BOOT held 5-10 s: recovery, for a device out of reach (in a wall box)
constexpr int64_t POWER_ON_LIMIT_US = 10 * SECOND_US;

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3
constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_0;
#elif CONFIG_IDF_TARGET_ESP32C5
constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_28;
#else
constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_9;  // C6, C3, H2
#endif

struct Client {
    char name[NAME_LEN];
    uint8_t hash[32];
};

struct Request {
    bool active = false;
    bool linked = false;
    char id[17] = {};
    char name[NAME_LEN] = {};
    char token[2 * TOKEN_BYTES + 1] = {};
    int64_t asked_us = 0;
};

std::mutex lock;
Client clients[MAX_CLIENTS] = {};
int client_count = 0;
Request request;
int64_t presence_us = -1;  // last proof of presence, -1 for none
void (*on_recovery)() = nullptr;

void random_hex(char* out, size_t bytes) {
    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, bytes);
    for (size_t i = 0; i < bytes; ++i) {
        std::snprintf(out + 2 * i, 3, "%02x", raw[i]);
    }
}

void hash_token(const char* token, uint8_t out[32]) {
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(token), std::strlen(token), out, 0);
}

void load_clients() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t size = sizeof(clients);
    if (nvs_get_blob(nvs, "clients", clients, &size) == ESP_OK) {
        client_count = static_cast<int>(size / sizeof(Client));
    }
    nvs_close(nvs);
}

bool store_clients() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = client_count == 0 ? nvs_erase_key(nvs, "clients")
                                      : nvs_set_blob(nvs, "clients", clients, client_count * sizeof(Client));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

// With the lock held: issue the token for the open request
void grant() {
    random_hex(request.token, TOKEN_BYTES);
    if (client_count == MAX_CLIENTS) {
        std::memmove(clients, clients + 1, (MAX_CLIENTS - 1) * sizeof(Client));  // the oldest goes
        --client_count;
    }
    Client& client = clients[client_count++];
    std::memset(&client, 0, sizeof(client));
    std::strncpy(client.name, request.name, NAME_LEN - 1);
    hash_token(request.token, client.hash);
    store_clients();
    request.linked = true;
    presence_us = -1;  // one proof, one link
    ESP_LOGW(TAG, "Linked '%s'", request.name);
}

bool request_open(int64_t now) { return request.active && !request.linked && now - request.asked_us < WINDOW_US; }

void presence(const char* how) {
    std::lock_guard<std::mutex> guard(lock);
    const int64_t now = esp_timer_get_time();
    ESP_LOGW(TAG, "Presence: %s", how);
    presence_us = now;
    if (request_open(now)) {
        grant();
    }
}

void factory_reset() {
    ESP_LOGE(TAG, "Factory reset: erasing all settings");
    nvs_flash_erase();
    esp_restart();
}

// The BOOT button without the page (it is out of reach, or the password is
// lost). A short press confirms that someone is at the device (linking a
// program). Held 3 s it opens a menu, shown on the status LED as blinks:
//
//   1  leave the menu
//   2  restart
//   3  open the setup network (to set Wi-Fi, Thread or the password again)
//   4  unpair: forget every Matter controller
//   5  forget the network (Wi-Fi and Thread), then the setup network
//
// In the menu a short press goes to the next action, holding 3 s does the
// chosen one; after 2 minutes it closes by itself. Held 10 s (the LED goes
// out): factory reset. Everything acts when the button is released, so a
// stuck button does nothing.
constexpr int MENU_ACTIONS = 5;
constexpr int64_t MENU_HOLD_US = 3 * SECOND_US;
constexpr int64_t RESET_HOLD_US = 10 * SECOND_US;
bool menu_open = false;
int menu_action = 1;
int64_t menu_touched_us = 0;

void close_menu() {
    menu_open = false;
    device_status::show_menu(0);
}

void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(300));
    mb_core::stop();
    esp_restart();
}

void do_action(int action) {
    ESP_LOGW(TAG, "BOOT menu: action %d", action);
    close_menu();
    switch (action) {
        case 2:
            xTaskCreate(restart_task, "restart", 2048, nullptr, 5, nullptr);
            break;
        case 3:
            if (on_recovery != nullptr) on_recovery();
            break;
        case 4:
            ESP_LOGW(TAG, "%s", matter_info::remove_all_fabrics().c_str());
            break;
        case 5:
            config_mode::forget_network();
            if (on_recovery != nullptr) on_recovery();
            break;
        default:
            break;
    }
}

void released(int64_t held_us) {
    device_status::show_hold(false);
    if (held_us >= RESET_HOLD_US && held_us < 30 * SECOND_US) {
        factory_reset();
    } else if (held_us >= MENU_HOLD_US && held_us < RESET_HOLD_US) {
        if (!menu_open) {
            ESP_LOGW(TAG, "BOOT held %" PRId64 " s: menu", held_us / SECOND_US);
            menu_open = true;
            menu_action = 1;
            menu_touched_us = esp_timer_get_time();
            device_status::show_menu(menu_action);
        } else {
            do_action(menu_action);
        }
    } else if (held_us < 2 * SECOND_US) {
        if (menu_open) {
            menu_action = menu_action % MENU_ACTIONS + 1;
            menu_touched_us = esp_timer_get_time();
            device_status::show_menu(menu_action);
        } else {
            presence("BOOT button");
        }
    } else {
        ESP_LOGI(TAG, "BOOT held %" PRId64 " s: nothing to do", held_us / SECOND_US);
    }
}

void button_task(void*) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOOT_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&config);
    // Only once the button has been seen released for a second: a board
    // without one, or a pin held low, never acts
    int64_t released_since = -1;
    int64_t pressed_since = -1;
    bool armed = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const bool down = gpio_get_level(BOOT_GPIO) == 0;
        const int64_t now = esp_timer_get_time();
        if (!armed) {
            released_since = down ? -1 : released_since < 0 ? now : released_since;
            armed = released_since >= 0 && now - released_since > SECOND_US;
        } else if (down && pressed_since < 0) {
            pressed_since = now;
        } else if (down) {
            // What letting go now would do: flicker for the menu, dark for a reset
            const int64_t held = now - pressed_since;
            if (held >= MENU_HOLD_US && held < 30 * SECOND_US) device_status::show_hold(true, held >= RESET_HOLD_US);
        } else if (!down && pressed_since >= 0) {
            released(now - pressed_since);
            pressed_since = -1;
        }
        if (menu_open && now - menu_touched_us > 120 * SECOND_US) {
            close_menu();
        }
    }
}

void store_power_ons(uint8_t count) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "power_ons", count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void on_power_on_limit(void*) { store_power_ons(0); }

// Power-ons in a row, each cut within 10 s: three count as a press of the
// BOOT button, six as holding it (recovery). A brownout or a normal start
// resets the count, so power cuts do not add up to it.
void count_power_ons() {
    uint8_t count = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "power_ons", &count);
        nvs_close(nvs);
    }
    if (esp_reset_reason() != ESP_RST_POWERON) {
        if (count != 0) {
            store_power_ons(0);
        }
        return;
    }
    ++count;
    if (count == POWER_ONS) {
        presence("power switched on 3 times");
    }
    if (count >= RECOVERY_POWER_ONS) {
        store_power_ons(0);
        ESP_LOGW(TAG, "Power switched on %d times: recovery", RECOVERY_POWER_ONS);
        if (on_recovery != nullptr) {
            on_recovery();  // restarts on the setup access point
        }
        return;
    }
    store_power_ons(count);
    if (count == 0) {
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = on_power_on_limit;
    args.name = "power_ons";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, POWER_ON_LIMIT_US);
    }
}

std::string json_text(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out + "\"";
}

}  // namespace

void start(void (*recovery)()) {
    on_recovery = recovery;
    device_status::start();  // the status LED, in every mode
    {
        std::lock_guard<std::mutex> guard(lock);
        load_clients();
    }
    count_power_ons();
    xTaskCreate(button_task, "boot_button", 3072, nullptr, 5, nullptr);
}

bool token_valid(const char* token) {
    if (token == nullptr || std::strlen(token) != 2 * TOKEN_BYTES) {
        return false;
    }
    uint8_t hash[32];
    hash_token(token, hash);
    std::lock_guard<std::mutex> guard(lock);
    for (int i = 0; i < client_count; ++i) {
        uint8_t diff = 0;
        for (size_t b = 0; b < sizeof(hash); ++b) {
            diff |= hash[b] ^ clients[i].hash[b];
        }
        if (diff == 0) {
            return true;
        }
    }
    return false;
}

std::string link_request(const std::string& name) {
    if (name.empty() || name.size() >= NAME_LEN) {
        return "{\"error\":\"name: 1 to 23 characters\"}";
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '\\') {
            return "{\"error\":\"name: printable characters, no quotes\"}";
        }
    }
    std::lock_guard<std::mutex> guard(lock);
    const int64_t now = esp_timer_get_time();
    request = Request();
    request.active = true;
    request.asked_us = now;
    random_hex(request.id, 8);
    std::strncpy(request.name, name.c_str(), NAME_LEN - 1);
    ESP_LOGW(TAG, "'%s' asks for access: press the BOOT button within 2 minutes", request.name);
    // Presence shortly before the request counts as well
    if (presence_us >= 0 && now - presence_us < WINDOW_US) {
        grant();
    }
    return std::string("{\"id\":\"") + request.id + "\",\"seconds\":" + std::to_string(WINDOW_US / SECOND_US) + "}";
}

std::string link_poll(const std::string& id) {
    std::lock_guard<std::mutex> guard(lock);
    const int64_t now = esp_timer_get_time();
    if (!request.active || id != request.id) {
        return "{\"state\":\"unknown\"}";
    }
    if (request.linked) {
        const std::string reply = std::string("{\"state\":\"linked\",\"token\":\"") + request.token + "\"}";
        request = Request();  // the token is given once
        return reply;
    }
    if (!request_open(now)) {
        request = Request();
        return "{\"state\":\"expired\"}";
    }
    return "{\"state\":\"waiting\",\"seconds\":" + std::to_string((WINDOW_US - (now - request.asked_us)) / SECOND_US) +
           "}";
}

std::string command(const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> guard(lock);
    if (args.size() == 3 && args[1] == "revoke") {
        const int index = std::atoi(args[2].c_str());
        if (args[2].empty() || args[2][0] < '0' || args[2][0] > '9' || index >= client_count) {
            return "no such program\n";
        }
        ESP_LOGW(TAG, "Access of '%s' revoked", clients[index].name);
        std::memmove(clients + index, clients + index + 1, (client_count - index - 1) * sizeof(Client));
        --client_count;
        return store_clients() ? "ok\n" : "storing failed\n";
    }
    const int64_t now = esp_timer_get_time();
    if (args.size() == 2 && args[1] == "json") {
        std::string out = "{\"clients\":[";
        for (int i = 0; i < client_count; ++i) {
            out += std::string(i ? "," : "") + "{\"index\":" + std::to_string(i) + ",\"name\":" +
                   json_text(clients[i].name) + "}";
        }
        out += "],\"pending\":";
        if (request_open(now)) {
            out += "{\"name\":" + json_text(request.name) +
                   ",\"seconds\":" + std::to_string((WINDOW_US - (now - request.asked_us)) / SECOND_US) + "}";
        } else {
            out += "null";
        }
        return out + "}";
    }
    std::string out;
    for (int i = 0; i < client_count; ++i) {
        out += std::to_string(i) + ": " + clients[i].name + "\n";
    }
    if (request_open(now)) {
        out += std::string("waiting for the BOOT button: '") + request.name + "'\n";
    }
    return out.empty() ? "no programs linked\n" : out;
}

}  // namespace program_access
