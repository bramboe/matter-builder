#include "device_status.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include "commands.h"
#include "config_mode.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mb_core.h"
#include "net.h"
#include "nvs.h"
#include "soc/soc_caps.h"

namespace device_status {

namespace {

const char* TAG = "status";
const char* NVS_NAMESPACE = "mb_core";
constexpr int TICK_MS = 50;
constexpr int POLL_TICKS = 5000 / TICK_MS;  // Matter is asked every 5 s
constexpr int MAX_CONTROLLERS = 16;

std::mutex lock;
std::atomic<State> current{State::starting};
std::atomic<bool> in_setup{false}, setup_own_network{false};

// What Matter said last (written on the Matter thread)
int fabric_count = 0;
int connected_count = 0;
struct Seen {
    bool known = false;
    bool connected = false;
    int64_t since_us = 0;      // connected since
    int64_t last_seen_us = -1;  // last time it was connected
};
Seen seen[MAX_CONTROLLERS + 1];

// The LED
int pin = -1;
bool inverted = false;
bool quiet = false;
std::atomic<int> menu_action{0};
std::atomic<int> hold{0};  // 1: flicker, 2: off

struct Step {
    uint16_t ms;
    bool on;
};
const Step SETUP[] = {{100, true}, {100, false}, {100, true}, {700, false}};
const Step CONNECTING[] = {{1000, true}, {1000, false}};
const Step UNPAIRED[] = {{120, true}, {180, false}, {120, true}, {2580, false}};
const Step PAIRED[] = {{2580, true}, {120, false}, {180, true}, {120, false}};
const Step ON[] = {{1000, true}};
const Step OFF[] = {{1000, false}};
const Step FLICKER[] = {{50, true}, {50, false}};

template <size_t N>
bool level(const Step (&steps)[N], int64_t ms) {
    int total = 0;
    for (const Step& s : steps) total += s.ms;
    int at = static_cast<int>(ms % total);
    for (const Step& s : steps) {
        if (at < s.ms) return s.on;
        at -= s.ms;
    }
    return false;
}

// n blinks, then a pause: which action of the menu is chosen
bool menu_level(int action, int64_t ms) {
    const int total = action * 400 + 1200;
    const int at = static_cast<int>(ms % total);
    return at < action * 400 && at % 400 < 150;
}

bool led_level(int64_t ms) {
    const int held = hold.load();
    if (held == 2) return false;
    if (held == 1) return level(FLICKER, ms);
    const int action = menu_action.load();
    if (action > 0) return menu_level(action, ms);
    switch (current.load()) {
        case State::setup: return level(SETUP, ms);
        case State::connecting: return level(CONNECTING, ms);
        case State::unpaired: return level(UNPAIRED, ms);
        case State::paired: return level(PAIRED, ms);
        case State::connected: return quiet ? level(OFF, ms) : level(ON, ms);
        default: return false;
    }
}

void poll_matter(intptr_t) {
    const int64_t now = esp_timer_get_time();
    auto* engine = chip::app::InteractionModelEngine::GetInstance();
    int fabrics = 0, connected = 0;
    bool present[MAX_CONTROLLERS + 1] = {};
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& fabric : chip::Server::GetInstance().GetFabricTable()) {
        const unsigned index = fabric.GetFabricIndex();
        ++fabrics;
        if (index > MAX_CONTROLLERS) continue;
        present[index] = true;
        const bool live = engine != nullptr &&
            engine->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe, fabric.GetFabricIndex()) > 0;
        Seen& s = seen[index];
        if (live && !(s.known && s.connected)) s.since_us = now;
        if (live) s.last_seen_us = now;
        s.known = true;
        s.connected = live;
        connected += live ? 1 : 0;
    }
    for (int i = 0; i <= MAX_CONTROLLERS; ++i) {
        if (!present[i]) seen[i] = Seen();
    }
    fabric_count = fabrics;
    connected_count = connected;
}

void update_state() {
    if (in_setup.load()) {
        current = setup_own_network.load() ? State::setup : State::connecting;
        return;
    }
    if (!net::up()) {
        current = State::connecting;
        return;
    }
    std::lock_guard<std::mutex> guard(lock);
    current = fabric_count == 0 ? State::unpaired : connected_count == 0 ? State::paired : State::connected;
}

void tick(void*) {
    static int ticks = 0;
    if (++ticks % POLL_TICKS == 0 && !in_setup.load() &&
        xTaskGetHandle(CHIP_DEVICE_CONFIG_CHIP_TASK_NAME) != nullptr) {
        (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(poll_matter, 0);
    }
    if (ticks % 10 == 0) update_state();
    if (pin >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(pin), led_level(esp_timer_get_time() / 1000) != inverted);
    }
}

