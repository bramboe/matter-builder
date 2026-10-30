#include "bed_link.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "octo_protocol.h"

namespace bed_link {

namespace {

const char* TAG = "bed";

// Timing, matching the Home Assistant integration (octo_bed_client.py)
constexpr uint32_t MANAGER_INTERVAL_MS = 500;
constexpr uint32_t MOTION_INTERVAL_MS = 375;      // bed remote repeats commands every ~375 ms
constexpr uint32_t HOST_TIMEOUT_MS = 10000;       // run_in_host watchdog
constexpr uint32_t KEEPALIVE_INTERVAL_MS = 25000;  // bed drops the link after ~30 s without PIN
constexpr uint32_t STATS_INTERVAL_MS = 300000;
constexpr int64_t SEEN_RECENTLY_US = 10 * 1000000LL;
constexpr int64_t LIVENESS_TIMEOUT_US = (25 * 3 + 5) * 1000000LL;  // three missed keep-alives
constexpr int64_t PIN_VERIFY_TIMEOUT_US = 8 * 1000000LL;
constexpr int64_t PIN_REJECTED_BACKOFF_US = 60 * 1000000LL;
constexpr int64_t CONNECT_SETTLE_US = 1 * 1000000LL;
constexpr int32_t CONNECT_TIMEOUT_MS = 15000;
// Backstop for a connect whose timeout event never arrives
constexpr int64_t CONNECT_STUCK_US = (CONNECT_TIMEOUT_MS + 5000) * 1000LL;
constexpr int64_t RECONNECT_DELAYS_US[] = {2000000, 5000000, 10000000, 20000000, 30000000, 60000000};
constexpr uint16_t NO_CONN = 0xFFFF;
constexpr int SCAN_CACHE_SIZE = 16;

enum class State { Idle, Connecting, Discovering, Authenticating, Ready };

const char* state_name(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::Connecting: return "connecting";
        case State::Discovering: return "discovering";
        case State::Authenticating: return "authenticating";
        case State::Ready: return "ready";
    }
    return "?";
}

// One motor's movement. Both motors move together when they go the same
// way; in opposite directions the earlier run goes first, because one
// command cannot drive the motors in opposite directions.
struct Run {
    uint32_t id = 0;  // 0: never moved; higher ids were requested later
    bool pending = false;
    bool up = false;
    bool driving = false;  // part of the last command sent
    int64_t remaining_us = 0;
    int64_t done_us = 0;  // time actually driven
};

uint32_t last_run_id = 0;

struct Bed {
    bool configured = false;
    bool verified = false;  // the bed has accepted this PIN at least once
    uint8_t mac[6] = {};  // display order, e.g. AA:BB:CC:DD:EE:01
    char pin[5] = {};
    char name[24] = {};

    // Presence (from advertisements)
    ble_addr_t addr = {};
    bool seen = false;
    int8_t rssi = 0;
    int64_t last_adv_us = 0;

    // Connection
    State state = State::Idle;
    uint16_t conn = NO_CONN;
    uint16_t svc_start = 0;
    uint16_t svc_end = 0;
    uint16_t chr_val = 0;
    uint16_t chr_end = 0;
    uint16_t cccd = 0;
    bool write_no_rsp = true;
    int failures = 0;
    int64_t next_attempt_us = 0;
    int64_t connect_started_us = 0;
    int64_t auth_started_us = 0;
    int64_t last_ack_us = 0;
    int64_t connected_since_us = 0;

    // Calibration (travel times in tenths of a second)
    uint16_t head_travel_ds = 300;
    uint16_t feet_travel_ds = 300;
    bool calibrating = false;
    Part calibrate_part = Part::Head;
    int64_t calibrate_started_us = 0;

    // Features
    bool features_done = false;
    int motor_count = -1;
    int memory_slots = 0;
    bool has_light = false;
    bool has_rgbw = false;
    bool has_synchro = false;

    // Motion: one run per motor, driven by motion_tick
    Run head_run;
    Run feet_run;
    bool motion_sent = false;  // a motor command went out since the last stop
    int64_t last_motion_us = 0;

