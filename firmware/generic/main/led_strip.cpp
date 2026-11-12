// Addressable LED strips as Matter colour lights.
//
// One data pin per strip, driven by the RMT peripheral: WS2812B, WS2811 and
// WS2815 (GRB, 3 bytes an LED) or SK6812 RGBW (GRBW, 4 bytes). The whole strip
// is one light: on/off, brightness and colour (hue and saturation, xy or a
// colour temperature) from any controller. Changes fade over a short time,
// so a strip does not jump between colours.
//
// Written for Matter Builder; the idea of a strip as a light with gamma and
// white from RGB comes from WLED (https://github.com/wled/WLED), no code of it
// is used here.

#include "led_strip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "mb_driver.h"

namespace led_strip {

namespace {

const char* TAG = "led_strip";

constexpr int SLOTS = 8;                // as io::SLOTS
constexpr uint16_t DEFAULT_LEDS = 30;   // when the number is left at 0
constexpr uint16_t MAX_LEDS = 1000;     // 4 KB of memory for RGBW; 30 ms a frame
constexpr uint32_t RESOLUTION_HZ = 10 * 1000 * 1000;  // 0.1 µs a tick
constexpr int64_t FADE_US = 300 * 1000;
constexpr int FRAME_MS = 20;
constexpr uint16_t DEFAULT_MIREDS = 370;  // 2700 K: a warm white until a controller says otherwise

struct Strip {
    bool used = false;
    bool rgbw = false;
    uint16_t leds = 0;
    uint8_t* frame = nullptr;
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    // What was asked for: the colour at full brightness, and on/level
    uint8_t color[4] = {};
    bool on = false;
    uint8_t level = 254;
    // The fade: from what is shown to the target
    float shown[4] = {};
    float from[4] = {};
    float target[4] = {};
    int64_t fade_start = 0;
    bool dirty = false;
};

Strip strips[SLOTS];
std::mutex lock;
TaskHandle_t task = nullptr;
uint8_t gamma_table[256];

// Light output of an LED is linear in its value, what eyes see is not:
// colours in sRGB and brightness levels go through gamma 2.2
void make_gamma() {
    for (int i = 0; i < 256; ++i) {
        gamma_table[i] = static_cast<uint8_t>(std::lround(std::pow(i / 255.0f, 2.2f) * 255.0f));
    }
}

uint8_t clamp8(float v) { return static_cast<uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, v)))); }

void hue_saturation_to_rgb(uint8_t hue, uint8_t saturation, float rgb[3]) {
    const float h = hue * 360.0f / 254.0f / 60.0f, s = saturation / 254.0f;
    const int sector = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);
    const float p = 1 - s, q = 1 - s * f, t = 1 - s * (1 - f);
    const float table[6][3] = {{1, t, p}, {q, 1, p}, {p, 1, t}, {p, q, 1}, {t, p, 1}, {1, p, q}};
    for (int c = 0; c < 3; ++c) rgb[c] = table[sector][c] * 255.0f;
}

// CIE xy to linear sRGB at full brightness (the brightest channel at 255)
void xy_to_rgb(uint16_t x16, uint16_t y16, float rgb[3]) {
    const float x = x16 / 65536.0f, y = std::max(y16 / 65536.0f, 0.0001f);
    const float X = x / y, Y = 1.0f, Z = (1 - x - y) / y;
    float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    r = std::max(r, 0.0f), g = std::max(g, 0.0f), b = std::max(b, 0.0f);
    const float top = std::max({r, g, b, 0.0001f});
    rgb[0] = r / top * 255.0f, rgb[1] = g / top * 255.0f, rgb[2] = b / top * 255.0f;
}

// A colour temperature to sRGB (Tanner Helland's fit of the black body)
void temperature_to_rgb(uint16_t mireds, float rgb[3]) {
    const float t = std::min(400.0f, std::max(10.0f, 1000000.0f / std::max<uint16_t>(mireds, 1) / 100.0f));
    rgb[0] = t <= 66 ? 255 : 329.698727f * std::pow(t - 60, -0.1332047592f);
    rgb[1] = t <= 66 ? 99.4708025861f * std::log(t) - 161.1195681661f : 288.1221695283f * std::pow(t - 60, -0.0755148492f);
    rgb[2] = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231f * std::log(t - 10) - 305.0447927307f;
}

