#include "io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace io {

namespace {

const char* TAG = "io";
const char* NVS_NAMESPACE = "io";
const char* DEFAULT_NAME = "Matter Builder";

constexpr int64_t MS = 1000;
constexpr int64_t TRIAL_US = 120 * 1000 * MS;
constexpr int MAX_TRIAL_BOOTS = 3;  // more starts in a trial: it crashes, go back
constexpr int POLL_MS = 20;
constexpr int64_t LONG_PRESS_US = 1000 * MS;
constexpr int64_t MULTI_PRESS_US = 400 * MS;
constexpr int64_t CLIMATE_US = 30 * 1000 * MS;
constexpr uint8_t SHT4X_ADDRESS = 0x44;

// Pins a connection may use on the ESP32-C6: not the strapping pins (8, 9,
// 15), USB (12, 13), the serial console (16, 17) or the flash (24-30)
constexpr uint8_t SAFE_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23};

const char* TYPE_NAMES[] = {"none", "relay", "led", "button", "contact", "motion", "sht4x"};
constexpr int TYPE_COUNT = sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]);

// Runtime state of one connection
struct Live {
    bool on = false;          // outputs
    uint8_t level = 254;      // LED
    bool raw = false;         // inputs: last sample, active
    bool active = false;      // debounced
    int64_t down_since = 0;   // button
    int64_t last_release = 0;
    uint8_t presses = 0;
    bool long_sent = false;
    bool have_climate = false;
    int16_t centi_celsius = 0;
    uint16_t centi_percent = 0;
    int64_t next_climate = 0;
    int ledc_channel = -1;
    i2c_master_dev_handle_t sensor = nullptr;
};

std::mutex lock;
Slot running[SLOTS] = {};
Slot draft[SLOTS] = {};
Live live[SLOTS];
Listener listener = {};
bool in_trial = false;
esp_timer_handle_t trial_timer = nullptr;
int64_t trial_end_us = 0;
char device_name[33] = {};

bool load(const char* key, Slot* slots) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(Slot) * SLOTS;
    const bool ok = nvs_get_blob(nvs, key, slots, &size) == ESP_OK && size == sizeof(Slot) * SLOTS;
    nvs_close(nvs);
    return ok;
}

bool store(const char* key, const Slot* slots) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_blob(nvs, key, slots, sizeof(Slot) * SLOTS) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

uint8_t trial_boots() {
    uint8_t value = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "trial", &value);
        nvs_close(nvs);
    }
    return value;
}

void set_trial_boots(uint8_t value) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "trial", value);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool safe_pin(int pin) {
    for (const uint8_t p : SAFE_PINS) {
        if (p == pin) return true;
    }
    return false;
}

bool is_output(Type type) { return type == Type::relay || type == Type::led; }
bool is_input(Type type) { return type == Type::button || type == Type::contact || type == Type::motion; }

void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));  // let the reply go out first
    esp_restart();
}

void restart_soon() {
    stop_outputs();
    xTaskCreate(restart_task, "restart", 2048, nullptr, 5, nullptr);
}

// No confirmation in time: back to what worked
void on_trial_timeout(void*) {
    ESP_LOGW(TAG, "Trial not confirmed: back to the previous connections");
    set_trial_boots(0);
    restart_soon();
}

// ---------------------------------------------------------------- pins

void write_output(int i) {
    const Slot& s = running[i];
    const Live& l = live[i];
    if (s.type == Type::relay) {
        const bool level = l.on != ((s.flags & INVERTED) != 0);
        gpio_set_level(static_cast<gpio_num_t>(s.pin), level);
    } else if (s.type == Type::led && l.ledc_channel >= 0) {
        uint32_t duty = l.on ? (static_cast<uint32_t>(l.level) * 8191) / 254 : 0;
        if (s.flags & INVERTED) duty = 8191 - duty;
        const auto channel = static_cast<ledc_channel_t>(l.ledc_channel);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }
}