    // Statistics for the 24 h stability test
    uint32_t connects = 0;
    uint32_t disconnects = 0;
    uint32_t pin_rejects = 0;
    int last_disconnect_reason = 0;
};

struct ScanEntry {
    ble_addr_t addr;
    char name[20];
    int8_t rssi;
    bool octo_service;
    int64_t last_seen_us;
};

Bed beds[MAX_BEDS];
ScanEntry scan_cache[SCAN_CACHE_SIZE];
int scan_cache_used = 0;
uint8_t own_addr_type = 0;
bool synced = false;
bed_link::CalibratedCallback calibrated_callback;
bool connect_in_progress = false;
int64_t next_connect_allowed_us = 0;

ble_npl_callout manager_co;
ble_npl_callout motion_co;
ble_npl_callout keepalive_co;
ble_npl_callout stats_co;

ble_uuid16_t uuid16(uint16_t value) {
    ble_uuid16_t uuid;
    uuid.u.type = BLE_UUID_TYPE_16;
    uuid.value = value;
    return uuid;
}

const ble_uuid16_t UUID_SERVICE = uuid16(0xFFE0);
const ble_uuid16_t UUID_COMMAND = uuid16(0xFFE1);
const ble_uuid16_t UUID_CCCD = uuid16(0x2902);

int64_t now_us() { return esp_timer_get_time(); }

// Forget all movement, e.g. when the link drops: the bed stops by itself
// once the repeated commands stop.
void end_runs(Bed& bed) {
    for (Run* run : {&bed.head_run, &bed.feet_run}) {
        run->pending = false;
        run->driving = false;
    }
    bed.motion_sent = false;
}

std::string mac_string(const uint8_t mac[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return buf;
}

std::string addr_string(const ble_addr_t& addr) {
    // NimBLE stores addresses least significant byte first
    const uint8_t mac[6] = {addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]};
    return mac_string(mac);
}

bool addr_matches(const ble_addr_t& addr, const uint8_t mac[6]) {
    for (int i = 0; i < 6; ++i) {
        if (addr.val[i] != mac[5 - i]) {
            return false;
        }
    }
    return true;
}

Bed* bed_by_conn(uint16_t conn) {
    for (auto& bed : beds) {
        if (bed.conn == conn && conn != NO_CONN) {
            return &bed;
        }
    }
    return nullptr;
}

int index_of(const Bed* bed) { return static_cast<int>(bed - beds); }

std::string hex(const uint8_t* data, size_t len) {
    std::string out;
    char buf[3];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

__attribute__((format(printf, 2, 3))) void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

// Recent connection events, readable over the network for the stability test
constexpr int EVENT_LOG_SIZE = 32;
char event_log[EVENT_LOG_SIZE][96];
int event_next = 0;

__attribute__((format(printf, 1, 2))) void log_event(const char* fmt, ...) {
    char* line = event_log[event_next];
    int used = std::snprintf(line, sizeof(event_log[0]), "[%" PRId64 "s] ", now_us() / 1000000);
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line + used, sizeof(event_log[0]) - used, fmt, args);
    va_end(args);
    event_next = (event_next + 1) % EVENT_LOG_SIZE;
    ESP_LOGI(TAG, "%s", line);
}

// ------------------------------------------------------------------ storage

void store_save(int index) {
    nvs_handle_t nvs;
    if (nvs_open("octo", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS");
        return;
    }
    char key[8];
    const Bed& bed = beds[index];
    std::snprintf(key, sizeof(key), "b%dmac", index);
    if (bed.configured) {
        nvs_set_blob(nvs, key, bed.mac, sizeof(bed.mac));
        std::snprintf(key, sizeof(key), "b%dpin", index);
        nvs_set_str(nvs, key, bed.pin);
        std::snprintf(key, sizeof(key), "b%dname", index);
        nvs_set_str(nvs, key, bed.name);
        std::snprintf(key, sizeof(key), "b%dth", index);
        nvs_set_u16(nvs, key, bed.head_travel_ds);
        std::snprintf(key, sizeof(key), "b%dtf", index);
        nvs_set_u16(nvs, key, bed.feet_travel_ds);
        std::snprintf(key, sizeof(key), "b%dok", index);
        nvs_set_u8(nvs, key, bed.verified ? 1 : 0);
    } else {
        nvs_erase_key(nvs, key);
        std::snprintf(key, sizeof(key), "b%dpin", index);
        nvs_erase_key(nvs, key);
        std::snprintf(key, sizeof(key), "b%dname", index);
        nvs_erase_key(nvs, key);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

void store_load() {
    nvs_handle_t nvs;
    if (nvs_open("octo", NVS_READONLY, &nvs) != ESP_OK) {
        return;  // nothing stored yet
    }
    for (int i = 0; i < MAX_BEDS; ++i) {
        Bed& bed = beds[i];
        char key[8];
        size_t len = sizeof(bed.mac);
        std::snprintf(key, sizeof(key), "b%dmac", i);
        if (nvs_get_blob(nvs, key, bed.mac, &len) != ESP_OK || len != sizeof(bed.mac)) {
            continue;
        }
        len = sizeof(bed.pin);
        std::snprintf(key, sizeof(key), "b%dpin", i);
        if (nvs_get_str(nvs, key, bed.pin, &len) != ESP_OK) {
            continue;
        }
        len = sizeof(bed.name);
        std::snprintf(key, sizeof(key), "b%dname", i);
        if (nvs_get_str(nvs, key, bed.name, &len) != ESP_OK) {
            std::snprintf(bed.name, sizeof(bed.name), "Bed %d", i);
        }
        std::snprintf(key, sizeof(key), "b%dth", i);
        nvs_get_u16(nvs, key, &bed.head_travel_ds);  // keeps the default if absent
        std::snprintf(key, sizeof(key), "b%dtf", i);
        nvs_get_u16(nvs, key, &bed.feet_travel_ds);
        uint8_t verified = 0;
        std::snprintf(key, sizeof(key), "b%dok", i);
        nvs_get_u8(nvs, key, &verified);
        bed.verified = verified != 0;
        bed.configured = true;
        ESP_LOGI(TAG, "Bed %d '%s' %s loaded", i, bed.name, mac_string(bed.mac).c_str());
    }
    nvs_close(nvs);
}

// ------------------------------------------------------------------ writes

bool write(Bed& bed, const octo::Bytes& packet) {
    if (bed.conn == NO_CONN || bed.chr_val == 0) {
        return false;
    }
    int rc = bed.write_no_rsp
                 ? ble_gattc_write_no_rsp_flat(bed.conn, bed.chr_val, packet.data(), packet.size())
                 : ble_gattc_write_flat(bed.conn, bed.chr_val, packet.data(), packet.size(), nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "Bed %d: write failed rc=%d", index_of(&bed), rc);
        return false;
    }
    if (octo::is_pin_packet(packet.data(), packet.size())) {
        ESP_LOGD(TAG, "Bed %d: -> PIN (masked)", index_of(&bed));
    } else {
        ESP_LOGD(TAG, "Bed %d: -> %s", index_of(&bed), hex(packet.data(), packet.size()).c_str());
    }
    return true;
}

void send_pin(Bed& bed) {
    auto packet = octo::encode_pin(bed.pin);
    if (packet) {
        write(bed, *packet);
    }
}

void terminate(Bed& bed, const char* why) {
    if (bed.conn == NO_CONN) {
        return;
    }
    log_event("bed %d dropping link: %s", index_of(&bed), why);
    ble_gap_terminate(bed.conn, BLE_ERR_REM_USER_CONN_TERM);
}

// Connect attempt failed before the link was usable: back off.
void connect_failed(Bed& bed) {
    const int step = bed.failures < 5 ? bed.failures : 5;
    bed.next_attempt_us = now_us() + RECONNECT_DELAYS_US[step];
    bed.failures++;
}

void release_connect_gate() {
    connect_in_progress = false;
    next_connect_allowed_us = now_us() + CONNECT_SETTLE_US;
}

// ------------------------------------------------------------------ scanning

int gap_event(ble_gap_event* event, void* arg);

void remember_scan(const ble_gap_disc_desc& disc, const ble_hs_adv_fields& fields) {
    bool octo_service = false;
    for (int i = 0; i < fields.num_uuids16; ++i) {
        if (fields.uuids16[i].value == 0xFFE0) {
            octo_service = true;
        }
    }
    if (fields.name_len == 0 && !octo_service) {
        return;  // keep the list readable: only named devices and Octo beds
    }
    ScanEntry* entry = nullptr;
    for (int i = 0; i < scan_cache_used; ++i) {
        if (ble_addr_cmp(&scan_cache[i].addr, &disc.addr) == 0) {
            entry = &scan_cache[i];
        }
    }
    if (entry == nullptr) {
        if (scan_cache_used < SCAN_CACHE_SIZE) {
            entry = &scan_cache[scan_cache_used++];
        } else {
            entry = &scan_cache[0];  // replace the stalest entry
            for (int i = 1; i < SCAN_CACHE_SIZE; ++i) {
                if (scan_cache[i].last_seen_us < entry->last_seen_us) {
                    entry = &scan_cache[i];
                }
            }
        }
        std::memset(entry, 0, sizeof(*entry));
        entry->addr = disc.addr;
    }
    if (fields.name_len > 0) {
        const size_t len = fields.name_len < sizeof(entry->name) - 1 ? fields.name_len : sizeof(entry->name) - 1;
        std::memcpy(entry->name, fields.name, len);
        entry->name[len] = '\0';
    }
    entry->octo_service = entry->octo_service || octo_service;
    entry->rssi = disc.rssi;
    entry->last_seen_us = now_us();
}

void on_advertisement(const ble_gap_disc_desc& disc) {
    ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data) == 0) {
        remember_scan(disc, fields);
    }
    for (auto& bed : beds) {
        if (bed.configured && addr_matches(disc.addr, bed.mac)) {
            if (!bed.seen) {
                ESP_LOGI(TAG, "Bed %d '%s' advertising (rssi %d)", index_of(&bed), bed.name, disc.rssi);
            }
            bed.addr = disc.addr;
            bed.seen = true;
            bed.rssi = disc.rssi;
            bed.last_adv_us = now_us();
        }
    }
}

void ensure_scanning() {
    if (!synced || connect_in_progress || ble_gap_disc_active()) {
        return;
    }
    ble_gap_disc_params params = {};
    params.itvl = 0x60;    // 60 ms
    params.window = 0x30;  // 30 ms: leave air time for the links (and later Wi-Fi)
    params.passive = 0;    // active: the bed's name is in the scan response
    params.filter_duplicates = 0;
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Starting scan failed rc=%d", rc);
    }
}