// The colour as the strip's channels at full brightness
void color_of(const io::Color& c, bool rgbw, uint8_t out[4]) {
    float rgb[3] = {};
    bool srgb = true;
    if (c.mode == io::Color::hue_saturation) {
        hue_saturation_to_rgb(c.hue, c.saturation, rgb);
    } else if (c.mode == io::Color::xy) {
        xy_to_rgb(c.x, c.y, rgb);
        srgb = false;  // already linear
    } else {
        temperature_to_rgb(c.mireds, rgb);
    }
    for (int k = 0; k < 3; ++k) out[k] = srgb ? gamma_table[clamp8(rgb[k])] : clamp8(rgb[k]);
    out[3] = 0;
    if (rgbw) {
        // The white LED gives what the three have in common: whiter and brighter
        const uint8_t w = std::min({out[0], out[1], out[2]});
        for (int k = 0; k < 3; ++k) out[k] -= w;
        out[3] = w;
    }
}

// Called with the lock held
void retarget(Strip& s) {
    const float brightness = s.on ? std::max<uint8_t>(1, gamma_table[s.level * 255 / 254]) / 255.0f : 0.0f;
    for (int k = 0; k < 4; ++k) {
        s.from[k] = s.shown[k];
        s.target[k] = s.color[k] * brightness;
    }
    s.fade_start = esp_timer_get_time();
    s.dirty = true;
}

void send(Strip& s) {
    const int bytes = s.rgbw ? 4 : 3;
    uint8_t pixel[4];
    {
        std::lock_guard<std::mutex> guard(lock);
        // GRB(W): the order these chips take their bytes in
        pixel[0] = clamp8(s.shown[1]);
        pixel[1] = clamp8(s.shown[0]);
        pixel[2] = clamp8(s.shown[2]);
        pixel[3] = clamp8(s.shown[3]);
    }
    for (int i = 0; i < s.leds; ++i) std::memcpy(s.frame + i * bytes, pixel, bytes);
    rmt_transmit_config_t config = {};
    if (rmt_transmit(s.channel, s.encoder, s.frame, static_cast<size_t>(s.leds) * bytes, &config) == ESP_OK) {
        rmt_tx_wait_all_done(s.channel, 100);
    }
}

// One task for every strip: a frame when something changed, and while fading.
// The pause between frames (20 ms) is also the reset the LEDs need to latch.
void render_task(void*) {
    while (true) {
        bool fading = false;
        for (Strip& s : strips) {
            if (!s.used) continue;
            bool show = false;
            {
                std::lock_guard<std::mutex> guard(lock);
                if (s.dirty) {
                    const float f = std::min(1.0f, (esp_timer_get_time() - s.fade_start) / static_cast<float>(FADE_US));
                    for (int k = 0; k < 4; ++k) s.shown[k] = s.from[k] + (s.target[k] - s.from[k]) * f;
                    s.dirty = f < 1.0f;
                    fading = fading || s.dirty;
                    show = true;
                }
            }
            if (show) send(s);
        }
        ulTaskNotifyTake(pdTRUE, fading ? pdMS_TO_TICKS(FRAME_MS) : portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1));  // at least the reset time between two frames
    }
}

void wake() {
    if (task != nullptr) xTaskNotifyGive(task);
}

