#include "io.h"
#include "sdkconfig.h"
#include "device_status.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ble_scan.h"
#include "io.h"
#include "ir.h"
#include "measurements.h"
#include "rules.h"
#if CONFIG_OPENTHREAD_ENABLED
#include "config_mode.h"
#endif

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
constexpr uint32_t BLUETOOTH_MIN_HEAP = 60 * 1024;  // free memory the Bluetooth stack needs to start

#if CONFIG_IDF_TARGET_ESP32
// Pins a connection may use on the classic ESP32: not the flash (6-11), the
// serial console (1, 3), the strapping pins that decide how it boots (0, 12,
// 15) or the input-only pins (34-39). GPIO 2 is a strapping pin too, but only
// matters while flashing: it is the on-board LED of most boards.
constexpr uint8_t SAFE_PINS[] = {2, 4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
#elif CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3: not the strapping pins (0, 3, 45, 46), USB (19, 20), the flash
// and extra memory (26-37) or the serial console (43, 44)
constexpr uint8_t SAFE_PINS[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48};
#elif CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3: not the strapping pins (2, 8, 9), the flash (11-17), USB (18, 19)
// or the serial console (20, 21)
constexpr uint8_t SAFE_PINS[] = {0, 1, 3, 4, 5, 6, 7, 10};
#elif CONFIG_IDF_TARGET_ESP32C5
// ESP32-C5: not the strapping pins (2, 3, 7, 25-28), the serial console (11,
// 12), USB (13, 14) or the flash (15-22)
constexpr uint8_t SAFE_PINS[] = {0, 1, 4, 5, 6, 8, 9, 10, 23, 24};
#else
// Pins a connection may use on the ESP32-C6: not the strapping pins (8, 9,
// 15), USB (12, 13), the serial console (16, 17) or the flash (24-30)
constexpr uint8_t SAFE_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23};
#endif

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
    int64_t measurement[2] = {-1, -1};
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
    int64_t simulated_until = 0;  // io simulate: the pin is not read until then
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
char group_names[GROUPS][24] = {};
bool group_given = false;  // the last parse_options() saw group=
char last_ir[24] = "";  // the last infrared code received, for learning codes
bool bluetooth = false;   // a connection uses Bluetooth (listens, or sends commands)
bool listening = false;   // a connection listens to advertisements
bool ble_pairing = false;
Slot factory[SLOTS] = {};
const char* factory_name = nullptr;
uint8_t last_on = 0;  // bit n: output n was on (for power-on "last" and "toggle")
// What went wrong with the stored connections at start, for the page ("" when nothing)
std::string problem;
esp_timer_handle_t off_timers[SLOTS] = {};

// `broken` is set when the key is there but cannot be used (not when it is missing)
bool load(const char* key, Slot* slots, bool* broken = nullptr) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = 0;
    bool ok = nvs_get_blob(nvs, key, nullptr, &size) == ESP_OK;
    if (broken != nullptr) *broken = ok && size != sizeof(Slot) * SLOTS && size != sizeof(SlotV1) * SLOTS;
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
    if (pin == device_status::led_pin()) return false;  // the status LED has it
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
    if (const Driver* d = driver_of(type)) {
        return d->matter == Matter::on_off || d->matter == Matter::dimmable || d->matter == Matter::color;
    }
    return type == Type::relay || type == Type::led;
}
bool is_input(Type type) { return type == Type::button || type == Type::contact || type == Type::motion; }
bool is_ir(Type type) { return type == Type::ir_send || type == Type::ir_receive; }
bool is_ble(Type type) { return type == Type::ble_sensor || type == Type::ble_presence; }
uint16_t code_address(const Slot& s) { return static_cast<uint16_t>(s.data[1] | (s.data[2] << 8)); }
uint16_t code_command(const Slot& s) { return static_cast<uint16_t>(s.data[3] | (s.data[4] << 8)); }