// ------------------------------------------------------------------ GATT setup chain

void gatt_failed(Bed& bed, const char* step, int status) {
    log_event("bed %d %s failed (status %d)", index_of(&bed), step, status);
    terminate(bed, step);
}

int on_cccd_written(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    Bed* bed = bed_by_conn(conn);
    if (bed == nullptr) {
        return 0;
    }
    if (error->status != 0) {
        gatt_failed(*bed, "enabling notifications", error->status);
        return 0;
    }
    bed->state = State::Authenticating;
    bed->auth_started_us = now_us();
    bed->last_ack_us = now_us();
    release_connect_gate();
    send_pin(*bed);
    return 0;
}

void enable_notifications(Bed& bed) {
    if (bed.cccd == 0) {
        gatt_failed(bed, "finding the notification descriptor", BLE_HS_ENOENT);
        return;
    }
    static const uint8_t enable[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(bed.conn, bed.cccd, enable, sizeof(enable), on_cccd_written, nullptr);
    if (rc != 0) {
        gatt_failed(bed, "enabling notifications", rc);
    }
}

int on_descriptor(uint16_t conn, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void*) {
    Bed* bed = bed_by_conn(conn);
    if (bed == nullptr) {
        return 0;
    }
    if (error->status == 0) {
        if (bed->cccd == 0 && ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u) == 0) {
            bed->cccd = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        enable_notifications(*bed);
    } else {
        gatt_failed(*bed, "descriptor discovery", error->status);
    }
    return 0;
}

int on_characteristic(uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
    Bed* bed = bed_by_conn(conn);
    if (bed == nullptr) {
        return 0;
    }
    if (error->status == 0) {
        if (bed->chr_val == 0 && ble_uuid_cmp(&chr->uuid.u, &UUID_COMMAND.u) == 0) {
            bed->chr_val = chr->val_handle;
            bed->write_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
        } else if (bed->chr_val != 0 && bed->chr_end == 0) {
            bed->chr_end = chr->def_handle - 1;  // our characteristic ends where the next starts
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        gatt_failed(*bed, "characteristic discovery", error->status);
        return 0;
    }
    if (bed->chr_val == 0) {
        gatt_failed(*bed, "finding characteristic FFE1", BLE_HS_ENOENT);
        return 0;
    }
    const uint16_t end = bed->chr_end != 0 ? bed->chr_end : bed->svc_end;
    if (end <= bed->chr_val) {
        enable_notifications(*bed);
        return 0;
    }
    int rc = ble_gattc_disc_all_dscs(conn, bed->chr_val, end, on_descriptor, nullptr);
    if (rc != 0) {
        gatt_failed(*bed, "descriptor discovery", rc);
    }
    return 0;
}

int on_service(uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    Bed* bed = bed_by_conn(conn);
    if (bed == nullptr) {
        return 0;
    }
    if (error->status == 0) {
        bed->svc_start = service->start_handle;
        bed->svc_end = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || bed->svc_end == 0) {
        gatt_failed(*bed, "finding service FFE0", error->status);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, bed->svc_start, bed->svc_end, on_characteristic, nullptr);
    if (rc != 0) {
        gatt_failed(*bed, "characteristic discovery", rc);
    }
    return 0;
}

int on_mtu(uint16_t conn, const ble_gatt_error* error, uint16_t mtu, void*) {
    Bed* bed = bed_by_conn(conn);
    if (bed == nullptr) {
        return 0;
    }
    ESP_LOGD(TAG, "Bed %d: MTU %u (status %d)", index_of(bed), mtu, error->status);
    // Continue regardless: all packets used here fit the default MTU
    int rc = ble_gattc_disc_svc_by_uuid(conn, &UUID_SERVICE.u, on_service, nullptr);
    if (rc != 0) {
        gatt_failed(*bed, "service discovery", rc);
    }
    return 0;
}

// ------------------------------------------------------------------ notifications

void on_feature(Bed& bed, const octo::Bytes& data) {
    auto feature = octo::extract_feature(data);
    if (!feature) {
        return;
    }
    switch (feature->id) {
        case octo::FEATURE_END:
            bed.features_done = true;
            ESP_LOGI(TAG, "Bed %d features: motors=%d presets=%d light=%d rgbw=%d synchro=%d", index_of(&bed),
                     bed.motor_count, bed.memory_slots, bed.has_light, bed.has_rgbw, bed.has_synchro);
            break;
        case octo::FEATURE_MOTORCOUNT:
            bed.motor_count = feature->value.empty() ? -1 : feature->value[0];
            break;
        case octo::FEATURE_MEMCOUNT:
            bed.memory_slots = feature->value.empty() ? 0 : feature->value[0];
            break;
        case octo::FEATURE_SYNCHRO: bed.has_synchro = true; break;
        case octo::FEATURE_LIGHT: bed.has_light = true; break;
        case octo::FEATURE_LIGHT_RGBWI: bed.has_rgbw = true; break;
        default: break;
    }
}

void on_notify(Bed& bed, const uint8_t* data, size_t len) {
    if (octo::is_pin_accepted(data, len)) {
        // Also the bed's answer to every keep-alive
        bed.last_ack_us = now_us();
        if (bed.state == State::Authenticating) {
            bed.state = State::Ready;
            bed.failures = 0;
            bed.connected_since_us = now_us();
            bed.connects++;
            log_event("bed %d '%s' ready (rssi %d)", index_of(&bed), bed.name, bed.rssi);
            if (!bed.verified) {
                bed.verified = true;
                store_save(index_of(&bed));
            }
            if (!bed.features_done) {
                write(bed, octo::get_features());
            }
        }
        return;
    }
    if (octo::is_pin_rejected(data, len)) {
        bed.pin_rejects++;
        log_event("bed %d '%s' rejected the PIN; check it with 'bed set'", index_of(&bed), bed.name);
        bed.next_attempt_us = now_us() + PIN_REJECTED_BACKOFF_US;
        terminate(bed, "PIN rejected");
        return;
    }
    if (octo::is_pin_required(data, len)) {
        send_pin(bed);
        return;
    }
    auto packet = octo::parse_packet(data, len);
    if (packet && packet->command == octo::RSP_FEATURE) {
        on_feature(bed, packet->data);
        return;
    }
    ESP_LOGD(TAG, "Bed %d: <- %s", index_of(&bed), hex(data, len).c_str());
}

// ------------------------------------------------------------------ GAP events

int gap_event(ble_gap_event* event, void* arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            on_advertisement(event->disc);
            return 0;

        case BLE_GAP_EVENT_CONNECT: {
            Bed& bed = beds[reinterpret_cast<intptr_t>(arg)];
            if (event->connect.status != 0) {
                log_event("bed %d connect failed (status %d)", index_of(&bed), event->connect.status);
                bed.state = State::Idle;
                connect_failed(bed);
                release_connect_gate();
                ensure_scanning();
                return 0;
            }
            bed.conn = event->connect.conn_handle;
            bed.state = State::Discovering;
            bed.svc_start = bed.svc_end = bed.chr_val = bed.chr_end = bed.cccd = 0;
            log_event("bed %d link up, discovering", index_of(&bed));
            int rc = ble_gattc_exchange_mtu(bed.conn, on_mtu, nullptr);
            if (rc != 0) {
                // Not needed for our packets: go straight to service discovery
                log_event("bed %d MTU exchange not started (rc %d), continuing", index_of(&bed), rc);
                ble_gatt_error no_error = {};
                on_mtu(bed.conn, &no_error, BLE_ATT_MTU_DFLT, nullptr);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            Bed* bed = bed_by_conn(event->disconnect.conn.conn_handle);
            if (bed == nullptr) {
                return 0;
            }
            const bool was_ready = bed->state == State::Ready;
            const int64_t up_s = was_ready ? (now_us() - bed->connected_since_us) / 1000000 : 0;
            log_event("bed %d '%s' disconnected (reason 0x%x, up %" PRId64 " s)", index_of(bed), bed->name,
                      event->disconnect.reason, up_s);
            if (bed->state != State::Ready && bed->state != State::Authenticating) {
                release_connect_gate();  // failed during discovery
            }
            if (was_ready) {
                bed->disconnects++;
            } else if (bed->next_attempt_us <= now_us()) {
                connect_failed(*bed);
            }
            bed->last_disconnect_reason = event->disconnect.reason;
            bed->state = State::Idle;
            bed->conn = NO_CONN;
            end_runs(*bed);
            bed->seen = false;  // wait for a fresh advertisement
            ensure_scanning();
            return 0;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            Bed* bed = bed_by_conn(event->notify_rx.conn_handle);
            if (bed == nullptr) {
                return 0;
            }
            uint8_t buf[64];
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            const uint16_t copy = len < sizeof(buf) ? len : sizeof(buf);
            if (os_mbuf_copydata(event->notify_rx.om, 0, copy, buf) == 0) {
                on_notify(*bed, buf, copy);
            }
            return 0;
        }

        default:
            return 0;
    }
}

// ------------------------------------------------------------------ periodic work

void manager_tick(ble_npl_event*) {
    const int64_t now = now_us();
    for (auto& bed : beds) {
        if (bed.state == State::Authenticating && now - bed.auth_started_us > PIN_VERIFY_TIMEOUT_US) {
            terminate(bed, "no answer to the PIN");
        } else if (bed.state == State::Ready && now - bed.last_ack_us > LIVENESS_TIMEOUT_US) {
            terminate(bed, "keep-alive not acknowledged");
        } else if (bed.state == State::Connecting && now - bed.connect_started_us > CONNECT_STUCK_US) {
            log_event("bed %d connect never completed, cancelling", index_of(&bed));
            ble_gap_conn_cancel();
            bed.state = State::Idle;
            connect_failed(bed);
            release_connect_gate();
        }
    }
    if (synced && !connect_in_progress && now >= next_connect_allowed_us) {
        for (int i = 0; i < MAX_BEDS; ++i) {
            Bed& bed = beds[i];
            if (!bed.configured || bed.state != State::Idle || !bed.seen) {
                continue;
            }
            if (now - bed.last_adv_us > SEEN_RECENTLY_US || now < bed.next_attempt_us) {
                continue;
            }
            // One connection set-up at a time, without scanning in parallel
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(own_addr_type, &bed.addr, CONNECT_TIMEOUT_MS, nullptr, gap_event,
                                     reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            if (rc != 0) {
                ESP_LOGW(TAG, "Bed %d: starting connect failed rc=%d", i, rc);
                connect_failed(bed);
                continue;
            }
            log_event("bed %d connecting (rssi %d)", i, bed.rssi);
            bed.state = State::Connecting;
            bed.connect_started_us = now;
            connect_in_progress = true;
            break;
        }
    }
    ensure_scanning();
    ble_npl_callout_reset(&manager_co, ble_npl_time_ms_to_ticks32(MANAGER_INTERVAL_MS));
}

// Book the time driven since the last command on the runs it drove.
void account(Bed& bed, int64_t now) {
    const int64_t elapsed = now - bed.last_motion_us;
    bed.last_motion_us = now;
    for (Run* run : {&bed.head_run, &bed.feet_run}) {
        if (run->driving) {
            run->done_us += elapsed;
            run->remaining_us -= elapsed;
            run->pending = run->remaining_us > 0;
        }
        run->driving = false;
    }
}

void motion_tick(ble_npl_event*) {
    bool active = false;
    const int64_t now = now_us();
    for (auto& bed : beds) {
        Run& head = bed.head_run;
        Run& feet = bed.feet_run;
        account(bed, now);
        if (bed.state != State::Ready) {
            end_runs(bed);
            continue;
        }
        bool drive_head = head.pending;
        bool drive_feet = feet.pending;
        if (drive_head && drive_feet && head.up != feet.up) {
            (head.id < feet.id ? drive_feet : drive_head) = false;
        }
        if (!drive_head && !drive_feet) {
            if (bed.motion_sent) {
                bed.motion_sent = false;
                write(bed, octo::motor_stop());
            }
            continue;
        }
        const uint8_t mask = (drive_head ? octo::MOTOR_HEAD : 0) | (drive_feet ? octo::MOTOR_FEET : 0);
        const bool up = drive_head ? head.up : feet.up;
        write(bed, up ? octo::motor_up(mask) : octo::motor_down(mask));
        head.driving = drive_head;
        feet.driving = drive_feet;
        bed.motion_sent = true;
        active = true;
    }
    if (active) {
        ble_npl_callout_reset(&motion_co, ble_npl_time_ms_to_ticks32(MOTION_INTERVAL_MS));
    }
}

void keepalive_tick(ble_npl_event*) {
    for (auto& bed : beds) {
        if (bed.state == State::Ready) {
            send_pin(bed);
        }
    }
    ble_npl_callout_reset(&keepalive_co, ble_npl_time_ms_to_ticks32(KEEPALIVE_INTERVAL_MS));
}

void stats_tick(ble_npl_event*) {
    std::fputs(status_text().c_str(), stdout);
    ble_npl_callout_reset(&stats_co, ble_npl_time_ms_to_ticks32(STATS_INTERVAL_MS));
}

// ------------------------------------------------------------------ host task

void on_sync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "No usable BLE address rc=%d", rc);
        return;
    }
    synced = true;
    ESP_LOGI(TAG, "BLE ready");
    ensure_scanning();
}

