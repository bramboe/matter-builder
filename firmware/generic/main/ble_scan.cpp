#include "ble_scan.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

extern "C" void ble_store_config_init(void);

namespace ble_scan {

namespace {

const char* TAG = "ble";

Handler handler = nullptr;
uint8_t own_addr_type = 0;
bool shared_stack = false;
volatile bool is_scanning = false;
bool listen = true;          // some connection listens to advertisements
volatile int paused = 0;     // a connection is being made (ble_device)

constexpr uint16_t BTHOME_UUID = 0xFCD2;
constexpr uint16_t ENVIRONMENT_UUID = 0x181A;  // pvvx and ATC1441 custom formats

int16_t s16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// BTHome v2: a device info byte, then objects of an id and a value whose
// length the id decides; an unknown id ends the parsing
void parse_bthome(const uint8_t* p, int len, Advert& a) {
    if (len < 1 || (p[0] & 0x01)) return;  // encrypted: not supported
    static const struct { uint8_t id; uint8_t size; } SIZES[] = {
        {0x00, 1}, {0x01, 1}, {0x02, 2}, {0x03, 2}, {0x04, 3}, {0x05, 3}, {0x08, 2}, {0x0C, 2},
        {0x10, 1}, {0x11, 1}, {0x12, 2}, {0x13, 2}, {0x14, 2}, {0x15, 1}, {0x16, 1}, {0x17, 1},
        {0x1A, 1}, {0x20, 1}, {0x21, 1}, {0x2D, 1}, {0x2E, 1}, {0x2F, 1}, {0x3A, 1}, {0x3F, 2},
        {0x40, 2}, {0x45, 2}, {0x46, 1}, {0x4A, 2}, {0x51, 2}, {0x5E, 2}, {0x5F, 2},
    };
    for (int i = 1; i < len;) {
        const uint8_t id = p[i];
        int size = -1;
        for (const auto& s : SIZES) {
            if (s.id == id) size = s.size;
        }
        if (size < 0 || i + 1 + size > len) return;
        const uint8_t* v = p + i + 1;
        if (id == 0x01) a.battery = static_cast<int8_t>(v[0]);
        if (id == 0x02) { a.centi_celsius = s16(v); a.has_climate = true; }
        if (id == 0x45) { a.centi_celsius = static_cast<int16_t>(s16(v) * 10); a.has_climate = true; }
        if (id == 0x03) a.centi_percent = u16(v);
        if (id == 0x2E) a.centi_percent = static_cast<uint16_t>(v[0] * 100);
        i += 1 + size;
    }
}

// pvvx (15 bytes, little endian, 0.01 units) and ATC1441 (13 bytes, big endian)
void parse_environment(const uint8_t* p, int len, Advert& a) {
    if (len >= 15) {
        a.centi_celsius = s16(p + 6);
        a.centi_percent = u16(p + 8);
        a.battery = static_cast<int8_t>(p[12]);
        a.has_climate = true;
    } else if (len == 13) {
        a.centi_celsius = static_cast<int16_t>(static_cast<int16_t>((p[6] << 8) | p[7]) * 10);
        a.centi_percent = static_cast<uint16_t>(p[8] * 100);
        a.battery = static_cast<int8_t>(p[9]);
        a.has_climate = true;
    }
}

void on_advert(const ble_gap_disc_desc& disc) {
    Advert a = {};
    for (int i = 0; i < 6; ++i) a.mac[i] = disc.addr.val[5 - i];  // NimBLE keeps it little endian
    a.rssi = disc.rssi;
    a.battery = -1;
    // Advertising data: length, type, payload; type 0x16 is service data
    for (int i = 0; i + 1 < disc.length_data;) {
        const int len = disc.data[i];
        if (len == 0 || i + 1 + len > disc.length_data) break;
        const uint8_t type = disc.data[i + 1];
        const uint8_t* payload = disc.data + i + 2;
        if (type == 0x16 && len >= 3) {
            const uint16_t uuid = u16(payload);
            if (uuid == BTHOME_UUID) parse_bthome(payload + 2, len - 3, a);
            if (uuid == ENVIRONMENT_UUID) parse_environment(payload + 2, len - 3, a);
        }
        i += 1 + len;
    }
    if (handler != nullptr) handler(a);
}

int on_gap(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        on_advert(event->disc);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        is_scanning = false;
        if (paused == 0) ESP_LOGW(TAG, "Scan stopped (%d)", event->disc_complete.reason);
    }
    return 0;
}

void scan() {
    if (!listen || paused > 0) return;
    ble_gap_disc_params params = {};
    params.itvl = 0x140;          // 200 ms
    params.window = 0x50;         // 50 ms: Thread gets the rest of the radio time
    params.passive = 1;           // listening only
    params.filter_duplicates = 0; // every reading, not just the first
    const int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, on_gap, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Starting the scan failed (%d)", rc);
        return;
    }
    is_scanning = true;
}

void on_sync() {
    ble_hs_id_infer_auto(0, &own_addr_type);
    scan();
    if (listen) ESP_LOGI(TAG, "Listening for Bluetooth advertisements");
}

void on_reset(int reason) { ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason); }

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Matter starts and stops its Bluetooth layer on its own schedule: wait for it
void start_task(void*) {
    for (int i = 0; i < 300; ++i) {  // up to a minute
        vTaskDelay(pdMS_TO_TICKS(200));
        if (shared_stack && ble_hs_synced()) {
            ble_hs_id_infer_auto(0, &own_addr_type);
            scan();  // passive scanning next to Matter's advertising
            if (listen) ESP_LOGI(TAG, "Listening for Bluetooth advertisements (shared with Matter pairing)");
            vTaskDelete(nullptr);
        }
        if (!shared_stack && !ble_hs_is_enabled() && i >= 10) {
            if (nimble_port_init() != ESP_OK) {
                ESP_LOGE(TAG, "Bluetooth unavailable");
                vTaskDelete(nullptr);
            }
            ble_hs_cfg.sync_cb = on_sync;
            ble_hs_cfg.reset_cb = on_reset;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
            ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
            ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
            ble_store_config_init();
            nimble_port_freertos_init(host_task);
            vTaskDelete(nullptr);
        }
    }
    ESP_LOGE(TAG, "Bluetooth did not become free: no scanning");
    vTaskDelete(nullptr);
}

}  // namespace

void start(Handler on_advert, bool shared, bool listen_to_adverts) {
    handler = on_advert;
    shared_stack = shared;
    listen = listen_to_adverts;
    xTaskCreate(start_task, "ble_start", 3072, nullptr, 4, nullptr);
}

bool scanning() { return is_scanning; }

bool ready() { return ble_hs_synced(); }

uint8_t own_address_type() { return own_addr_type; }

// Scanning and connecting share the radio: the scan stops while a connection
// is made and its services are found, then goes on
void pause() {
    if (paused++ == 0 && is_scanning) {
        ble_gap_disc_cancel();
        is_scanning = false;
    }
}

void resume() {
    if (paused > 0 && --paused == 0) scan();
}

}  // namespace ble_scan