// Options in a slot's spare data bytes, so the stored format stays as it is.
// Outputs: data[4] what it does at power-on, data[5..6] seconds after which
// it switches itself off. Inputs: data[7] the output it controls, plus one.
// (IR codes use data[0..4], a Bluetooth address data[0..5], a component's
// number data[0..1]: none of these overlap the bytes an option uses.)
const char* POWER_NAMES[] = {"off", "on", "last", "toggle"};
constexpr uint8_t POWER_OFF = 0, POWER_ON = 1, POWER_LAST = 2, POWER_TOGGLE = 3;
bool can_control(Type type) {
    if (const Driver* d = driver_of(type)) {
        return d->matter == Matter::button || d->matter == Matter::contact || d->matter == Matter::occupancy;
    }
    return is_input(type) || type == Type::ble_presence;
}
uint8_t power_of(const Slot& s) { return is_output(s.type) && s.data[4] <= POWER_TOGGLE ? s.data[4] : POWER_OFF; }
uint16_t off_after_of(const Slot& s) {
    return is_output(s.type) ? static_cast<uint16_t>(s.data[5] | (s.data[6] << 8)) : 0;
}
int controls_of(const Slot& s) {
    return can_control(s.type) && s.data[7] >= 1 && s.data[7] <= SLOTS ? s.data[7] - 1 : -1;
}

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

void act(int source, int how);  // below: an input switching the output it controls

// What happened, to Matter (the listener) and to the rules on the device
void emit_input(int i, bool active) {
    if (listener.input) listener.input(i, active);
    rules::on_input(i, active);
}
void emit_button(int i, Press press, uint8_t count) {
    if (listener.button) listener.button(i, press, count);
    if (press == Press::multi_complete) rules::on_press(i, count);
    if (press == Press::long_press) rules::on_long(i);
}
void emit_climate(int i, int16_t centi_celsius, uint16_t centi_percent) {
    if (listener.climate) listener.climate(i, centi_celsius, centi_percent);
    rules::on_climate(i, centi_celsius, centi_percent);
}

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
        emit_button(i, Press::initial, 0);
        emit_button(i, Press::short_release, 0);
        emit_button(i, Press::multi_complete, 1);
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
        if (arrived) emit_input(i, true);
        if (arrived) act(i, 1);
        if (report) emit_climate(i, a.centi_celsius, a.centi_percent);
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
            l.sensor = measurements::i2c_device(s.pin, s.pin2, SHT4X_ADDRESS);
            if (!l.sensor) ESP_LOGE(TAG, "Connection %d: no I2C bus on pins %d/%d", i, s.pin, s.pin2);
        }
    }
}