void setup_pins() {
    bool timer_ready = false;
    int next_channel = 0;
    for (int i = 0; i < SLOTS; ++i) {
        const Slot& s = running[i];
        Live& l = live[i];
        const auto pin = static_cast<gpio_num_t>(s.pin);
        if (s.type == Type::relay) {
            gpio_reset_pin(pin);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            write_output(i);
        } else if (s.type == Type::led) {
            if (!timer_ready) {
                ledc_timer_config_t timer = {};
                timer.speed_mode = LEDC_LOW_SPEED_MODE;
                timer.duty_resolution = LEDC_TIMER_13_BIT;
                timer.timer_num = LEDC_TIMER_0;
                timer.freq_hz = 5000;
                timer.clk_cfg = LEDC_AUTO_CLK;
                timer_ready = ledc_timer_config(&timer) == ESP_OK;
            }
            ledc_channel_config_t channel = {};
            channel.gpio_num = s.pin;
            channel.speed_mode = LEDC_LOW_SPEED_MODE;
            channel.channel = static_cast<ledc_channel_t>(next_channel);
            channel.timer_sel = LEDC_TIMER_0;
            if (timer_ready && ledc_channel_config(&channel) == ESP_OK) {
                l.ledc_channel = next_channel++;
            }
            write_output(i);
        } else if (is_input(s.type)) {
            gpio_config_t config = {};
            config.pin_bit_mask = 1ULL << s.pin;
            config.mode = GPIO_MODE_INPUT;
            // Buttons and contacts switch to ground; a motion sensor drives high
            const bool to_ground = s.type != Type::motion;
            config.pull_up_en = to_ground ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
            config.pull_down_en = to_ground ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
            gpio_config(&config);
        } else if (s.type == Type::sht4x) {
            i2c_master_bus_config_t bus_config = {};
            bus_config.i2c_port = I2C_NUM_0;
            bus_config.sda_io_num = static_cast<gpio_num_t>(s.pin);
            bus_config.scl_io_num = static_cast<gpio_num_t>(s.pin2);
            bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
            bus_config.glitch_ignore_cnt = 7;
            bus_config.flags.enable_internal_pullup = true;
            i2c_master_bus_handle_t bus = nullptr;
            i2c_device_config_t device = {};
            device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            device.device_address = SHT4X_ADDRESS;
            device.scl_speed_hz = 100000;
            if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK ||
                i2c_master_bus_add_device(bus, &device, &l.sensor) != ESP_OK) {
                ESP_LOGE(TAG, "Connection %d: no I2C bus on pins %d/%d", i, s.pin, s.pin2);
                l.sensor = nullptr;
            }
        }
    }
}

bool input_level_active(const Slot& s) {
    const bool high = gpio_get_level(static_cast<gpio_num_t>(s.pin)) != 0;
    const bool active = s.type == Type::motion ? high : !high;
    return active != ((s.flags & INVERTED) != 0);
}

uint8_t crc8(const uint8_t* data) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

// High-precision measurement: 10 ms, then temperature and humidity with CRCs
void read_sht4x(int i) {
    Live& l = live[i];
    const uint8_t command = 0xFD;
    uint8_t data[6] = {};
    if (l.sensor == nullptr || i2c_master_transmit(l.sensor, &command, 1, 100) != ESP_OK) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (i2c_master_receive(l.sensor, data, sizeof(data), 100) != ESP_OK || crc8(data) != data[2] ||
        crc8(data + 3) != data[5]) {
        return;
    }
    const uint32_t t = (data[0] << 8) | data[1];
    const uint32_t h = (data[3] << 8) | data[4];
    const int32_t centi_celsius = -4500 + static_cast<int32_t>((17500 * t) / 65535);
    int32_t centi_percent = -600 + static_cast<int32_t>((12500 * h) / 65535);
    centi_percent = centi_percent < 0 ? 0 : centi_percent > 10000 ? 10000 : centi_percent;
    {
        std::lock_guard<std::mutex> guard(lock);
        l.have_climate = true;
        l.centi_celsius = static_cast<int16_t>(centi_celsius);
        l.centi_percent = static_cast<uint16_t>(centi_percent);
    }
    if (listener.climate != nullptr) {
        listener.climate(i, l.centi_celsius, l.centi_percent);
    }
}

