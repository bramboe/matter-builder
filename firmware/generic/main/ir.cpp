#include "ir.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ir {

namespace {

const char* TAG = "ir";

constexpr uint32_t RESOLUTION_HZ = 1000000;  // 1 tick = 1 µs
constexpr int SYMBOLS = 48;                  // one RMT memory block on the C6; a NEC frame is 34

// NEC timing in µs: a 9 ms mark and 4.5 ms space, then 32 bits of a 560 µs
// mark and a 560 µs (0) or 1690 µs (1) space, least significant bit first;
// a held key repeats as a 9 ms mark and a 2.25 ms space
constexpr uint16_t LEAD_MARK = 9000, LEAD_SPACE = 4500, REPEAT_SPACE = 2250;
constexpr uint16_t BIT_MARK = 560, ZERO_SPACE = 560, ONE_SPACE = 1690;

struct Sender {
    int pin = -1;
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
};

struct Receiver {
    int pin = -1;
    rmt_channel_handle_t channel = nullptr;
    rmt_symbol_word_t symbols[SYMBOLS] = {};
};

struct Received {
    int receiver;
    size_t count;
};

Sender senders[MAX_PINS];
Receiver receivers[MAX_PINS];
QueueHandle_t received = nullptr;
CodeHandler handler = nullptr;

bool near(uint32_t duration, uint32_t expected) {
    return duration > expected * 7 / 10 && duration < expected * 13 / 10;
}

// The receiver's output is low during a mark, so each symbol is a mark then a space
bool decode(const rmt_symbol_word_t* s, size_t count, uint16_t& address, uint16_t& command) {
    if (count < 33 || !near(s[0].duration0, LEAD_MARK) || !near(s[0].duration1, LEAD_SPACE)) {
        return false;
    }
    uint32_t value = 0;
    for (int bit = 0; bit < 32; ++bit) {
        const rmt_symbol_word_t& b = s[1 + bit];
        if (!near(b.duration0, BIT_MARK)) return false;
        if (near(b.duration1, ONE_SPACE)) {
            value |= 1u << bit;
        } else if (!near(b.duration1, ZERO_SPACE)) {
            return false;
        }
    }
    address = static_cast<uint16_t>(value & 0xFFFF);
    command = static_cast<uint16_t>(value >> 16);
    return true;
}

bool IRAM_ATTR on_received(rmt_channel_handle_t, const rmt_rx_done_event_data_t* data, void* arg) {
    BaseType_t woken = pdFALSE;
    const Received r = {static_cast<int>(reinterpret_cast<intptr_t>(arg)), data->num_symbols};
    xQueueSendFromISR(received, &r, &woken);
    return woken == pdTRUE;
}

rmt_receive_config_t receive_config() {
    rmt_receive_config_t config = {};
    config.signal_range_min_ns = 1250;        // shorter pulses are noise
    config.signal_range_max_ns = 12000000;    // a longer gap ends the frame
    return config;
}

void receive_task(void*) {
    Received r = {};
    while (true) {
        if (xQueueReceive(received, &r, portMAX_DELAY) != pdTRUE) continue;
        Receiver& rx = receivers[r.receiver];
        uint16_t address = 0, command = 0;
        const bool repeat = r.count <= 2 && near(rx.symbols[0].duration0, LEAD_MARK) &&
                            near(rx.symbols[0].duration1, REPEAT_SPACE);
        if (!repeat && decode(rx.symbols, r.count, address, command)) {
            ESP_LOGI(TAG, "Pin %d: nec:%04X:%04X", rx.pin, address, command);
            if (handler != nullptr) handler(rx.pin, address, command);
        }
        const rmt_receive_config_t config = receive_config();
        rmt_receive(rx.channel, rx.symbols, sizeof(rx.symbols), &config);
    }
}

}  // namespace

bool start_send(int pin) {
    for (Sender& s : senders) {
        if (s.pin == pin) return true;
    }
    for (Sender& s : senders) {
        if (s.pin >= 0) continue;
        rmt_tx_channel_config_t config = {};
        config.gpio_num = static_cast<gpio_num_t>(pin);
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.resolution_hz = RESOLUTION_HZ;
        config.mem_block_symbols = SYMBOLS;
        config.trans_queue_depth = 2;
        rmt_carrier_config_t carrier = {};
        carrier.frequency_hz = 38000;
        carrier.duty_cycle = 0.33f;
        rmt_copy_encoder_config_t encoder = {};
        if (rmt_new_tx_channel(&config, &s.channel) != ESP_OK || rmt_apply_carrier(s.channel, &carrier) != ESP_OK ||
            rmt_new_copy_encoder(&encoder, &s.encoder) != ESP_OK || rmt_enable(s.channel) != ESP_OK) {
            ESP_LOGE(TAG, "Pin %d: no IR send channel", pin);
            return false;
        }
        s.pin = pin;
        return true;
    }
    ESP_LOGE(TAG, "Pin %d: at most %d IR send pins", pin, MAX_PINS);
    return false;
}

bool send_nec(int pin, uint16_t address, uint16_t command) {
    Sender* sender = nullptr;
    for (Sender& s : senders) {
        if (s.pin == pin) sender = &s;
    }
    if (sender == nullptr) return false;
    rmt_symbol_word_t frame[34] = {};
    frame[0] = {LEAD_MARK, 1, LEAD_SPACE, 0};
    const uint32_t value = address | (static_cast<uint32_t>(command) << 16);
    for (int bit = 0; bit < 32; ++bit) {
        frame[1 + bit] = {BIT_MARK, 1, (value >> bit) & 1 ? ONE_SPACE : ZERO_SPACE, 0};
    }
    frame[33] = {BIT_MARK, 1, 0, 0};
    rmt_transmit_config_t config = {};
    if (rmt_transmit(sender->channel, sender->encoder, frame, sizeof(frame), &config) != ESP_OK) {
        return false;
    }
    return rmt_tx_wait_all_done(sender->channel, 200) == ESP_OK;
}

bool start_receive(int pin, CodeHandler on_code) {
    handler = on_code;
    for (Receiver& r : receivers) {
        if (r.pin == pin) return true;
    }
    if (received == nullptr) {
        received = xQueueCreate(4, sizeof(Received));
        xTaskCreate(receive_task, "ir", 3072, nullptr, 5, nullptr);
    }
    for (int i = 0; i < MAX_PINS; ++i) {
        Receiver& r = receivers[i];
        if (r.pin >= 0) continue;
        rmt_rx_channel_config_t config = {};
        config.gpio_num = static_cast<gpio_num_t>(pin);
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.resolution_hz = RESOLUTION_HZ;
        config.mem_block_symbols = SYMBOLS;
        rmt_rx_event_callbacks_t callbacks = {};
        callbacks.on_recv_done = on_received;
        const rmt_receive_config_t receive = receive_config();
        if (rmt_new_rx_channel(&config, &r.channel) != ESP_OK ||
            rmt_rx_register_event_callbacks(r.channel, &callbacks, reinterpret_cast<void*>(static_cast<intptr_t>(i))) != ESP_OK ||
            rmt_enable(r.channel) != ESP_OK || rmt_receive(r.channel, r.symbols, sizeof(r.symbols), &receive) != ESP_OK) {
            ESP_LOGE(TAG, "Pin %d: no IR receive channel", pin);
            return false;
        }
        r.pin = pin;
        return true;
    }
    ESP_LOGE(TAG, "Pin %d: at most %d IR receive pins", pin, MAX_PINS);
    return false;
}

}  // namespace ir