bool input_level_active(const Slot& s) {
    const bool high = gpio_get_level(static_cast<gpio_num_t>(s.pin)) != 0;
    const bool active = s.type == Type::motion ? high : !high;
    return active != ((s.flags & INVERTED) != 0);
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
    if (i2c_master_receive(l.sensor, data, sizeof(data), 100) != ESP_OK || measurements::sensirion_crc(data) != data[2] ||
        measurements::sensirion_crc(data + 3) != data[5]) {
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
    emit_climate(i, l.centi_celsius, l.centi_percent);
}

void button_step(int i, bool active, int64_t now) {
    Live& l = live[i];
    if (active && !l.active) {
        l.active = true;
        l.down_since = now;
        l.long_sent = false;
        emit_button(i, Press::initial, 0);
    } else if (active && !l.long_sent && now - l.down_since > LONG_PRESS_US) {
        l.long_sent = true;
        l.presses = 0;
        emit_button(i, Press::long_press, 0);
    } else if (!active && l.active) {
        l.active = false;
        if (l.long_sent) {
            emit_button(i, Press::long_release, 0);
        } else {
            ++l.presses;
            l.last_release = now;
            emit_button(i, Press::short_release, 0);
            act(i, -1);  // a press toggles the output it controls
        }
    } else if (!active && l.presses > 0 && now - l.last_release > MULTI_PRESS_US) {
        emit_button(i, Press::multi_complete, l.presses);
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
                emit_input(i, false);
                act(i, 0);
            }
            if (!is_input(s.type)) {
                continue;
            }
            if (now < l.simulated_until) continue;  // a simulated state holds for a minute
            // Debounced: the same reading twice in a row
            const bool sample = input_level_active(s);
            const bool stable = sample == l.raw;
            l.raw = sample;
            if (s.type == Type::button) {
                button_step(i, stable ? sample : l.active, now);
            } else if (stable && sample != l.active) {
                l.active = sample;
                emit_input(i, sample);
                act(i, sample ? 1 : 0);  // the output it controls follows
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
                      ",\"inverted\":" + ((s.flags & INVERTED) ? "true" : "false") + ",\"name\":" + json_text(s.name) +
                      ",\"group\":" + std::to_string(group_of(s));
    if (is_ir(s.type)) out += ",\"code\":\"" + code_text(s.data) + "\"";
    if (is_ble(s.type)) out += ",\"mac\":\"" + mac_text(s.data) + "\"";
    if (const Driver* d = driver_of(s.type); d != nullptr && d->number_label) {
        out += ",\"number\":" + std::to_string(number_of(s));
    }
    if (is_output(s.type)) {
        out += std::string(",\"power\":\"") + POWER_NAMES[power_of(s)] + "\",\"off_after\":" +
               std::to_string(off_after_of(s));
    }
    if (can_control(s.type)) {
        const int target = controls_of(s);
        out += ",\"controls\":" + (target >= 0 ? std::to_string(target) : std::string("null"));
    }
    if (with_state) {
        const Live& l = live[i];
        out += ",\"on\":" + std::string(l.on ? "true" : "false") + ",\"level\":" + std::to_string(l.level) +
               ",\"active\":" + (l.active ? "true" : "false");
        if (l.have_climate) {
            out += ",\"temperature\":" + std::to_string(l.centi_celsius / 100.0) +
                   ",\"humidity\":" + std::to_string(l.centi_percent / 100.0);
        }
        if (const Driver* d = driver_of(s.type); d != nullptr) {
            std::lock_guard<std::mutex> guard(lock);
            if (d->matter == Matter::air_quality && l.measurement[0] >= 0) {
                out += ",\"co2_ppm\":" + std::to_string(l.measurement[0]);
            } else if (d->matter == Matter::electrical) {
                if (l.measurement[0] >= 0) out += ",\"power_mw\":" + std::to_string(l.measurement[0]);
                if (l.measurement[1] >= 0) out += ",\"energy_mwh\":" + std::to_string(l.measurement[1]);
            }
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

// "inverted" and the options (power=, off_after=, controls=) in any order,
// before the name; "" when they are fine, else what is wrong
std::string parse_options(const std::vector<std::string>& args, size_t& next, Slot& s, int index) {
    group_given = false;
    for (; next < args.size(); ++next) {
        const std::string& word = args[next];
        char* end = nullptr;
        if (word == "inverted") {
            s.flags |= INVERTED;
        } else if (word.rfind("power=", 0) == 0) {
            if (!is_output(s.type)) return "power= is for outputs\n";
            int mode = -1;
            for (int m = 0; m < 4; ++m) mode = word.substr(6) == POWER_NAMES[m] ? m : mode;
            if (mode < 0) return "power=off|on|last|toggle\n";
            s.data[4] = static_cast<uint8_t>(mode);
        } else if (word.rfind("off_after=", 0) == 0) {
            const long value = std::strtol(word.c_str() + 10, &end, 10);
            if (!is_output(s.type)) return "off_after= is for outputs\n";
            if (*end != '\0' || value < 0 || value > 65535) return "off_after=<seconds>, 0-65535\n";
            s.data[5] = value & 0xFF;
            s.data[6] = value >> 8;
        } else if (word.rfind("controls=", 0) == 0) {
            const long value = std::strtol(word.c_str() + 9, &end, 10);
            if (!can_control(s.type)) return "controls= is for buttons, contacts, motion and presence\n";
            if (*end != '\0' || value < 0 || value >= SLOTS || value == index) return "controls=<another connection 0-7>\n";
            s.data[7] = static_cast<uint8_t>(value + 1);
        } else if (word.rfind("group=", 0) == 0) {
            const long value = std::strtol(word.c_str() + 6, &end, 10);
            if (*end != '\0' || value < 0 || value > GROUPS) return "group=<0-4> (0: none)\n";
            s.flags = static_cast<uint8_t>((s.flags & ~GROUP_MASK) | (value << GROUP_SHIFT));
            group_given = true;
        } else {
            break;
        }
    }
    return "";
}

// io set <n> <component type> [<pin> [<pin2>]] [n=<number>] [inverted] [options] <name...>
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
    if (std::string why = parse_options(args, next, s, index); !why.empty()) return why;
    std::string label;
    for (; next < args.size(); ++next) label += (label.empty() ? "" : " ") + args[next];
    if (label.empty()) label = d->title;
    std::strncpy(s.name, label.c_str(), sizeof(s.name) - 1);
    std::lock_guard<std::mutex> guard(lock);
    for (int p = 0; p < d->pins; ++p) {
        if (pin_taken(pins[p], index, type)) return "pin already in use\n";
    }
    if (!group_given && draft[index].type != Type::none) {
        s.flags = static_cast<uint8_t>((s.flags & ~GROUP_MASK) | (draft[index].flags & GROUP_MASK));
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
    if (std::string why = parse_options(args, next, s, index); !why.empty()) return why;
    std::string label;
    for (; next < args.size(); ++next) {
        label += (label.empty() ? "" : " ") + args[next];
    }
    if (label.empty()) label = TYPE_NAMES[type];
    std::strncpy(s.name, label.c_str(), sizeof(s.name) - 1);
    if (!group_given && draft[index].type != Type::none) {
        s.flags = static_cast<uint8_t>((s.flags & ~GROUP_MASK) | (draft[index].flags & GROUP_MASK));
    }
    draft[index] = s;
    return store("draft", draft) ? "ok: 'io apply' to use it\n" : "storing failed\n";
}

void store_last(uint8_t bits) {
    ESP_LOGI(TAG, "Last state of the outputs: 0x%02x", bits);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "last", bits);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void on_off_timer(void* arg) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    ESP_LOGI(TAG, "Connection %d: off after %u s", i, off_after_of(running[i]));
    set_output(i, false, 0);
    if (listener.output) listener.output(i, false);
}

// What follows a change of an output: its timer to switch off, and its state
// for the next power-on when that should come back
void after_output(int i) {
    const Slot& s = running[i];
    if (const uint16_t seconds = off_after_of(s)) {
        if (off_timers[i] == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = on_off_timer;
            args.arg = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            args.name = "io_off";
            esp_timer_create(&args, &off_timers[i]);
        }
        esp_timer_stop(off_timers[i]);
        if (live[i].on) esp_timer_start_once(off_timers[i], static_cast<uint64_t>(seconds) * 1000000);
    }
    const uint8_t power = power_of(s);
    if (power == POWER_LAST || power == POWER_TOGGLE) {
        const uint8_t bits = live[i].on ? (last_on | (1 << i)) : (last_on & ~(1 << i));
        if (bits != last_on) {
            last_on = bits;
            store_last(bits);
        }
    }
}

// An input switches the output it controls: how -1 toggles (a button),
// 1 on and 0 off (a contact, motion or presence).
// Matter hears of it, so controllers show the new state.
void act(int source, int how) {
    const int target = controls_of(running[source]);
    if (target < 0) return;
    if (is_momentary(target)) {
        if (how != 0) set_output(target, true, 0);  // an IR code: sent on press or arrival
        return;
    }
    if (!is_output(running[target].type)) return;
    // With an off-after time the output's own timer switches it off, counted
    // from when the input went inactive (the last motion), not at once
    const uint16_t delay = off_after_of(running[target]);
    if (how == 0 && delay > 0) {
        if (live[target].on) after_output(target);  // restarts the timer from now
        return;
    }
    const bool on = how < 0 ? !live[target].on : how == 1;
    set_output(target, on, 0);
    if (listener.output) listener.output(target, on);
    if (how == 1 && delay > 0 && off_timers[target] != nullptr) {
        esp_timer_stop(off_timers[target]);  // stays on while the input is active
    }
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
    emit_input(slot, active);
    act(slot, active ? 1 : 0);
}

void report_press(int slot) {
    if (slot >= 0 && slot < SLOTS) act(slot, -1);
    emit_button(slot, Press::initial, 0);
    emit_button(slot, Press::short_release, 0);
    emit_button(slot, Press::multi_complete, 1);
}

void report_climate(int slot, int16_t centi_celsius, uint16_t centi_percent) {
    {
        std::lock_guard<std::mutex> guard(lock);
        live[slot].have_climate = true;
        live[slot].centi_celsius = centi_celsius;
        live[slot].centi_percent = centi_percent;
    }
    emit_climate(slot, centi_celsius, centi_percent);
}

void report_measurement(int slot, unsigned channel, int64_t value) {
    if (slot < 0 || slot >= SLOTS || channel > 1) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (live[slot].measurement[channel] == value) return;
        live[slot].measurement[channel] = value;
    }
    if (listener.measurement) listener.measurement(slot, channel);
}

int64_t measurement(int slot, unsigned channel) {
    std::lock_guard<std::mutex> guard(lock);
    return live[slot].measurement[channel];
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
        for (int g = 0; g < GROUPS; ++g) {
            char key[4] = {'g', static_cast<char>('1' + g), 0};
            size_t length = sizeof(group_names[g]);
            if (nvs_get_str(nvs, key, group_names[g], &length) != ESP_OK) group_names[g][0] = '\0';
        }
        nvs_get_u8(nvs, "last", &last_on);
        nvs_close(nvs);
    }
    bool broken = false;
    if (!load("active", running, &broken)) {
        std::memcpy(running, factory, sizeof(running));  // nothing set on the page yet
        if (broken) {
            problem = "The stored connections could not be read: the device started with its factory setting. "
                      "Set the connections again.";
            ESP_LOGE(TAG, "Stored connections unreadable: factory setting");
        }
    }
    if (!load("draft", draft)) {
        std::memcpy(draft, running, sizeof(draft));
    }
    const uint8_t boots = trial_boots();
    if (boots > 0 && boots < MAX_TRIAL_BOOTS) {
        // A trial: the draft runs; without "io confirm" it ends in 2 minutes.
        // A start counts towards giving up on it, except the restart into
        // configuration mode (a device without Thread makes that one by
        // itself right after starting, so every try counted twice)
#if CONFIG_OPENTHREAD_ENABLED
        if (!config_mode::active()) set_trial_boots(boots + 1);
#else
        set_trial_boots(boots + 1);
#endif
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
    bluetooth = listening = false;
    const Type ble_command = type_named("ble_command");
    for (int i = 0; i < SLOTS; ++i) {
        const Type t = running[i].type;
        if (t != Type::none && static_cast<int>(t) >= TYPE_COUNT && driver_of(t) == nullptr) {
            running[i].type = Type::none;  // a component this firmware does not have
            problem += (problem.empty() ? "" : " ") + std::string("Connection ") + std::to_string(i) + " (" +
                       running[i].name + ") is left out: this firmware does not have its type.";
        } else if (t != Type::none && pin_count(t) > 0 &&
                   (!safe_pin(running[i].pin) || (pin_count(t) == 2 && !safe_pin(running[i].pin2)))) {
            running[i].type = Type::none;  // never drive a pin that is not safe
            problem += (problem.empty() ? "" : " ") + std::string("Connection ") + std::to_string(i) + " (" +
                       running[i].name + ") is left out: its pin is not safe to use.";
        }
        listening = listening || is_ble(running[i].type);
        bluetooth = bluetooth || is_ble(running[i].type) ||
                    (ble_command != Type::none && running[i].type == ble_command);
    }
    setup_pins();
    const bool power_cut = esp_reset_reason() == ESP_RST_POWERON;
    ESP_LOGI(TAG, "Outputs at start: last state 0x%02x%s", last_on, power_cut ? ", after a power cut" : "");
    for (int i = 0; i < SLOTS; ++i) {
        if (is_input(running[i].type)) {
            live[i].raw = live[i].active = input_level_active(running[i]);
        }
        // What an output does at power-on (off unless set otherwise). "toggle"
        // flips only after a real power cut: a restart (an update,
        // configuration mode) keeps the output as it was
        const uint8_t power = power_of(running[i]) == POWER_TOGGLE && !power_cut ? POWER_LAST : power_of(running[i]);
        const bool was_on = (last_on >> i) & 1;
        if (is_output(running[i].type) && (power == POWER_ON || (power == POWER_LAST && was_on) ||
                                           (power == POWER_TOGGLE && !was_on))) {
            set_output(i, true, 0);
        } else if (power == POWER_TOGGLE && was_on) {
            after_output(i);  // stored as off now, so the next power-on toggles back
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
    // Only when a connection needs it; scanning only when one listens. The
    // Bluetooth stack needs room: on a classic ESP32 next to Matter and Wi-Fi
    // there is too little, and starting it anyway took the device down
    if (!bluetooth) return;
#if !CONFIG_BT_ENABLED
    problem += (problem.empty() ? "" : " ") + std::string("This board's firmware has no Bluetooth: the Bluetooth connections do not work here.");
    return;
#endif
    if (!shared_with_pairing && esp_get_free_heap_size() < BLUETOOTH_MIN_HEAP) {
        problem += (problem.empty() ? "" : " ") + std::string("Bluetooth is off: not enough memory (") +
                   std::to_string(esp_get_free_heap_size() / 1024) + " KB free, " +
                   std::to_string(BLUETOOTH_MIN_HEAP / 1024) + " KB needed). The Bluetooth connections do not work on this board.";
        ESP_LOGE(TAG, "Not enough memory for Bluetooth (%u bytes free)", static_cast<unsigned>(esp_get_free_heap_size()));
        return;
    }
    ble_scan::start(on_ble_advert, shared_with_pairing, listening);
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
    if (index < 0 || index >= SLOTS || !is_output(running[index].type)) {
        return;
    }
    const bool changed = live[index].on != on;
    live[index].on = on;
    if (level > 0) {
        live[index].level = level;
    }
    if (const Driver* d = driver_of(running[index].type)) {
        if (d->output) d->output(index, running[index], on, live[index].level);
    } else {
        write_output(index);
    }
    after_output(index);
    if (changed) rules::on_output(index, on);
}

void switch_output(int index, bool on, uint8_t level) {
    set_output(index, on, level);
    if (!is_momentary(index) && listener.output) listener.output(index, on);
}

void set_color(int index, const Color& color) {
    if (index < 0 || index >= SLOTS) return;
    if (const Driver* d = driver_of(running[index].type); d != nullptr && d->color != nullptr) {
        d->color(index, running[index], color);
    }
}

bool output_on(int index) { return index >= 0 && index < SLOTS && is_output(running[index].type) && live[index].on; }

void stop_outputs() {
    for (int i = 0; i < SLOTS; ++i) {
        if (is_output(running[i].type)) {
            live[i].on = false;
            // A component's output too (an LED strip goes dark before a restart)
            if (const Driver* d = driver_of(running[i].type); d != nullptr && d->output != nullptr) {
                d->output(i, running[i], false, live[i].level);
            } else {
                write_output(i);
            }
        }
    }
}

std::string name() { return device_name[0] ? device_name : factory_name ? factory_name : DEFAULT_NAME; }

std::string group_name(int group) {
    return group >= 1 && group <= GROUPS ? group_names[group - 1] : "";
}

// io group <g> [<name...>]: a group's name, the device name controllers show
// for it (empty: the name of its first connection)
std::string cmd_group(const std::vector<std::string>& args) {
    const long g = args.size() >= 3 ? std::strtol(args[2].c_str(), nullptr, 10) : 0;
    if (g < 1 || g > GROUPS) return "io group <1-4> [<name>]\n";
    std::string label;
    for (size_t k = 3; k < args.size(); ++k) label += (label.empty() ? "" : " ") + args[k];
    std::strncpy(group_names[g - 1], label.c_str(), sizeof(group_names[g - 1]) - 1);
    group_names[g - 1][sizeof(group_names[g - 1]) - 1] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return "storing failed\n";
    char key[4] = {'g', static_cast<char>('0' + g), 0};
    nvs_set_str(nvs, key, group_names[g - 1]);
    nvs_commit(nvs);
    nvs_close(nvs);
    return "ok, controllers see it after the next start\n";
}

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
    if (sub == "group") {
        return cmd_group(args);
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
    // Inputs as if they happened, for trying out rules and controls without wiring
    if (sub == "simulate" && args.size() >= 4) {
        const int index = parse_slot(args[2]);
        if (index < 0 || running[index].type == Type::none) return "not a connection in use\n";
        const std::string what = args[3];
        if (what == "press") {
            const long count = args.size() > 4 ? std::strtol(args[4].c_str(), nullptr, 10) : 1;
            if (count < 1 || count > 5) return "press [1-5]\n";
            for (long k = 0; k < count; ++k) {
                emit_button(index, Press::initial, 0);
                emit_button(index, Press::short_release, 0);
                act(index, -1);
            }
            emit_button(index, Press::multi_complete, static_cast<uint8_t>(count));
            return "ok\n";
        }
        if (what == "long") {
            emit_button(index, Press::initial, 0);
            emit_button(index, Press::long_press, 0);
            emit_button(index, Press::long_release, 0);
            return "ok\n";
        }
        if (what == "active" || what == "inactive") {
            const bool active = what == "active";
            live[index].simulated_until = esp_timer_get_time() + 60 * 1000 * MS;  // the pin would undo it at once
            if (live[index].active != active) {
                live[index].active = active;
                emit_input(index, active);
                act(index, active ? 1 : 0);
            }
            return "ok\n";
        }
        if (what == "climate" && args.size() == 6) {
            const long c = std::strtol(args[4].c_str(), nullptr, 10), h = std::strtol(args[5].c_str(), nullptr, 10);
            {
                std::lock_guard<std::mutex> guard(lock);
                live[index].have_climate = true;
                live[index].centi_celsius = static_cast<int16_t>(c);
                live[index].centi_percent = static_cast<uint16_t>(h);
            }
            emit_climate(index, static_cast<int16_t>(c), static_cast<uint16_t>(h));
            return "ok\n";
        }
        return "io simulate <n> press [count]|long|active|inactive|climate <centi °C> <centi %>\n";
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
                      // of all there is: the page shows how full it is
                      ",\"heap_total\":" + std::to_string(heap_caps_get_total_size(MALLOC_CAP_DEFAULT)) +
                      // the state and the BOOT menu (open action, a press held), for the page and the add-on
                      ",\"status\":" + device_status::json() +
                      ",\"io\":{\"name\":" + json_text(name().c_str()) + ",\"trial\":";
    const int64_t left = in_trial ? (trial_end_us - esp_timer_get_time()) / 1000000 : 0;
    out += std::to_string(left > 0 ? left : 0) + ",\"problem\":" + json_text(problem.c_str()) + ",\"changed\":";
    out += std::memcmp(draft, running, sizeof(draft)) != 0 ? "true" : "false";
    {
        std::lock_guard<std::mutex> guard(lock);
        out += ",\"ir_last\":" + json_text(last_ir);
#if CONFIG_BT_ENABLED
        const char *bt = ble_scan::scanning() ? "on" : bluetooth ? "starting" : "off";
#else
        const char *bt = "none";  // this board's firmware has no Bluetooth (classic ESP32, C3)
#endif
        out += std::string(",\"bluetooth\":\"") + bt + "\",\"ble_pairing\":" + (ble_pairing ? "true" : "false");
    }
    out += ",\"pins\":[";
    for (size_t i = 0; i < sizeof(SAFE_PINS); ++i) {
        out += (i ? "," : "") + std::to_string(SAFE_PINS[i]);
    }
    out += "],\"drivers\":[";
    for (int i = 0; i < driver_count; ++i) {
        const Driver& d = drivers[i];
        static const char* MATTER[] = {"on_off", "dimmable", "button", "contact", "occupancy", "climate", "color", "air_quality", "electrical"};
        out += (i ? ",{" : "{") + std::string("\"type\":") + json_text(d.type) + ",\"title\":" + json_text(d.title) +
               ",\"matter\":\"" + MATTER[static_cast<int>(d.matter)] + "\",\"pins\":" + std::to_string(d.pins) +
               ",\"number\":" + (d.number_label ? json_text(d.number_label) : std::string("null")) + "}";
    }
    out += "],\"groups\":[";
    for (int g = 0; g < GROUPS; ++g) out += (g ? "," : "") + json_text(group_names[g]);
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

bool pin_free_for_led(int pin) {
    bool listed = false;
    for (const uint8_t p : SAFE_PINS) listed = listed || p == pin;
    if (!listed) return false;
    std::lock_guard<std::mutex> guard(lock);
    for (int i = 0; i < SLOTS; ++i) {
        for (const Slot* s : {&running[i], &draft[i]}) {
            const int pins = pin_count(s->type);
            if (pins > 0 && (s->pin == pin || (pins == 2 && s->pin2 == pin))) return false;
        }
    }
    return true;
}

}  // namespace io