void button_step(int i, bool active, int64_t now) {
    Live& l = live[i];
    if (active && !l.active) {
        l.active = true;
        l.down_since = now;
        l.long_sent = false;
        if (listener.button) listener.button(i, Press::initial, 0);
    } else if (active && !l.long_sent && now - l.down_since > LONG_PRESS_US) {
        l.long_sent = true;
        l.presses = 0;
        if (listener.button) listener.button(i, Press::long_press, 0);
    } else if (!active && l.active) {
        l.active = false;
        if (l.long_sent) {
            if (listener.button) listener.button(i, Press::long_release, 0);
        } else {
            ++l.presses;
            l.last_release = now;
            if (listener.button) listener.button(i, Press::short_release, 0);
        }
    } else if (!active && l.presses > 0 && now - l.last_release > MULTI_PRESS_US) {
        if (listener.button) listener.button(i, Press::multi_complete, l.presses);
        l.presses = 0;
    }
}

void poll_task(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < SLOTS; ++i) {
            const Slot& s = running[i];
            Live& l = live[i];
            if (s.type == Type::sht4x && now >= l.next_climate) {
                l.next_climate = now + CLIMATE_US;
                read_sht4x(i);
            }
            if (!is_input(s.type)) {
                continue;
            }
            // Debounced: the same reading twice in a row
            const bool sample = input_level_active(s);
            const bool stable = sample == l.raw;
            l.raw = sample;
            if (s.type == Type::button) {
                button_step(i, stable ? sample : l.active, now);
            } else if (stable && sample != l.active) {
                l.active = sample;
                if (listener.input) listener.input(i, sample);
            }
        }
    }
}

// ---------------------------------------------------------------- page

std::string json_text(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out + "\"";
}

std::string slot_json(const Slot& s, int i, bool with_state) {
    std::string out = "{\"index\":" + std::to_string(i) + ",\"type\":\"" + TYPE_NAMES[static_cast<int>(s.type)] +
                      "\",\"pin\":" + std::to_string(s.pin) + ",\"pin2\":" + std::to_string(s.pin2) +
                      ",\"inverted\":" + ((s.flags & INVERTED) ? "true" : "false") + ",\"name\":" + json_text(s.name);
    if (with_state) {
        const Live& l = live[i];
        out += ",\"on\":" + std::string(l.on ? "true" : "false") + ",\"level\":" + std::to_string(l.level) +
               ",\"active\":" + (l.active ? "true" : "false");
        if (l.have_climate) {
            out += ",\"temperature\":" + std::to_string(l.centi_celsius / 100.0) +
                   ",\"humidity\":" + std::to_string(l.centi_percent / 100.0);
        }
    }
    return out + "}";
}

int parse_slot(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    return (text.empty() || *end != '\0' || value < 0 || value >= SLOTS) ? -1 : static_cast<int>(value);
}

int parse_pin(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    return (text.empty() || *end != '\0' || !safe_pin(static_cast<int>(value))) ? -1 : static_cast<int>(value);
}

// A pin used by another connection of the draft
bool pin_taken(int pin, int except) {
    for (int i = 0; i < SLOTS; ++i) {
        if (i == except || draft[i].type == Type::none) continue;
        if (draft[i].pin == pin || (draft[i].type == Type::sht4x && draft[i].pin2 == pin)) return true;
    }
    return false;
}