bool setup(int slot, const io::Slot& s, bool rgbw) {
    if (slot < 0 || slot >= SLOTS) return false;
    Strip& strip = strips[slot];
    const uint16_t number = io::number_of(s);
    strip.leds = number == 0 ? DEFAULT_LEDS : std::min(number, MAX_LEDS);
    strip.rgbw = rgbw;
    strip.frame = static_cast<uint8_t*>(heap_caps_calloc(strip.leds, rgbw ? 4 : 3, MALLOC_CAP_8BIT));
    if (strip.frame == nullptr) {
        ESP_LOGE(TAG, "Connection %d: no memory for %u LEDs", slot, strip.leds);
        return false;
    }
    rmt_tx_channel_config_t channel = {};
    channel.gpio_num = static_cast<gpio_num_t>(s.pin);
    channel.clk_src = RMT_CLK_SRC_DEFAULT;
    channel.resolution_hz = RESOLUTION_HZ;
#if CONFIG_IDF_TARGET_ESP32
    // Two memory blocks: fewer refills, so Wi-Fi interrupts do not garble the colours
    channel.mem_block_symbols = 2 * SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
    channel.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#endif
    channel.trans_queue_depth = 1;
    channel.flags.invert_out = (s.flags & io::INVERTED) != 0;  // behind an inverting level shifter
    rmt_bytes_encoder_config_t bits = {};
    // WS2812 timing: a 0 is 0.3 µs high and 0.9 µs low, a 1 the other way round
    bits.bit0.level0 = 1;
    bits.bit0.duration0 = 3;
    bits.bit0.level1 = 0;
    bits.bit0.duration1 = 9;
    bits.bit1.level0 = 1;
    bits.bit1.duration0 = 9;
    bits.bit1.level1 = 0;
    bits.bit1.duration1 = 3;
    bits.flags.msb_first = 1;
    if (rmt_new_tx_channel(&channel, &strip.channel) != ESP_OK ||
        rmt_new_bytes_encoder(&bits, &strip.encoder) != ESP_OK || rmt_enable(strip.channel) != ESP_OK) {
        ESP_LOGE(TAG, "Connection %d: no RMT channel for pin %d (infrared sending uses them too)", slot, s.pin);
        if (strip.encoder != nullptr) rmt_del_encoder(strip.encoder);
        if (strip.channel != nullptr) rmt_del_channel(strip.channel);
        heap_caps_free(strip.frame);
        strip = Strip();
        return false;
    }
    io::Color warm = {};
    warm.mode = io::Color::temperature;
    warm.mireds = DEFAULT_MIREDS;
    {
        std::lock_guard<std::mutex> guard(lock);
        color_of(warm, rgbw, strip.color);
        strip.used = true;
        retarget(strip);  // off: the strip shows black, whatever it held before
    }
    if (task == nullptr) xTaskCreate(render_task, "led_strip", 3072, nullptr, 4, &task);
    wake();
    ESP_LOGI(TAG, "Connection %d: %u %s LEDs on pin %d", slot, strip.leds, rgbw ? "RGBW" : "RGB", s.pin);
    return true;
}

bool setup_rgb(int slot, const io::Slot& s) { return setup(slot, s, false); }
bool setup_rgbw(int slot, const io::Slot& s) { return setup(slot, s, true); }

void output(int slot, const io::Slot&, bool on, uint8_t level) {
    if (slot < 0 || slot >= SLOTS || !strips[slot].used) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        strips[slot].on = on;
        if (level > 0) strips[slot].level = level;
        retarget(strips[slot]);
    }
    wake();
}

void color(int slot, const io::Slot&, const io::Color& c) {
    if (slot < 0 || slot >= SLOTS || !strips[slot].used) return;
    {
        std::lock_guard<std::mutex> guard(lock);
        color_of(c, strips[slot].rgbw, strips[slot].color);
        retarget(strips[slot]);
    }
    wake();
}

}  // namespace

void register_types() {
    make_gamma();
    io::Driver d = {};
    d.type = "led_strip";
    d.title = "LED strip (WS2812)";
    d.matter = io::Matter::color;
    d.pins = 1;
    d.number_label = "LEDs";
    d.setup = setup_rgb;
    d.output = output;
    d.color = color;
    io::register_driver(d);
    d.type = "led_strip_rgbw";
    d.title = "LED strip RGBW (SK6812)";
    d.setup = setup_rgbw;
    io::register_driver(d);
}

}  // namespace led_strip