void on_reset(int reason) {
    synced = false;
    ESP_LOGE(TAG, "BLE host reset, reason %d", reason);
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

struct HostRequest {
    ble_npl_event event;
    const std::function<void()>* fn;
    SemaphoreHandle_t done;
};

void run_request(ble_npl_event* event) {
    auto* request = static_cast<HostRequest*>(ble_npl_event_get_arg(event));
    (*request->fn)();
    xSemaphoreGive(request->done);
}

}  // namespace

// ------------------------------------------------------------------ public API

void start() {
    store_load();
#if SOC_BT_CLASSIC_SUPPORTED
    // BLE only: give the classic Bluetooth controller memory back to the heap
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(185);

    ble_npl_eventq* queue = nimble_port_get_dflt_eventq();
    ble_npl_callout_init(&manager_co, queue, manager_tick, nullptr);
    ble_npl_callout_init(&motion_co, queue, motion_tick, nullptr);
    ble_npl_callout_init(&keepalive_co, queue, keepalive_tick, nullptr);
    ble_npl_callout_init(&stats_co, queue, stats_tick, nullptr);
    ble_npl_callout_reset(&manager_co, ble_npl_time_ms_to_ticks32(MANAGER_INTERVAL_MS));
    ble_npl_callout_reset(&keepalive_co, ble_npl_time_ms_to_ticks32(KEEPALIVE_INTERVAL_MS));
    ble_npl_callout_reset(&stats_co, ble_npl_time_ms_to_ticks32(STATS_INTERVAL_MS));

    nimble_port_freertos_init(host_task);
}