std::string cmd_set(const std::vector<std::string>& args) {
    // io set <n> <type> <pin> [<pin2>] [inverted] <name...>
    if (args.size() < 5) {
        return "io set <n> <type> <pin> [<pin2>] [inverted] <name>\n";
    }
    const int index = parse_slot(args[2]);
    int type = -1;
    for (int t = 1; t < TYPE_COUNT; ++t) {
        if (args[3] == TYPE_NAMES[t]) type = t;
    }
    if (index < 0 || type < 0) {
        return "connection 0-7 and type relay|led|button|contact|motion|sht4x\n";
    }
    size_t next = 4;
    const int pin = parse_pin(args[next++]);
    int pin2 = 0;
    if (static_cast<Type>(type) == Type::sht4x) {
        pin2 = next < args.size() ? parse_pin(args[next++]) : -1;
    }
    if (pin < 0 || pin2 < 0 || (static_cast<Type>(type) == Type::sht4x && pin2 == pin)) {
        return "not a usable pin\n";
    }
    std::lock_guard<std::mutex> guard(lock);
    if (pin_taken(pin, index) || (static_cast<Type>(type) == Type::sht4x && pin_taken(pin2, index))) {
        return "pin already in use\n";
    }
    if (static_cast<Type>(type) == Type::sht4x) {
        for (int i = 0; i < SLOTS; ++i) {
            if (i != index && draft[i].type == Type::sht4x) return "only one SHT4x\n";
        }
    }
    Slot s = {};
    s.type = static_cast<Type>(type);
    s.pin = static_cast<uint8_t>(pin);
    s.pin2 = static_cast<uint8_t>(pin2);
    if (next < args.size() && args[next] == "inverted") {
        s.flags |= INVERTED;
        ++next;
    }
    std::string label;
    for (; next < args.size(); ++next) {
        label += (label.empty() ? "" : " ") + args[next];
    }
    if (label.empty()) label = TYPE_NAMES[type];
    std::strncpy(s.name, label.c_str(), sizeof(s.name) - 1);
    draft[index] = s;
    return store("draft", draft) ? "ok: 'io apply' to use it\n" : "storing failed\n";
}

}  // namespace

void set_listener(const Listener& l) { listener = l; }

void start() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(device_name);
        if (nvs_get_str(nvs, "name", device_name, &size) != ESP_OK) device_name[0] = '\0';
        nvs_close(nvs);
    }
    load("active", running);
    if (!load("draft", draft)) {
        std::memcpy(draft, running, sizeof(draft));
    }
    const uint8_t boots = trial_boots();
    if (boots > 0 && boots < MAX_TRIAL_BOOTS) {
        // A trial: the draft runs; without "io confirm" it ends in 2 minutes
        set_trial_boots(boots + 1);
        std::memcpy(running, draft, sizeof(running));
        in_trial = true;
        trial_end_us = esp_timer_get_time() + TRIAL_US;
        esp_timer_create_args_t args = {};
        args.callback = on_trial_timeout;
        args.name = "io_trial";
        esp_timer_create(&args, &trial_timer);
        esp_timer_start_once(trial_timer, TRIAL_US);
        ESP_LOGW(TAG, "Trying new connections: confirm on the settings page within 2 minutes");
    } else if (boots >= MAX_TRIAL_BOOTS) {
        ESP_LOGE(TAG, "New connections keep crashing: back to the previous ones");
        set_trial_boots(0);
    }
    for (int i = 0; i < SLOTS; ++i) {
        if (running[i].type != Type::none && (!safe_pin(running[i].pin) ||
                                              (running[i].type == Type::sht4x && !safe_pin(running[i].pin2)))) {
            running[i].type = Type::none;  // never drive a pin that is not safe
        }
    }
    setup_pins();
    for (int i = 0; i < SLOTS; ++i) {
        if (is_input(running[i].type)) {
            live[i].raw = live[i].active = input_level_active(running[i]);
        }
    }
    xTaskCreate(poll_task, "io", 3072, nullptr, 5, nullptr);
}

const Slot& slot(int index) { return running[index]; }

bool input_active(int index) { return live[index].active; }