// Why the last restart happened, noted just before it and read once at start
char last_why[64] = "";

void load() {
    nvs_handle_t rw;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &rw) == ESP_OK) {
        size_t size = sizeof(last_why);
        if (nvs_get_str(rw, "why", last_why, &size) == ESP_OK) {
            nvs_erase_key(rw, "why");
            nvs_commit(rw);
        } else {
            last_why[0] = '\0';
        }
        nvs_close(rw);
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    int8_t stored = -1;
    uint8_t flags = 0;
    if (nvs_get_i8(nvs, "led_pin", &stored) == ESP_OK) pin = stored;
    if (nvs_get_u8(nvs, "led_flags", &flags) == ESP_OK) {
        inverted = (flags & 1) != 0;
        quiet = (flags & 2) != 0;
    }
    nvs_close(nvs);
}

bool store() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_i8(nvs, "led_pin", static_cast<int8_t>(pin)) == ESP_OK &&
                    nvs_set_u8(nvs, "led_flags", (inverted ? 1 : 0) | (quiet ? 2 : 0)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

void use_pin() {
    if (pin < 0) return;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
}

}  // namespace

void start() {
    load();
    use_pin();
    esp_timer_create_args_t args = {};
    args.callback = tick;
    args.name = "status";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, TICK_MS * 1000);
    }
    add_command("led", "led [json|set <pin>|none [inverted] [quiet]]   the status LED\n", led_command);
}

State state() { return current.load(); }

const char* state_name() {
    switch (current.load()) {
        case State::setup: return "setup";
        case State::connecting: return "connecting";
        case State::unpaired: return "unpaired";
        case State::paired: return "paired";
        case State::connected: return "connected";
        default: return "starting";
    }
}

void set_setup(bool own_network) {
    setup_own_network = own_network;
    in_setup = true;
}

Controller controller(unsigned fabric_index) {
    std::lock_guard<std::mutex> guard(lock);
    if (fabric_index > MAX_CONTROLLERS || !seen[fabric_index].known) return {false, -1, -1};
    const Seen& s = seen[fabric_index];
    const int64_t now = esp_timer_get_time();
    return {s.connected, s.connected ? static_cast<long>((now - s.since_us) / 1000000) : -1,
            s.last_seen_us < 0 ? -1 : static_cast<long>((now - s.last_seen_us) / 1000000)};
}

std::string json() {
    std::lock_guard<std::mutex> guard(lock);
    // The BOOT button's menu (action, 0: closed) and a press being held
    // (1: letting go opens the menu or does the action, 2: past 10 s)
    return std::string("{\"state\":\"") + state_name() + "\",\"fabrics\":" + std::to_string(fabric_count) +
           ",\"connected\":" + std::to_string(connected_count) + ",\"menu\":" + std::to_string(menu_action.load()) +
           ",\"hold\":" + std::to_string(hold.load()) + "}";
}

void show_menu(int action) { menu_action = action; }

void note_restart(const char* why) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, "why", why);
    nvs_commit(nvs);
    nvs_close(nvs);
}

const char* last_restart() { return last_why; }

void show_hold(bool holding, bool off) { hold = !holding ? 0 : off ? 2 : 1; }

int led_pin() { return pin; }

std::string led_command(const std::vector<std::string>& args) {
    if (args.size() >= 3 && args[1] == "set") {
        int wanted = -1;
        if (args[2] != "none") {
            char* end = nullptr;
            const long value = std::strtol(args[2].c_str(), &end, 10);
            if (args[2].empty() || *end != '\0' || value < 0 || value >= SOC_GPIO_PIN_COUNT) return "error: a pin number, or none\n";
            const auto ok = mb_core::app().status_led_pin_ok;
            if (value != pin && ok != nullptr && !ok(static_cast<int>(value))) {
                return "error: that pin is not free (in use by a connection, or not usable on this board)\n";
            }
            wanted = static_cast<int>(value);
        }
        bool want_inverted = false, want_quiet = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "inverted") want_inverted = true;
            else if (args[i] == "quiet") want_quiet = true;
            else return "error: led set <pin>|none [inverted] [quiet]\n";
        }
        if (pin >= 0 && pin != wanted) gpio_reset_pin(static_cast<gpio_num_t>(pin));
        pin = wanted;
        inverted = want_inverted;
        quiet = want_quiet;
        use_pin();
        ESP_LOGI(TAG, "Status LED on pin %d", pin);
        return store() ? "ok\n" : "storing failed\n";
    }
    return "{\"pin\":" + (pin < 0 ? std::string("null") : std::to_string(pin)) + ",\"inverted\":" + (inverted ? "true" : "false") +
           ",\"quiet\":" + (quiet ? "true" : "false") + ",\"state\":\"" + state_name() + "\"}\n";
}

}  // namespace device_status