void run_in_host(const std::function<void()>& fn) {
    // Zero-initialise: the ESP-IDF NPL port only allocates the event's pool
    // block when ev->event is null, so stack garbage there is a wild write.
    HostRequest request{};
    request.fn = &fn;
    request.done = xSemaphoreCreateBinary();
    ble_npl_event_init(&request.event, run_request, &request);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &request.event);
    // Watchdog: a host task that stops answering would block Matter and the
    // HTTP API with it, so restart instead.
    if (xSemaphoreTake(request.done, pdMS_TO_TICKS(HOST_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "BLE host task not answering for %u ms, restarting", static_cast<unsigned>(HOST_TIMEOUT_MS));
        esp_restart();
    }
    ble_npl_event_deinit(&request.event);  // return the pool block
    vSemaphoreDelete(request.done);
}

RunInfo run_info(const Run& run, int64_t last_motion_us) {
    RunInfo info;
    info.id = run.id;
    info.moving = run.pending;
    info.up = run.up;
    // Include the time since the last command while the motor is driven
    const int64_t done = run.done_us + (run.driving ? now_us() - last_motion_us : 0);
    info.done_s = done / 1000000.0f;
    info.finished = !run.pending && run.remaining_us <= 0;
    return info;
}

Info info(int index) {
    Info result;
    if (index >= 0 && index < MAX_BEDS) {
        const Bed& bed = beds[index];
        result.configured = bed.configured;
        result.ready = bed.state == State::Ready;
        result.moving = bed.head_run.pending || bed.feet_run.pending;
        result.head = run_info(bed.head_run, bed.last_motion_us);
        result.feet = run_info(bed.feet_run, bed.last_motion_us);
        result.name = bed.name;
        result.head_travel_s = bed.head_travel_ds / 10.0f;
        result.feet_travel_s = bed.feet_travel_ds / 10.0f;
    }
    return result;
}

