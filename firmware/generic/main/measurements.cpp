#include "measurements.h"

#include <algorithm>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "io.h"
#include "nvs.h"

namespace measurements {
namespace {

constexpr int64_t SECOND = 1000000;
constexpr uint8_t SCD4X_ADDRESS = 0x62;
constexpr uint16_t STOP_PERIODIC = 0x3F86;
constexpr uint16_t START_PERIODIC = 0x21B1;
constexpr uint16_t READ_MEASUREMENT = 0xEC05;

enum class ScdStep { stop, start, request, read };
struct ScdState {
    i2c_master_dev_handle_t device = nullptr;
    ScdStep step = ScdStep::stop;
    int64_t next = 0;
    uint8_t failures = 0;
};

ScdState scd[io::SLOTS];

struct PulseState {
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    uint64_t count = 0;
    uint64_t saved = 0;
    bool ready = false;
    int64_t latest = 0;
    int64_t interval = 0;
    int64_t next = 0;
    int64_t save_at = 0;
    char key[16] = {};
};

PulseState pulses[io::SLOTS];

bool scd_command(i2c_master_dev_handle_t device, uint16_t command) {
    const uint8_t data[] = {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command)};
    return i2c_master_transmit(device, data, sizeof(data), 50) == ESP_OK;
}

bool setup_scd(int slot, const io::Slot& s) {
    auto& state = scd[slot];
    state.device = i2c_device(s.pin, s.pin2, SCD4X_ADDRESS);
    state.next = esp_timer_get_time() + SECOND;  // allow the sensor to power up
    return state.device != nullptr;
}

void poll_scd(int slot, const io::Slot&, int64_t now) {
    auto& s = scd[slot];
    if (!s.device || now < s.next) return;
    switch (s.step) {
        case ScdStep::stop:
            // Also works when only the ESP32 rebooted and the sensor kept measuring.
            scd_command(s.device, STOP_PERIODIC);
            s.step = ScdStep::start;
            s.next = now + SECOND / 2;
            break;
        case ScdStep::start:
            s.step = scd_command(s.device, START_PERIODIC) ? ScdStep::request : ScdStep::stop;
            s.next = now + 6 * SECOND;
            break;
        case ScdStep::request:
            if (scd_command(s.device, READ_MEASUREMENT)) {
                s.step = ScdStep::read;
                s.next = now + 1000;
                break;
            }
            if (++s.failures >= 3) {
                s.step = ScdStep::stop;
                s.failures = 0;
                io::report_measurement(slot, 0, -1);
            }
            s.next = now + SECOND;
            break;
        case ScdStep::read: {
            uint8_t data[9] = {};
            const bool valid = i2c_master_receive(s.device, data, sizeof(data), 50) == ESP_OK &&
                               sensirion_crc(data) == data[2] && sensirion_crc(data + 3) == data[5] &&
                               sensirion_crc(data + 6) == data[8];
            s.step = ScdStep::request;
            s.next = now + 5 * SECOND;
            if (!valid) {
                if (++s.failures >= 3) {
                    s.step = ScdStep::stop;
                    s.failures = 0;
                    io::report_measurement(slot, 0, -1);
                }
                break;
            }
            s.failures = 0;
            const uint16_t ppm = (data[0] << 8) | data[1];
            const uint32_t temperature = (data[3] << 8) | data[4];
            const uint32_t humidity = (data[6] << 8) | data[7];
            if (ppm != 0) {
                const auto centi_celsius = -4500 + static_cast<int32_t>(17500 * temperature / 65535);
                io::report_climate(slot, centi_celsius, 10000 * humidity / 65535);
            }
            io::report_measurement(slot, 0, ppm == 0 ? -1 : ppm);
            break;
        }
    }
}

void on_pulse(void* arg) {
    auto& s = *static_cast<PulseState*>(arg);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s.lock);
    if (s.latest == 0 || now - s.latest >= 10000) {
        s.interval = s.latest == 0 ? 0 : now - s.latest;
        s.latest = now;
        ++s.count;
    }
    portEXIT_CRITICAL_ISR(&s.lock);
}

