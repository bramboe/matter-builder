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

#include "ble_scan.h"
#include "io.h"
#include "ir.h"

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
constexpr int64_t PRESENCE_US = 60 * 1000 * MS;   // not seen for a minute: away
constexpr int64_t BLE_CLIMATE_US = 60 * 1000 * MS;  // a thermometer's reading at most every minute

// Pins a connection may use on the ESP32-C6: not the strapping pins (8, 9,
// 15), USB (12, 13), the serial console (16, 17) or the flash (24-30)
constexpr uint8_t SAFE_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23};

const char* TYPE_NAMES[] = {"none", "relay", "led", "button", "contact", "motion", "sht4x",
                            "ir_send", "ir_receive", "ble_sensor", "ble_presence"};
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
    int8_t rssi = 0;          // Bluetooth
    int8_t battery = -1;
    int64_t seen_us = 0;
    int64_t next_report = 0;
};

// Before version 0.1.3 a slot had no data; stored connections are converted
struct SlotV1 {
    Type type;
    uint8_t pin;
    uint8_t pin2;
    uint8_t flags;
    char name[24];
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
char last_ir[24] = "";  // the last infrared code received, for learning codes
bool bluetooth = false;   // a connection listens to Bluetooth
bool ble_pairing = false;
Slot factory[SLOTS] = {};
const char* factory_name = nullptr;

bool load(const char* key, Slot* slots) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = 0;
    bool ok = nvs_get_blob(nvs, key, nullptr, &size) == ESP_OK;
    if (ok && size == sizeof(Slot) * SLOTS) {
        ok = nvs_get_blob(nvs, key, slots, &size) == ESP_OK;
    } else if (ok && size == sizeof(SlotV1) * SLOTS) {
        SlotV1 old[SLOTS] = {};
        ok = nvs_get_blob(nvs, key, old, &size) == ESP_OK;
        for (int i = 0; ok && i < SLOTS; ++i) {
            slots[i] = Slot();
            slots[i].type = old[i].type;
            slots[i].pin = old[i].pin;
            slots[i].pin2 = old[i].pin2;
            slots[i].flags = old[i].flags;
            std::memcpy(slots[i].name, old[i].name, sizeof(slots[i].name));
        }
    } else {
        ok = false;
    }
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

// Component types: a type id from the name, so stored connections keep
// their type when other components come or go
constexpr int MAX_DRIVERS = 16;
Driver drivers[MAX_DRIVERS];
uint8_t driver_ids[MAX_DRIVERS];
int driver_count = 0;

uint8_t id_of(const char* name) {
    uint8_t crc = 0;
    for (const char* c = name; *c; ++c) {
        crc ^= static_cast<uint8_t>(*c);
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
    return static_cast<uint8_t>(0x40 | (crc & 0x3F));
}

const Driver* driver_of(Type type) {
    for (int i = 0; i < driver_count; ++i) {
        if (driver_ids[i] == static_cast<uint8_t>(type)) return &drivers[i];
    }
    return nullptr;
}

const char* type_name(Type type) {
    if (static_cast<int>(type) < TYPE_COUNT) return TYPE_NAMES[static_cast<int>(type)];
    const Driver* d = driver_of(type);
    return d != nullptr ? d->type : "unknown";
}

int pin_count(Type type) {
    if (const Driver* d = driver_of(type)) return d->pins;
    if (type == Type::none || type == Type::ble_sensor || type == Type::ble_presence) return 0;
    return type == Type::sht4x ? 2 : 1;
}

bool is_output(Type type) {
    if (const Driver* d = driver_of(type)) return d->matter == Matter::on_off || d->matter == Matter::dimmable;
    return type == Type::relay || type == Type::led;
}
bool is_input(Type type) { return type == Type::button || type == Type::contact || type == Type::motion; }
bool is_ir(Type type) { return type == Type::ir_send || type == Type::ir_receive; }
bool is_ble(Type type) { return type == Type::ble_sensor || type == Type::ble_presence; }
uint16_t code_address(const Slot& s) { return static_cast<uint16_t>(s.data[1] | (s.data[2] << 8)); }
uint16_t code_command(const Slot& s) { return static_cast<uint16_t>(s.data[3] | (s.data[4] << 8)); }

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

// An infrared code on a receiver: a press of every connection waiting for it
void on_ir_code(int pin, uint16_t address, uint16_t command) {
    {
        std::lock_guard<std::mutex> guard(lock);
        std::snprintf(last_ir, sizeof(last_ir), "nec:%04X:%04X", address, command);
    }
    for (int i = 0; i < SLOTS; ++i) {
        const Slot& s = running[i];
        if (s.type != Type::ir_receive || s.pin != pin || code_address(s) != address || code_command(s) != command) {
            continue;
        }
        if (listener.button) {
            listener.button(i, Press::initial, 0);
            listener.button(i, Press::short_release, 0);
            listener.button(i, Press::multi_complete, 1);
        }
    }
}

// A Bluetooth advertisement: the connections that follow this address
void on_ble_advert(const ble_scan::Advert& a) {
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < SLOTS; ++i) {
        const Slot& s = running[i];
        if (!is_ble(s.type) || std::memcmp(s.data, a.mac, 6) != 0) continue;
        Live& l = live[i];
        bool arrived = false, report = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            l.seen_us = now;
            l.rssi = a.rssi;
            if (a.battery >= 0) l.battery = a.battery;
            if (s.type == Type::ble_presence && !l.active) {
                l.active = arrived = true;
            }
            if (s.type == Type::ble_sensor && a.has_climate) {
                const bool changed = !l.have_climate || a.centi_celsius != l.centi_celsius ||
                                     a.centi_percent != l.centi_percent;
                l.have_climate = true;
                l.centi_celsius = a.centi_celsius;
                l.centi_percent = a.centi_percent;
                report = changed && now >= l.next_report;
                if (report) l.next_report = now + BLE_CLIMATE_US;
            }
        }
        if (arrived && listener.input) listener.input(i, true);
        if (report && listener.climate) listener.climate(i, a.centi_celsius, a.centi_percent);
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
        } else if (s.type == Type::ir_send) {
            ir::start_send(s.pin);
        } else if (s.type == Type::ir_receive) {
            ir::start_receive(s.pin, on_ir_code);
        } else if (const Driver* d = driver_of(s.type)) {
            if (!d->setup(i, s)) ESP_LOGE(TAG, "Connection %d (%s): setup failed", i, d->type);
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
            if (const Driver* d = driver_of(s.type)) {
                if (d->poll) d->poll(i, s, now);
                continue;
            }
            if (s.type == Type::ble_presence && l.active && now - l.seen_us > PRESENCE_US) {
                l.active = false;
                if (listener.input) listener.input(i, false);
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
    std::string out = "{\"index\":" + std::to_string(i) + ",\"type\":\"" + type_name(s.type) +
                      "\",\"pin\":" + std::to_string(s.pin) + ",\"pin2\":" + std::to_string(s.pin2) +
                      ",\"inverted\":" + ((s.flags & INVERTED) ? "true" : "false") + ",\"name\":" + json_text(s.name);
    if (is_ir(s.type)) out += ",\"code\":\"" + code_text(s.data) + "\"";
    if (is_ble(s.type)) out += ",\"mac\":\"" + mac_text(s.data) + "\"";
    if (const Driver* d = driver_of(s.type); d != nullptr && d->number_label) {
        out += ",\"number\":" + std::to_string(number_of(s));
    }
    if (with_state) {
        const Live& l = live[i];
        out += ",\"on\":" + std::string(l.on ? "true" : "false") + ",\"level\":" + std::to_string(l.level) +
               ",\"active\":" + (l.active ? "true" : "false");
        if (l.have_climate) {
            out += ",\"temperature\":" + std::to_string(l.centi_celsius / 100.0) +
                   ",\"humidity\":" + std::to_string(l.centi_percent / 100.0);
        }
        if (is_ble(s.type) && l.seen_us > 0) {
            out += ",\"rssi\":" + std::to_string(l.rssi) +
                   ",\"seen\":" + std::to_string((esp_timer_get_time() - l.seen_us) / 1000000);
            if (l.battery >= 0) out += ",\"battery\":" + std::to_string(l.battery);
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

// A pin used by another connection of the draft; IR codes of the same kind
// share their LED or receiver
bool pin_taken(int pin, int except, Type type) {
    for (int i = 0; i < SLOTS; ++i) {
        const Slot& d = draft[i];
        const int pins = pin_count(d.type);
        if (i == except || pins == 0) continue;
        if (is_ir(type) && d.type == type && d.pin == pin) continue;
        if (d.pin == pin || (pins == 2 && d.pin2 == pin)) return true;
    }
    return false;
}

// Distinct pins of a kind in the draft with this slot set to (type, pin)
int pins_of(Type type, int index, int pin) {
    int pins[SLOTS] = {};
    int count = 0;
    for (int i = 0; i < SLOTS; ++i) {
        const int p = i == index ? pin : draft[i].pin;
        if ((i == index ? type : draft[i].type) != type) continue;
        bool seen = false;
        for (int k = 0; k < count; ++k) seen = seen || pins[k] == p;
        if (!seen) pins[count++] = p;
    }
    return count;
}

// io set <n> <component type> [<pin> [<pin2>]] [n=<number>] [inverted] <name...>
std::string set_component(const std::vector<std::string>& args, int index, Type type) {
    const Driver* d = driver_of(type);
    size_t next = 4;
    int pins[2] = {0, 0};
    for (int p = 0; p < d->pins; ++p) {
        pins[p] = next < args.size() ? parse_pin(args[next++]) : -1;
        if (pins[p] < 0) return "not a usable pin\n";
    }
    if (d->pins == 2 && pins[0] == pins[1]) return "two different pins\n";
    Slot s = {};
    s.type = type;
    s.pin = static_cast<uint8_t>(pins[0]);
    s.pin2 = static_cast<uint8_t>(pins[1]);
    if (d->number_label && next < args.size() && args[next].rfind("n=", 0) == 0) {
        const long value = std::strtol(args[next++].c_str() + 2, nullptr, 10);
        if (value < 0 || value > 65535) return "number 0-65535\n";
        s.data[0] = value & 0xFF;
        s.data[1] = value >> 8;
    }
    if (next < args.size() && args[next] == "inverted") {
        s.flags |= INVERTED;
        ++next;
    }
    std::string label;
    for (; next < args.size(); ++next) label += (label.empty() ? "" : " ") + args[next];
    if (label.empty()) label = d->title;
    std::strncpy(s.name, label.c_str(), sizeof(s.name) - 1);
    std::lock_guard<std::mutex> guard(lock);
    for (int p = 0; p < d->pins; ++p) {
        if (pin_taken(pins[p], index, type)) return "pin already in use\n";
    }
    draft[index] = s;
    return store("draft", draft) ? "ok: 'io apply' to use it\n" : "storing failed\n";
}

std::string cmd_set(const std::vector<std::string>& args) {
    // io set <n> <type> <pin> [<pin2>] [inverted] <name...>
    // io set <n> ir_send|ir_receive <pin> <code> <name...>
    // io set <n> ble_sensor|ble_presence <mac> <name...>
    if (args.size() < 5) {
        return "io set <n> <type> <pin> [<pin2>] [inverted] <name>\n";
    }
    const int index = parse_slot(args[2]);
    int type = -1;
    for (int t = 1; t < TYPE_COUNT; ++t) {
        if (args[3] == TYPE_NAMES[t]) type = t;
    }
    for (int d = 0; d < driver_count; ++d) {
        if (args[3] == drivers[d].type) type = driver_ids[d];
    }
    if (index >= 0 && type >= 0 && driver_of(static_cast<Type>(type)) != nullptr) {
        return set_component(args, index, static_cast<Type>(type));
    }
    if (index < 0 || type < 0) {
        return "connection 0-7 and type relay|led|button|contact|motion|sht4x|ir_send|ir_receive|ble_sensor|ble_presence\n";
    }
    const Type kind = static_cast<Type>(type);
    size_t next = 4;
    uint8_t data[8] = {};
    int pin = 0;
    if (is_ble(kind)) {
        if (!parse_mac(args[next++].c_str(), data)) return "not a Bluetooth address (AA:BB:CC:DD:EE:FF)\n";
    } else {
        pin = parse_pin(args[next++]);
    }
    int pin2 = 0;
    if (kind == Type::sht4x) {
        pin2 = next < args.size() ? parse_pin(args[next++]) : -1;
    }
    if (is_ir(kind) && (next >= args.size() || !parse_code(args[next++].c_str(), data))) {
        return "not an IR code (nec:<address>:<command>, 4 hex digits each)\n";
    }
    if (pin < 0 || pin2 < 0 || (kind == Type::sht4x && pin2 == pin)) {
        return "not a usable pin\n";
    }
    std::lock_guard<std::mutex> guard(lock);
    if (!is_ble(kind) && (pin_taken(pin, index, kind) || (kind == Type::sht4x && pin_taken(pin2, index, kind)))) {
        return "pin already in use\n";
    }
    if (is_ir(kind) && pins_of(kind, index, pin) > ir::MAX_PINS) {
        return "at most 2 pins for IR sending and 2 for receiving\n";
    }
    if (static_cast<Type>(type) == Type::sht4x) {
        for (int i = 0; i < SLOTS; ++i) {
            if (i != index && draft[i].type == Type::sht4x) return "only one SHT4x\n";
        }
    }
    Slot s = {};
    s.type = kind;
    s.pin = static_cast<uint8_t>(pin);
    s.pin2 = static_cast<uint8_t>(pin2);
    std::memcpy(s.data, data, sizeof(s.data));
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

void register_driver(const Driver& driver) {
    if (driver_count >= MAX_DRIVERS) return;
    const uint8_t id = id_of(driver.type);
    for (int i = 0; i < driver_count; ++i) {
        if (driver_ids[i] == id) {
            ESP_LOGE(TAG, "Component type %s clashes with %s: rename one", driver.type, drivers[i].type);
            return;
        }
    }
    drivers[driver_count] = driver;
    driver_ids[driver_count++] = id;
}

void report_input(int slot, bool active) {
    if (slot < 0 || slot >= SLOTS || live[slot].active == active) return;
    live[slot].active = active;
    if (listener.input) listener.input(slot, active);
}

void report_press(int slot) {
    if (!listener.button) return;
    listener.button(slot, Press::initial, 0);
    listener.button(slot, Press::short_release, 0);
    listener.button(slot, Press::multi_complete, 1);
}

void report_climate(int slot, int16_t centi_celsius, uint16_t centi_percent) {
    {
        std::lock_guard<std::mutex> guard(lock);
        live[slot].have_climate = true;
        live[slot].centi_celsius = centi_celsius;
        live[slot].centi_percent = centi_percent;
    }
    if (listener.climate) listener.climate(slot, centi_celsius, centi_percent);
}

void report_output(int slot, bool on) {
    live[slot].on = on;
    if (listener.output) listener.output(slot, on);
}

Type type_named(const char* name) {
    for (int t = 1; t < TYPE_COUNT; ++t) {
        if (std::strcmp(name, TYPE_NAMES[t]) == 0) return static_cast<Type>(t);
    }
    for (int d = 0; d < driver_count; ++d) {
        if (std::strcmp(name, drivers[d].type) == 0) return static_cast<Type>(driver_ids[d]);
    }
    return Type::none;
}

uint16_t number_of(const Slot& s) { return static_cast<uint16_t>(s.data[0] | (s.data[1] << 8)); }

int matter_of(int index) {
    const Type type = running[index].type;
    if (const Driver* d = driver_of(type)) return static_cast<int>(d->matter);
    switch (type) {
        case Type::relay: case Type::ir_send: return 0;
        case Type::led: return 1;
        case Type::button: case Type::ir_receive: return 2;
        case Type::contact: return 3;
        case Type::motion: case Type::ble_presence: return 4;
        case Type::sht4x: case Type::ble_sensor: return 5;
        default: return -1;
    }
}

bool parse_code(const char* text, uint8_t* data) {
    unsigned address = 0, command = 0;
    char end = 0;
    if (std::sscanf(text, "nec:%4x:%4x%c", &address, &command, &end) != 2) return false;
    std::memset(data, 0, 8);
    data[0] = IR_NEC;
    data[1] = address & 0xFF;
    data[2] = address >> 8;
    data[3] = command & 0xFF;
    data[4] = command >> 8;
    return true;
}

bool parse_mac(const char* text, uint8_t* data) {
    unsigned b[6] = {};
    char end = 0;
    if (std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6) {
        return false;
    }
    std::memset(data, 0, 8);
    for (int i = 0; i < 6; ++i) data[i] = static_cast<uint8_t>(b[i]);
    return true;
}

std::string code_text(const uint8_t* data) {
    char text[24];
    std::snprintf(text, sizeof(text), "nec:%04X:%04X", data[1] | (data[2] << 8), data[3] | (data[4] << 8));
    return text;
}

std::string mac_text(const uint8_t* data) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", data[0], data[1], data[2], data[3], data[4],
                  data[5]);
    return text;
}

void set_listener(const Listener& l) { listener = l; }

void set_factory(const Slot* slots, const char* name) {
    std::memcpy(factory, slots, sizeof(factory));
    factory_name = name;
}

void start() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(device_name);
        if (nvs_get_str(nvs, "name", device_name, &size) != ESP_OK) device_name[0] = '\0';
        nvs_close(nvs);
    }
    if (!load("active", running)) {
        std::memcpy(running, factory, sizeof(running));  // nothing set on the page yet
    }
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
    bluetooth = false;
    for (int i = 0; i < SLOTS; ++i) {
        const Type t = running[i].type;
        if (t != Type::none && static_cast<int>(t) >= TYPE_COUNT && driver_of(t) == nullptr) {
            running[i].type = Type::none;  // a component this firmware does not have
        }
        if (t != Type::none && pin_count(t) > 0 &&
            (!safe_pin(running[i].pin) || (pin_count(t) == 2 && !safe_pin(running[i].pin2)))) {
            running[i].type = Type::none;  // never drive a pin that is not safe
        }
        bluetooth = bluetooth || is_ble(running[i].type);
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

bool needs_bluetooth() { return bluetooth; }

void start_bluetooth(bool shared_with_pairing) {
    if (bluetooth) ble_scan::start(on_ble_advert, shared_with_pairing);  // only when a connection needs it
}

void set_ble_pairing(bool on) { ble_pairing = on; }

bool is_momentary(int index) { return index >= 0 && index < SLOTS && running[index].type == Type::ir_send; }

void set_output(int index, bool on, uint8_t level) {
    if (is_momentary(index)) {
        const Slot& s = running[index];
        if (on && !ir::send_nec(s.pin, code_address(s), code_command(s))) {
            ESP_LOGW(TAG, "Connection %d: sending the IR code failed", index);
        }
        return;
    }
    if (index >= 0 && index < SLOTS) {
        if (const Driver* d = driver_of(running[index].type)) {
            live[index].on = on;
            if (level > 0) live[index].level = level;
            if (d->output) d->output(index, running[index], on, live[index].level);
            return;
        }
    }
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

std::string name() { return device_name[0] ? device_name : factory_name ? factory_name : DEFAULT_NAME; }

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
        if (index < 0 || (!is_output(running[index].type) && !is_momentary(index))) return "not an output in use\n";
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
    {
        std::lock_guard<std::mutex> guard(lock);
        out += ",\"ir_last\":" + json_text(last_ir);
        out += std::string(",\"bluetooth\":\"") + (ble_scan::scanning() ? "on" : bluetooth ? "starting" : "off") +
               "\",\"ble_pairing\":" + (ble_pairing ? "true" : "false");
    }
    out += ",\"pins\":[";
    for (size_t i = 0; i < sizeof(SAFE_PINS); ++i) {
        out += (i ? "," : "") + std::to_string(SAFE_PINS[i]);
    }
    out += "],\"drivers\":[";
    for (int i = 0; i < driver_count; ++i) {
        const Driver& d = drivers[i];
        static const char* MATTER[] = {"on_off", "dimmable", "button", "contact", "occupancy", "climate"};
        out += (i ? ",{" : "{") + std::string("\"type\":") + json_text(d.type) + ",\"title\":" + json_text(d.title) +
               ",\"matter\":\"" + MATTER[static_cast<int>(d.matter)] + "\",\"pins\":" + std::to_string(d.pins) +
               ",\"number\":" + (d.number_label ? json_text(d.number_label) : std::string("null")) + "}";
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