bool configure(int index, const uint8_t mac[6], const std::string& pin, const std::string& name) {
    if (index < 0 || index >= MAX_BEDS || !octo::encode_pin(pin)) {
        return false;
    }
    Bed& bed = beds[index];
    terminate(bed, "reconfigured");
    const uint16_t head_travel = bed.head_travel_ds;  // calibration survives a new PIN or name
    const uint16_t feet_travel = bed.feet_travel_ds;
    bed = Bed{};
    bed.configured = true;
    bed.head_travel_ds = head_travel;
    bed.feet_travel_ds = feet_travel;
    std::memcpy(bed.mac, mac, sizeof(bed.mac));
    std::snprintf(bed.pin, sizeof(bed.pin), "%s", pin.c_str());
    std::snprintf(bed.name, sizeof(bed.name), "%s", name.c_str());
    store_save(index);
    ESP_LOGI(TAG, "Bed %d '%s' %s stored (PIN not shown)", index, bed.name, mac_string(bed.mac).c_str());
    return true;
}

bool rename(int index, const std::string& name) {
    if (index < 0 || index >= MAX_BEDS || !beds[index].configured || name.empty()) {
        return false;
    }
    std::snprintf(beds[index].name, sizeof(beds[index].name), "%s", name.c_str());
    store_save(index);
    return true;
}