bool climate(int index, int16_t& centi_celsius, uint16_t& centi_percent) {
    std::lock_guard<std::mutex> guard(lock);
    centi_celsius = live[index].centi_celsius;
    centi_percent = live[index].centi_percent;
    return live[index].have_climate;
}

void set_output(int index, bool on, uint8_t level) {
    if (index < 0 || index >= SLOTS || !is_output(running[index].type)) {
        return;
    }
    live[index].on = on;
    if (level > 0) {
        live[index].level = level;
    }
    write_output(index);
}

void stop_outputs() {
    for (int i = 0; i < SLOTS; ++i) {
        if (is_output(running[i].type)) {
            live[i].on = false;
            write_output(i);
        }
    }
}

std::string name() { return device_name[0] ? device_name : DEFAULT_NAME; }

std::string name_command(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return name() + "\n";
    }
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
        text += (i > 1 ? " " : "") + args[i];
    }
    std::strncpy(device_name, text.c_str(), sizeof(device_name) - 1);
    device_name[sizeof(device_name) - 1] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return "storing failed\n";
    }
    nvs_set_str(nvs, "name", device_name);
    nvs_commit(nvs);
    nvs_close(nvs);
    return "ok, controllers see it after the next start\n";
}

std::string command(const std::vector<std::string>& args) {
    const std::string sub = args.size() >= 2 ? args[1] : "json";
    if (sub == "set") {
        return cmd_set(args);
    }
    if (sub == "clear" && args.size() == 3) {
        const int index = parse_slot(args[2]);
        if (index < 0) return "connection 0-7\n";
        std::lock_guard<std::mutex> guard(lock);
        draft[index] = Slot();
        return store("draft", draft) ? "ok: 'io apply' to use it\n" : "storing failed\n";
    }
    if (sub == "test" && args.size() == 4) {
        const int index = parse_slot(args[2]);
        if (index < 0 || !is_output(running[index].type)) return "not an output in use\n";
        set_output(index, args[3] == "on", 0);
        return "ok\n";
    }
    if (sub == "cancel") {
        std::lock_guard<std::mutex> guard(lock);
        std::memcpy(draft, running, sizeof(draft));
        return store("draft", draft) ? "ok\n" : "storing failed\n";
    }
    if (sub == "apply") {
        set_trial_boots(1);
        restart_soon();
        return "restarting with the new connections: confirm within 2 minutes\n";
    }
    if (sub == "revert") {
        if (!in_trial) return "nothing to undo\n";
        set_trial_boots(0);
        restart_soon();
        return "restarting with the previous connections\n";
    }
    if (sub == "confirm") {
        if (!in_trial) return "nothing to confirm\n";
        esp_timer_stop(trial_timer);
        in_trial = false;
        set_trial_boots(0);
        return store("active", running) ? "ok, kept\n" : "storing failed\n";
    }
    return state_json();
}

std::string state_json() {
    std::string out = "{\"uptime\":" + std::to_string(esp_timer_get_time() / 1000000) +
                      ",\"heap\":" + std::to_string(esp_get_free_heap_size()) +
                      ",\"io\":{\"name\":" + json_text(name().c_str()) + ",\"trial\":";
    const int64_t left = in_trial ? (trial_end_us - esp_timer_get_time()) / 1000000 : 0;
    out += std::to_string(left > 0 ? left : 0) + ",\"changed\":";
    out += std::memcmp(draft, running, sizeof(draft)) != 0 ? "true" : "false";
    out += ",\"pins\":[";
    for (size_t i = 0; i < sizeof(SAFE_PINS); ++i) {
        out += (i ? "," : "") + std::to_string(SAFE_PINS[i]);
    }
    out += "],\"slots\":[";
    for (int i = 0; i < SLOTS; ++i) {
        out += (i ? "," : "") + slot_json(running[i], i, true);
    }
    out += "],\"draft\":[";
    for (int i = 0; i < SLOTS; ++i) {
        out += (i ? "," : "") + slot_json(draft[i], i, false);
    }
    return out + "]}}";
}

}  // namespace io