bool setup_pulse(int slot, const io::Slot& config) {
    auto& s = pulses[slot];
    const unsigned rate = io::number_of(config) ? io::number_of(config) : 1000;
    // Keep a separate total if the slot, input pin or meter calibration changes.
    std::snprintf(s.key, sizeof(s.key), "%d_%u_%u", slot, config.pin, rate);
    nvs_handle_t nvs;
    if (nvs_open("pulse_energy", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u64(nvs, s.key, &s.count);
        nvs_close(nvs);
    }
    s.saved = s.count;
    s.save_at = esp_timer_get_time() + 60 * SECOND;

    const bool rising = config.flags & io::INVERTED;
    gpio_config_t pin = {};
    pin.pin_bit_mask = 1ULL << config.pin;
    pin.mode = GPIO_MODE_INPUT;
    pin.pull_up_en = rising ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    pin.pull_down_en = rising ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    pin.intr_type = rising ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
    if (gpio_config(&pin) != ESP_OK) return false;
    const esp_err_t installed = gpio_install_isr_service(0);
    s.ready = (installed == ESP_OK || installed == ESP_ERR_INVALID_STATE) &&
              gpio_isr_handler_add(static_cast<gpio_num_t>(config.pin), on_pulse, &s) == ESP_OK;
    return s.ready;
}

void poll_pulse(int slot, const io::Slot& config, int64_t now) {
    auto& s = pulses[slot];
    if (!s.ready || now < s.next) return;
    s.next = now + SECOND;
    portENTER_CRITICAL(&s.lock);
    const uint64_t count = s.count;
    const int64_t latest = s.latest;
    const int64_t interval = s.interval;
    portEXIT_CRITICAL(&s.lock);
    const int64_t rate = io::number_of(config) ? io::number_of(config) : 1000;
    // A rate needs two pulses. Report zero only after an established rate has gone stale.
    const int64_t power = interval <= 0 ? -1 : now - latest <= std::max(120 * SECOND, 2 * interval)
                                             ? 3600000000000000LL / rate / interval : 0;
    const int64_t energy = (count / rate) * 1000000 + (count % rate) * 1000000 / rate;
    io::report_measurement(slot, 0, power);   // milliwatts
    io::report_measurement(slot, 1, energy);  // milliwatt-hours
    if (now >= s.save_at && count != s.saved) {
        s.save_at = now + 60 * SECOND;
        nvs_handle_t nvs;
        if (nvs_open("pulse_energy", NVS_READWRITE, &nvs) == ESP_OK) {
            if (nvs_set_u64(nvs, s.key, count) == ESP_OK && nvs_commit(nvs) == ESP_OK) s.saved = count;
            nvs_close(nvs);
        }
    }
}

}  // namespace

i2c_master_dev_handle_t i2c_device(int sda, int scl, uint8_t address) {
    i2c_master_bus_config_t config = {};
    config.i2c_port = -1;
    config.sda_io_num = static_cast<gpio_num_t>(sda);
    config.scl_io_num = static_cast<gpio_num_t>(scl);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&config, &bus) != ESP_OK) return nullptr;
    i2c_device_config_t device = {};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = address;
    device.scl_speed_hz = 100000;
    i2c_master_dev_handle_t handle = nullptr;
    if (i2c_master_bus_add_device(bus, &device, &handle) != ESP_OK) {
        i2c_del_master_bus(bus);
        return nullptr;
    }
    return handle;
}

uint8_t sensirion_crc(const uint8_t* word) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
        crc ^= word[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc << 1) ^ ((crc & 0x80) ? 0x31 : 0);
    }
    return crc;
}

void register_types() {
    io::register_driver({"scd4x", "SCD4x CO2 sensor", io::Matter::air_quality, 2, nullptr,
                         setup_scd, poll_scd, nullptr, nullptr});
    io::register_driver({"pulse_meter", "Energy pulse meter", io::Matter::electrical, 1, "Pulses per kWh",
                         setup_pulse, poll_pulse, nullptr, nullptr});
}

}  // namespace measurements