void clear(int index) {
    if (index < 0 || index >= MAX_BEDS) {
        return;
    }
    terminate(beds[index], "removed");
    beds[index] = Bed{};
    store_save(index);
}

uint32_t move(int index, Part part, bool up, float seconds) {
    if (index < 0 || index >= MAX_BEDS || beds[index].state != State::Ready || seconds <= 0) {
        return 0;
    }
    Bed& bed = beds[index];
    const uint32_t id = ++last_run_id;
    account(bed, now_us());  // so the time driven by a replaced run is not lost
    for (Run* run : {&bed.head_run, &bed.feet_run}) {
        if ((run == &bed.head_run ? Part::Head : Part::Feet) == part || part == Part::Both) {
            run->id = id;
            run->pending = true;
            run->up = up;
            run->driving = false;
            run->remaining_us = static_cast<int64_t>(seconds * 1000000.0f);
            run->done_us = 0;
        }
    }
    motion_tick(nullptr);
    return id;
}

bool stop(int index, Part part) {
    if (index < 0 || index >= MAX_BEDS) {
        return false;
    }
    Bed& bed = beds[index];
    if (bed.calibrating && (part == Part::Both || part == bed.calibrate_part)) {
        // A stop from anywhere ends a calibration without saving it
        bed.calibrating = false;
        log_event("bed %d calibration cancelled", index);
    }
    account(bed, now_us());
    if (part != Part::Both) {
        // The other motor keeps going: the next command drives only that one
        (part == Part::Head ? bed.head_run : bed.feet_run).pending = false;
        motion_tick(nullptr);
        return true;
    }
    end_runs(bed);
    return write(bed, octo::motor_stop());
}

void stop_all() {
    for (int i = 0; i < MAX_BEDS; ++i) {
        if (beds[i].head_run.pending || beds[i].feet_run.pending) {
            stop(i);
        }
    }
}

bool light(int index, bool on) {
    if (index < 0 || index >= MAX_BEDS || beds[index].state != State::Ready) {
        return false;
    }
    return write(beds[index], octo::light(on));
}

bool set_travel(int index, Part part, float seconds) {
    if (index < 0 || index >= MAX_BEDS || !beds[index].configured || part == Part::Both) {
        return false;
    }
    if (seconds < MIN_TRAVEL_S) seconds = MIN_TRAVEL_S;
    if (seconds > MAX_TRAVEL_S) seconds = MAX_TRAVEL_S;
    Bed& bed = beds[index];
    (part == Part::Head ? bed.head_travel_ds : bed.feet_travel_ds) = static_cast<uint16_t>(seconds * 10.0f + 0.5f);
    store_save(index);
    return true;
}

void on_calibrated(CalibratedCallback callback) {
    calibrated_callback = std::move(callback);
}

bool calibrate_start(int index, Part part) {
    if (part == Part::Both || !move(index, part, true, MAX_TRAVEL_S)) {
        return false;
    }
    Bed& bed = beds[index];
    bed.calibrating = true;
    bed.calibrate_part = part;
    bed.calibrate_started_us = now_us();
    log_event("bed %d calibrating %s", index, part == Part::Head ? "head" : "legs");
    return true;
}

float calibrate_stop(int index) {
    if (index < 0 || index >= MAX_BEDS || !beds[index].calibrating) {
        return -1.0f;
    }
    Bed& bed = beds[index];
    bed.calibrating = false;
    stop(index);
    const float seconds = (now_us() - bed.calibrate_started_us) / 1000000.0f;
    if (seconds < MIN_TRAVEL_S || seconds >= MAX_TRAVEL_S) {
        log_event("bed %d calibration discarded (%.1f s)", index, seconds);
        return -1.0f;
    }
    set_travel(index, bed.calibrate_part, seconds);
    log_event("bed %d %s travel %.1f s", index, bed.calibrate_part == Part::Head ? "head" : "legs", seconds);
    if (calibrated_callback) {
        calibrated_callback(index, bed.calibrate_part, seconds);
    }
    return seconds;
}

static std::string json_string(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            out += '\\';
            out += *c;
        } else if (static_cast<unsigned char>(*c) >= 0x20) {
            out += *c;
        }
    }
    return out + "\"";
}

std::string state_json() {
    const int64_t now = now_us();
    std::string out;
    appendf(out, "{\"uptime\":%" PRId64 ",\"heap\":%u,\"beds\":[", now / 1000000,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    for (int i = 0; i < MAX_BEDS; ++i) {
        const Bed& bed = beds[i];
        appendf(out,
                "%s{\"index\":%d,\"configured\":%s,\"name\":%s,\"mac\":\"%s\",\"state\":\"%s\",\"rssi\":%d,"
                "\"head_travel\":%.1f,\"legs_travel\":%.1f,\"calibrating\":%s,\"calibrate_elapsed\":%.1f,"
                "\"drops\":%" PRIu32 ",\"verified\":%s,\"pin_rejects\":%" PRIu32 "}",
                i ? "," : "", i, bed.configured ? "true" : "false", json_string(bed.name).c_str(),
                bed.configured ? mac_string(bed.mac).c_str() : "", state_name(bed.state), bed.rssi,
                bed.head_travel_ds / 10.0, bed.feet_travel_ds / 10.0,
                !bed.calibrating ? "null" : bed.calibrate_part == Part::Head ? "\"head\"" : "\"legs\"",
                bed.calibrating ? (now - bed.calibrate_started_us) / 1000000.0 : 0.0, bed.disconnects,
                bed.verified ? "true" : "false", bed.pin_rejects);
    }
    out += "],\"scan\":[";
    for (int i = 0; i < scan_cache_used; ++i) {
        const ScanEntry& entry = scan_cache[i];
        appendf(out, "%s{\"address\":\"%s\",\"name\":%s,\"rssi\":%d,\"seen\":%" PRId64 ",\"octo\":%s}",
                i ? "," : "", addr_string(entry.addr).c_str(), json_string(entry.name).c_str(), entry.rssi,
                (now - entry.last_seen_us) / 1000000, entry.octo_service ? "true" : "false");
    }
    return out + "]}";
}

std::string status_text() {
    const int64_t now = now_us();
    std::string out;
    appendf(out, "STAT up=%" PRId64 "s heap=%u min=%u largest=%u\n", now / 1000000,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    for (int i = 0; i < MAX_BEDS; ++i) {
        const Bed& bed = beds[i];
        if (!bed.configured) {
            appendf(out, "  bed %d: not configured\n", i);
            continue;
        }
        const int64_t link_s = bed.state == State::Ready ? (now - bed.connected_since_us) / 1000000 : 0;
        const int64_t adv_ago = bed.last_adv_us ? (now - bed.last_adv_us) / 1000000 : -1;
        appendf(out,
                "  bed %d '%s' %s: %s link=%" PRId64 "s rssi=%d adv_ago=%" PRId64 "s connects=%" PRIu32
                " drops=%" PRIu32 " pin_rejects=%" PRIu32 " last_reason=0x%x\n",
                i, bed.name, mac_string(bed.mac).c_str(), state_name(bed.state), link_s, bed.rssi, adv_ago,
                bed.connects, bed.disconnects, bed.pin_rejects, bed.last_disconnect_reason);
        if (bed.features_done) {
            appendf(out, "    features: motors=%d presets=%d light=%d rgbw=%d synchro=%d\n", bed.motor_count,
                    bed.memory_slots, bed.has_light, bed.has_rgbw, bed.has_synchro);
        }
    }
    return out;
}

std::string scan_text() {
    const int64_t now = now_us();
    std::string out;
    appendf(out, "%-17s  %-19s %5s  %s\n", "address", "name", "rssi", "seen");
    for (int i = 0; i < scan_cache_used; ++i) {
        const ScanEntry& entry = scan_cache[i];
        appendf(out, "%-17s  %-19s %5d  %" PRId64 "s ago%s\n", addr_string(entry.addr).c_str(), entry.name,
                entry.rssi, (now - entry.last_seen_us) / 1000000, entry.octo_service ? "  [Octo FFE0]" : "");
    }
    return out;
}

std::string events_text() {
    std::string out;
    for (int i = 0; i < EVENT_LOG_SIZE; ++i) {
        const char* line = event_log[(event_next + i) % EVENT_LOG_SIZE];
        if (line[0] != '\0') {
            out += line;
            out += '\n';
        }
    }
    return out;
}

}  // namespace bed_link
