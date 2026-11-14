#include "ble_device.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ble_scan.h"
#include "ble_setup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "mb_driver.h"
#include "nvs.h"

namespace ble_device {

namespace {

const char* TAG = "bledev";
const char* NVS_NAMESPACE = "bledev";

constexpr size_t MAX_BYTES = 64;   // one write; a longer MTU is asked for when needed
constexpr int MAX_TIMES = 50;
constexpr int MAX_GAP_MS = 2000;
constexpr int MAX_FINISH_GAP_MS = 5000;
constexpr int CONNECT_MS = 10000;
constexpr uint16_t NO_CONN = 0xFFFF;

struct Command {
    std::string label;
    std::vector<uint8_t> bytes;
    uint8_t times = 1;
    uint16_t gap_ms = 0;
    std::vector<uint8_t> finish;
    uint16_t finish_gap_ms = 0;
};

struct Settings {
    uint8_t mac[6] = {};  // as written: AA:BB:... is {0xAA, ...}
    bool have_mac = false;
    bool random_address = false;
    std::string service = "";
    std::string characteristic = "";
    bool no_response = false;
    bool pair = false;
    std::vector<uint8_t> hello;
    uint16_t idle_s = 30;
    Command commands[COMMANDS];
};

Settings settings;
std::mutex procedure_lock;
std::string procedure_json = "[]";
std::vector<ble_setup::Step> procedure;
int test_state = 0;  // idle, running, succeeded, failed
std::string test_error;
std::atomic<bool> link_ready{false}, collect_notifications{false}, notification_overflow{false};
std::atomic<unsigned> configuration_epoch{0};
unsigned link_epoch = 0;
constexpr size_t MAX_PROCEDURE_JSON = 3900;
std::string last_error;
int sent_count = 0;

// The link, owned by the worker task (and the host's callbacks)
volatile uint16_t conn = NO_CONN;
uint16_t value_handle = 0;
uint16_t service_start = 0, service_end = 0;
uint16_t mtu = 23;
EventGroupHandle_t events = nullptr;
constexpr EventBits_t CONNECTED = 1, FAILED = 2, DONE = 4, DISCONNECTED = 8, SECURED = 16;
volatile int op_status = 0;
uint16_t discovered_handle = 0, descriptor_end = 0, cccd_handle = 0;
struct Notification {
    uint16_t handle;
    uint16_t len;
    uint8_t bytes[MAX_BYTES];
};
QueueHandle_t notifications = nullptr;
std::vector<uint8_t> read_value;

QueueHandle_t queue = nullptr;
struct Job {
    int command;                // -1: raw bytes, -2: disconnect, -3: test
    uint8_t raw[MAX_BYTES];
    uint8_t raw_len;
};
esp_timer_handle_t idle_timer = nullptr;

// ------------------------------------------------------------------ text

std::string hex(const std::vector<uint8_t>& bytes) {
    std::string out;
    char b[3];
    for (uint8_t v : bytes) {
        std::snprintf(b, sizeof(b), "%02X", v);
        out += b;
    }
    return out;
}

bool from_hex(const std::string& text, std::vector<uint8_t>& out, size_t max) {
    std::string digits;
    for (char c : text) {
        if (c != ' ' && c != ':' && c != '-') digits += c;
    }
    if (digits.size() % 2 != 0 || digits.size() / 2 > max) return false;
    out.clear();
    for (size_t i = 0; i < digits.size(); i += 2) {
        char* end = nullptr;
        const std::string pair = digits.substr(i, 2);
        const long v = std::strtol(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

bool parse_mac(const std::string& text, uint8_t* mac) {
    std::vector<uint8_t> bytes;
    if (text.size() != 17 || !from_hex(text, bytes, 6) || bytes.size() != 6) return false;
    std::memcpy(mac, bytes.data(), 6);
    return true;
}

std::string mac_text(const uint8_t* mac) {
    char out[18];
    std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return out;
}

std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char e[8];
            std::snprintf(e, sizeof(e), "\\u%04x", c);
            out += e;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

bool valid_uuid(const std::string& text) {
    ble_uuid_any_t uuid;
    if (text.size() == 4) {
        return std::strtoul(text.c_str(), nullptr, 16) > 0 && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    }
    return ble_uuid_from_str(&uuid, text.c_str()) == 0;
}

bool to_uuid(const std::string& text, ble_uuid_any_t& uuid) {
    if (text.size() == 4) {
        uuid.u16.u.type = BLE_UUID_TYPE_16;
        uuid.u16.value = static_cast<uint16_t>(std::strtoul(text.c_str(), nullptr, 16));
        return true;
    }
    return ble_uuid_from_str(&uuid, text.c_str()) == 0;
}

// ------------------------------------------------------------------ storage

bool store_str(const char* key, const std::string& value) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = value.empty() ? nvs_erase_key(nvs, key) : nvs_set_str(nvs, key, value.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

std::string read_str(nvs_handle_t nvs, const char* key) {
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) return "";
    std::string value(len, '\0');
    nvs_get_str(nvs, key, value.data(), &len);
    value.resize(len - 1);
    return value;
}

// One command as "<hex>\t<times>\t<gap ms>\t<label>"
std::string command_text(const Command& c) {
    if (c.bytes.empty()) return "";
    std::string value = hex(c.bytes) + "\t" + std::to_string(c.times) + "\t" + std::to_string(c.gap_ms) + "\t" + c.label;
    if (!c.finish.empty()) value += "\t" + hex(c.finish) + "\t" + std::to_string(c.finish_gap_ms);
    return value;
}

void parse_command(const std::string& text, Command& c) {
    c = Command();
    const size_t a = text.find('\t'), b = a == std::string::npos ? a : text.find('\t', a + 1),
                 d = b == std::string::npos ? b : text.find('\t', b + 1);
    if (d == std::string::npos || !from_hex(text.substr(0, a), c.bytes, MAX_BYTES)) {
        c.bytes.clear();
        return;
    }
    c.times = static_cast<uint8_t>(std::atoi(text.substr(a + 1, b - a - 1).c_str()));
    c.gap_ms = static_cast<uint16_t>(std::atoi(text.substr(b + 1, d - b - 1).c_str()));
    const size_t e = text.find('\t', d + 1);
    c.label = text.substr(d + 1, e == std::string::npos ? e : e - d - 1);
    if (e != std::string::npos) {
        const size_t f = text.find('\t', e + 1);
        if (f != std::string::npos && from_hex(text.substr(e + 1, f - e - 1), c.finish, MAX_BYTES)) {
            c.finish_gap_ms = static_cast<uint16_t>(std::atoi(text.c_str() + f + 1));
        }
    }
    if (c.times < 1) c.times = 1;
}

// ------------------------------------------------------------------ the link

int on_gap(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn = event->connect.conn_handle;
                xEventGroupSetBits(events, CONNECTED);
            } else {
                op_status = event->connect.status;
                xEventGroupSetBits(events, FAILED);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            conn = NO_CONN;
            link_ready = false;
            value_handle = 0;
            mtu = 23;
            xEventGroupSetBits(events, DISCONNECTED | FAILED);
            ESP_LOGI(TAG, "Disconnected (%d)", event->disconnect.reason);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE: {
            if (event->enc_change.conn_handle != conn) break;
            ble_gap_conn_desc desc = {};
            const bool encrypted = event->enc_change.status == 0 &&
                ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted;
            xEventGroupSetBits(events, encrypted ? SECURED : FAILED);
            break;
        }
        case BLE_GAP_EVENT_NOTIFY_RX: {
            const auto& rx = event->notify_rx;
            if (rx.conn_handle != conn || !collect_notifications) break;
            Notification item{};
            item.handle = rx.attr_handle;
            item.len = OS_MBUF_PKTLEN(rx.om);
            if (item.len <= sizeof(item.bytes) && os_mbuf_copydata(rx.om, 0, item.len, item.bytes) == 0) {
                if (xQueueSend(notifications, &item, 0) != pdTRUE) notification_overflow = true;
            }
            break;
        }
        case BLE_GAP_EVENT_MTU:
            mtu = event->mtu.value;
            break;
        default:
            break;
    }
    return 0;
}

int on_service(uint16_t, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (error->status == 0 && service != nullptr) {
        service_start = service->start_handle;
        service_end = service->end_handle;
    } else {
        op_status = error->status == BLE_HS_EDONE ? 0 : error->status;
        xEventGroupSetBits(events, DONE);
    }
    return 0;
}

int on_characteristic(uint16_t, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
    if (error->status == 0 && chr != nullptr) {
        const auto* uuid = static_cast<const ble_uuid_t*>(arg);
        if (discovered_handle == 0 && ble_uuid_cmp(&chr->uuid.u, uuid) == 0) {
            discovered_handle = chr->val_handle;
        } else if (discovered_handle != 0 && descriptor_end == service_end) {
            descriptor_end = chr->def_handle - 1;
        }
    } else {
        op_status = error->status == BLE_HS_EDONE ? 0 : error->status;
        xEventGroupSetBits(events, DONE);
    }
    return 0;
}

int on_mtu(uint16_t, const ble_gatt_error*, uint16_t value, void*) {
    mtu = value;
    xEventGroupSetBits(events, DONE);
    return 0;
}

bool wait(EventBits_t bits, int ms) {
    const EventBits_t got = xEventGroupWaitBits(events, bits | FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
    return (got & bits) != 0 && (got & FAILED) == 0;
}

int on_write(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    op_status = error->status;
    xEventGroupSetBits(events, DONE);
    return 0;
}

bool write_at(uint16_t handle, const uint8_t* data, size_t len, bool no_response, int ms) {
    if (conn == NO_CONN || handle == 0) {
        last_error = "Bluetooth disconnected";
        return false;
    }
    if (len > static_cast<size_t>(mtu - 3)) {
        last_error = "write exceeds negotiated Bluetooth MTU";
        return false;
    }
    xEventGroupClearBits(events, DONE);
    op_status = 0;
    const int rc = no_response ? ble_gattc_write_no_rsp_flat(conn, handle, data, len)
                                        : ble_gattc_write_flat(conn, handle, data, len, on_write, nullptr);
    if (rc != 0) {
        last_error = "write failed (" + std::to_string(rc) + ")";
        return false;
    }
    if (!no_response && !wait(DONE, ms)) {
        last_error = "no Bluetooth write response";
        if (conn != NO_CONN) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return false;
    }
    if (op_status != 0) {
        last_error = "write rejected (" + std::to_string(op_status) + ")";
        return false;
    }
    return true;
}

bool write(const uint8_t* data, size_t len) {
    return write_at(value_handle, data, len, settings.no_response, 3000);
}

bool secure(int ms) {
    ble_gap_conn_desc desc = {};
    if (ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted) return true;
    xEventGroupClearBits(events, SECURED);
    const int rc = ble_gap_security_initiate(conn);
    return (rc == 0 || rc == BLE_HS_EALREADY) && wait(SECURED, ms);
}

int remaining_ms(int64_t deadline) {
    return std::max<int64_t>(0, (deadline - esp_timer_get_time() + 999) / 1000);
}

bool discover(const std::string& service, const std::string& characteristic, int64_t deadline) {
    ble_uuid_any_t service_uuid, chr_uuid;
    if (!to_uuid(service, service_uuid) || !to_uuid(characteristic, chr_uuid)) return false;
    service_start = service_end = 0;
    xEventGroupClearBits(events, DONE);
    int rc = ble_gattc_disc_svc_by_uuid(conn, &service_uuid.u, on_service, nullptr);
    if (rc != 0 || !wait(DONE, remaining_ms(deadline)) || op_status != 0 || service_start == 0) {
        last_error = "service " + service + " was not found";
        return false;
    }
    discovered_handle = 0;
    descriptor_end = service_end;
    xEventGroupClearBits(events, DONE);
    // The next characteristic definition bounds the descriptors of the selected characteristic.
    static ble_uuid_any_t search_uuid;
    search_uuid = chr_uuid;
    rc = ble_gattc_disc_all_chrs(conn, service_start, service_end, on_characteristic, &search_uuid.u);
    if (rc != 0 || !wait(DONE, remaining_ms(deadline)) || op_status != 0 || discovered_handle == 0) {
        last_error = "characteristic " + characteristic + " was not found";
        return false;
    }
    return true;
}

int on_descriptor(uint16_t, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* descriptor, void*) {
    if (error->status == 0 && descriptor) {
        const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&descriptor->uuid.u, &cccd.u) == 0) cccd_handle = descriptor->handle;
    } else {
        op_status = error->status == BLE_HS_EDONE ? 0 : error->status;
        xEventGroupSetBits(events, DONE);
    }
    return 0;
}

int on_read(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
    op_status = error->status;
    if (op_status == 0 && attr && attr->om) {
        const auto len = OS_MBUF_PKTLEN(attr->om);
        if (len > MAX_BYTES) op_status = BLE_HS_EMSGSIZE;
        else {
            read_value.resize(len);
            op_status = os_mbuf_copydata(attr->om, 0, len, read_value.data());
        }
    }
    xEventGroupSetBits(events, DONE);
    return 0;
}

bool subscribe(uint16_t handle, uint16_t end, bool indicate, int64_t deadline) {
    cccd_handle = 0;
    xEventGroupClearBits(events, DONE);
    if (handle >= end || ble_gattc_disc_all_dscs(conn, handle, end, on_descriptor, nullptr) != 0 ||
        !wait(DONE, remaining_ms(deadline)) || op_status != 0 || cccd_handle == 0) {
        last_error = "notification descriptor (CCCD) was not found";
        return false;
    }
    const uint8_t value[] = {static_cast<uint8_t>(indicate ? 2 : 1), 0};
    return write_at(cccd_handle, value, sizeof(value), false, remaining_ms(deadline));
}

bool wait_notification(uint16_t handle, const ble_setup::Step& step, int64_t deadline) {
    Notification item;
    while (conn != NO_CONN && remaining_ms(deadline) > 0) {
        if (notification_overflow) {
            last_error = "notification queue overflow";
            return false;
        }
        if (xQueueReceive(notifications, &item, pdMS_TO_TICKS(std::min(100, remaining_ms(deadline))) + 1) != pdTRUE) continue;
        if (item.handle != handle) continue;
        const auto matches = [&](const auto& expected) {
            return !expected.empty() && item.len == expected.size() && std::memcmp(item.bytes, expected.data(), item.len) == 0;
        };
        if (matches(step.expect)) return true;
        if (matches(step.reject)) {
            last_error = "device rejected the request";
            return false;
        }
    }
    last_error = conn == NO_CONN ? "Bluetooth disconnected" : "expected notification did not arrive";
    return false;
}

std::string local_secret(const std::string& name) {
    nvs_handle_t nvs;
    if (nvs_open("ble_secrets", NVS_READONLY, &nvs) != ESP_OK) return {};
    const auto value = read_str(nvs, name.c_str());
    nvs_close(nvs);
    return value;
}

bool exchange_mtu(int ms) {
    if (mtu > 23) return true;
    xEventGroupClearBits(events, DONE);
    return ble_gattc_exchange_mtu(conn, on_mtu, nullptr) == 0 && wait(DONE, ms);
}

struct GattTarget {
    std::string service, characteristic;
    uint16_t handle, end;
};

bool run_step(const ble_setup::Step& step, std::vector<GattTarget>& targets, uint16_t& main_end) {
    using ble_setup::Type;
    const int64_t deadline = esp_timer_get_time() + step.ms * 1000;
    if (conn == NO_CONN) return false;
    switch (step.type) {
        case Type::pair: return secure(step.ms);
        case Type::delay:
            vTaskDelay(pdMS_TO_TICKS(step.ms) + 1);
            return conn != NO_CONN;
        case Type::discover:
            if (!discover(settings.service, settings.characteristic, deadline)) return false;
            value_handle = discovered_handle;
            main_end = descriptor_end;
            return true;
        default: break;
    }

    uint16_t handle = value_handle, end = main_end;
    if (!step.service.empty()) {
        auto target = std::find_if(targets.begin(), targets.end(), [&](const GattTarget& candidate) {
            return candidate.service == step.service && candidate.characteristic == step.characteristic;
        });
        if (target == targets.end()) {
            if (!discover(step.service, step.characteristic, deadline)) return false;
            targets.push_back({step.service, step.characteristic, discovered_handle, descriptor_end});
            target = targets.end() - 1;
        }
        handle = target->handle;
        end = target->end;
    }
    switch (step.type) {
        case Type::subscribe: return subscribe(handle, end, step.indicate, deadline);
        case Type::wait: return wait_notification(handle, step, deadline);
        case Type::write: {
            std::vector<uint8_t> packet;
            if (!ble_setup::encode(step, step.secret.empty() ? "" : local_secret(step.secret), packet)) {
                last_error = step.secret.empty() ? "invalid packet" : "missing or invalid local secret: " + step.secret;
                return false;
            }
            if (packet.size() > static_cast<size_t>(mtu - 3)) exchange_mtu(remaining_ms(deadline));
            if (remaining_ms(deadline) == 0) return false;
            return write_at(handle, packet.data(), packet.size(), settings.no_response, remaining_ms(deadline));
        }
        case Type::read:
            read_value.clear();
            xEventGroupClearBits(events, DONE);
            if (ble_gattc_read(conn, handle, on_read, nullptr) != 0 || !wait(DONE, remaining_ms(deadline)) || op_status != 0) {
                last_error = "could not read characteristic";
                return false;
            }
            if (step.expect.empty() || read_value == step.expect) return true;
            last_error = "read value did not match";
            return false;
        default: return false;
    }
}

bool setup_link() {
    std::vector<ble_setup::Step> steps;
    {
        std::lock_guard<std::mutex> lock(procedure_lock);
        steps = procedure;
    }
    if (!steps.empty()) {
        std::vector<GattTarget> targets;
        uint16_t main_end = 0;
        for (size_t i = 0; i < steps.size(); ++i) {
            last_error.clear();
            if (!run_step(steps[i], targets, main_end)) {
                last_error = "step " + std::to_string(i + 1) + " (" + steps[i].name + "): " +
                    (last_error.empty() ? "failed or timed out" : last_error);
                return false;
            }
        }
    } else {
        if (settings.pair && !secure(CONNECT_MS)) {
            last_error = "could not pair with the Bluetooth device";
            return false;
        }
        if (!discover(settings.service, settings.characteristic, esp_timer_get_time() + 10000000)) return false;
        value_handle = discovered_handle;
    }

    size_t longest = steps.empty() ? settings.hello.size() : 0;
    for (const Command& c : settings.commands) longest = std::max({longest, c.bytes.size(), c.finish.size()});
    if (longest > 20) exchange_mtu(3000);
    if (steps.empty() && !settings.hello.empty()) {
        if (!write(settings.hello.data(), settings.hello.size())) return false;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return conn != NO_CONN;
}

bool connect() {
    const unsigned epoch = configuration_epoch;
    if (conn != NO_CONN && link_ready && link_epoch == epoch) return true;
    if (conn != NO_CONN) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait(DISCONNECTED, 2000);
        if (conn != NO_CONN) {
            last_error = "previous Bluetooth link did not close";
            return false;
        }
    }
    if (!settings.have_mac || settings.service.empty() || settings.characteristic.empty()) {
        last_error = "not set up: device, service and characteristic first";
        return false;
    }
    if (!ble_scan::ready()) {
        last_error = "Bluetooth is not running yet";
        return false;
    }
    ble_addr_t addr = {};
    addr.type = settings.random_address ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; ++i) addr.val[i] = settings.mac[5 - i];  // NimBLE keeps it reversed

    ble_scan::pause();
    bool ok = false;
    xEventGroupClearBits(events, CONNECTED | FAILED | DONE | DISCONNECTED | SECURED);
    op_status = 0;
    int rc = ble_gap_connect(ble_scan::own_address_type(), &addr, CONNECT_MS, nullptr, on_gap, nullptr);
    if (rc != 0) {
        last_error = "connecting failed (" + std::to_string(rc) + ")";
    } else if (!wait(CONNECTED, CONNECT_MS + 2000)) {
        last_error = "the device did not answer (in range, and not connected to something else?)";
        ble_gap_conn_cancel();
    } else {
        xQueueReset(notifications);
        notification_overflow = false;
        collect_notifications = true;
        ok = setup_link();
        collect_notifications = false;
        if (configuration_epoch != epoch) {
            last_error = "Bluetooth configuration changed during setup";
            ok = false;
        }
        link_epoch = epoch;
        link_ready = ok;
        if (ok) ESP_LOGI(TAG, "Connected to %s", mac_text(settings.mac).c_str());
    }
    if (!ok && conn != NO_CONN) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait(DISCONNECTED, 2000);
    }
    ble_scan::resume();
    return ok;
}

void disconnect() {
    if (conn != NO_CONN) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait(DISCONNECTED, 2000);
    }
}

void on_idle(void*) {
    Job job = {};
    job.command = -2;
    xQueueSend(queue, &job, 0);
}

void keep_alive() {
    if (idle_timer == nullptr) return;
    esp_timer_stop(idle_timer);
    if (settings.idle_s < 3600) esp_timer_start_once(idle_timer, static_cast<int64_t>(settings.idle_s) * 1000000LL);
}

void worker(void*) {
    Job job;
    for (;;) {
        if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.command == -2) {
            disconnect();
            continue;
        }
        last_error.clear();
        if (job.command == -3) {
            esp_timer_stop(idle_timer);
            disconnect();
            const bool ok = connect();
            disconnect();
            std::lock_guard<std::mutex> lock(procedure_lock);
            test_state = ok ? 2 : 3;
            test_error = ok ? "" : last_error;
            continue;
        }
        if (!connect()) {
            ESP_LOGW(TAG, "%s", last_error.c_str());
            continue;
        }
        if (job.command == -1) {
            if (write(job.raw, job.raw_len)) sent_count++;
        } else {
            const Command c = settings.commands[job.command];
            if (!c.bytes.empty()) {
                bool ok = true;
                for (int i = 0; i < c.times && ok; ++i) {
                    ok = write(c.bytes.data(), c.bytes.size());
                    if (ok && i + 1 < c.times && c.gap_ms > 0) vTaskDelay(pdMS_TO_TICKS(c.gap_ms));
                }
                if (!c.finish.empty() && conn != NO_CONN) {
                    if (ok && c.finish_gap_ms > 0) vTaskDelay(pdMS_TO_TICKS(c.finish_gap_ms));
                    ok = write(c.finish.data(), c.finish.size()) && ok;
                }
                if (ok) {
                    sent_count++;
                    ESP_LOGI(TAG, "Sent '%s'", c.label.c_str());
                }
            }
        }
        if (!last_error.empty()) ESP_LOGW(TAG, "%s", last_error.c_str());
        if (settings.idle_s == 0) disconnect();
        else keep_alive();
    }
}

void start_worker() {
    if (queue != nullptr) return;
    events = xEventGroupCreate();
    queue = xQueueCreate(8, sizeof(Job));
    notifications = xQueueCreate(12, sizeof(Notification));
    esp_timer_create_args_t args = {};
    args.callback = on_idle;
    args.name = "bledev_idle";
    esp_timer_create(&args, &idle_timer);
    xTaskCreate(worker, "bledev", 4096, nullptr, 5, nullptr);
}

std::string json() {
    std::lock_guard<std::mutex> lock(procedure_lock);
    std::string out = "{\"bledev\":{";
    out += "\"mac\":" + json_text(settings.have_mac ? mac_text(settings.mac) : "");
    out += ",\"random\":" + std::string(settings.random_address ? "true" : "false");
    out += ",\"service\":" + json_text(settings.service);
    out += ",\"characteristic\":" + json_text(settings.characteristic);
    out += ",\"no_response\":" + std::string(settings.no_response ? "true" : "false");
    out += ",\"pair\":" + std::string(settings.pair ? "true" : "false");
    out += ",\"hello\":" + json_text(procedure.empty() ? hex(settings.hello) : "");
    out += ",\"idle\":" + std::to_string(settings.idle_s);
    out += ",\"setup\":" + procedure_json;
    constexpr const char* states[] = {"idle", "running", "passed", "failed"};
    out += ",\"test\":" + json_text(states[test_state]) + ",\"test_error\":" + json_text(test_error);
    out += ",\"connected\":" + std::string(conn != NO_CONN && link_ready ? "true" : "false");
    out += ",\"secrets\":[";
    std::vector<std::string> listed;
    for (const auto& step : procedure) {
        if (step.secret.empty() || std::find(listed.begin(), listed.end(), step.secret) != listed.end()) continue;
        if (!listed.empty()) out += ",";
        out += "{\"name\":" + json_text(step.secret) + ",\"encoding\":" + json_text(step.digits ? "digits" : "hex") +
            ",\"set\":" + (local_secret(step.secret).empty() ? "false" : "true") + "}";
        listed.push_back(step.secret);
    }
    out += "]";
    out += ",\"sent\":" + std::to_string(sent_count);
    out += ",\"error\":" + json_text(last_error);
    out += ",\"commands\":[";
    for (int i = 0; i < COMMANDS; ++i) {
        const Command& c = settings.commands[i];
        out += std::string(i ? "," : "") + "{\"label\":" + json_text(c.label) + ",\"bytes\":" + json_text(hex(c.bytes)) +
               ",\"times\":" + std::to_string(c.times) + ",\"gap\":" + std::to_string(c.gap_ms);
        if (!c.finish.empty()) {
            out += ",\"finish\":" + json_text(hex(c.finish)) + ",\"finish_gap\":" + std::to_string(c.finish_gap_ms);
        }
        out += "}";
    }
    return out + "]}}\n";
}

// ------------------------------------------------------------------ the connection type

// "ble_command": a switch in Matter that sends command <number> and turns
// itself off again, as an IR code to send does
esp_timer_handle_t off_timers[8] = {};

void on_off_timer(void* arg) { io::report_output(static_cast<int>(reinterpret_cast<intptr_t>(arg)), false); }

bool command_setup(int slot, const io::Slot&) {
    if (slot < 0 || slot >= 8 || off_timers[slot] != nullptr) return true;
    esp_timer_create_args_t args = {};
    args.callback = on_off_timer;
    args.arg = reinterpret_cast<void*>(static_cast<intptr_t>(slot));
    args.name = "bledev_off";
    esp_timer_create(&args, &off_timers[slot]);
    return true;
}

void command_output(int slot, const io::Slot& s, bool on, uint8_t) {
    if (!on) return;
    const int n = static_cast<int>(io::number_of(s)) - 1;
    if (!send(n)) ESP_LOGW(TAG, "Connection %d: command %d is not set up", slot, n + 1);
    if (slot >= 0 && slot < 8 && off_timers[slot] != nullptr) {
        esp_timer_stop(off_timers[slot]);
        esp_timer_start_once(off_timers[slot], 500 * 1000);
    }
}

}  // namespace

const char* const TYPE = "ble_command";

void register_type() {
    io::Driver driver = {};
    driver.type = TYPE;
    driver.title = "Bluetooth command";
    driver.matter = io::Matter::on_off;
    driver.pins = 0;
    driver.number_label = "Command (1-8)";
    driver.setup = command_setup;
    driver.poll = nullptr;
    driver.output = command_output;
    io::register_driver(driver);
}

void load() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    const std::string mac = read_str(nvs, "mac");
    settings.have_mac = parse_mac(mac, settings.mac);
    settings.random_address = read_str(nvs, "atype") == "random";
    settings.service = read_str(nvs, "svc");
    settings.characteristic = read_str(nvs, "chr");
    settings.no_response = read_str(nvs, "wmode") == "no-response";
    settings.pair = read_str(nvs, "pair") == "on";
    const auto stored_setup = read_str(nvs, "setup");
    if (!stored_setup.empty()) {
        procedure_json = stored_setup;
        if (!ble_setup::parse(procedure_json, procedure)) {
            procedure_json = "[]";
            last_error = "stored Bluetooth setup could not be read";
            // The invalid procedure must be replaced before this target is used again.
            settings.have_mac = false;
        }
    }
    from_hex(read_str(nvs, "hello"), settings.hello, MAX_BYTES);
    const std::string idle = read_str(nvs, "idle");
    if (!idle.empty()) settings.idle_s = static_cast<uint16_t>(std::atoi(idle.c_str()));
    char key[4];
    for (int i = 0; i < COMMANDS; ++i) {
        std::snprintf(key, sizeof(key), "c%d", i);
        parse_command(read_str(nvs, key), settings.commands[i]);
    }
    nvs_close(nvs);
}

bool configured() { return settings.have_mac && !settings.service.empty() && !settings.characteristic.empty(); }

bool send(int command) {
    if (command < 0 || command >= COMMANDS || settings.commands[command].bytes.empty()) return false;
    start_worker();
    Job job = {};
    job.command = command;
    return xQueueSend(queue, &job, 0) == pdTRUE;
}

std::string label(int command) {
    return command >= 0 && command < COMMANDS ? settings.commands[command].label : "";
}

std::string command(const std::vector<std::string>& args) {
    const std::string sub = args.size() >= 2 ? args[1] : "json";
    if (sub == "json") return json();
    if (sub == "setup" && args.size() == 3) {
        const auto& data = args[2];
        std::vector<ble_setup::Step> next;
        if (data.size() > MAX_PROCEDURE_JSON || !ble_setup::parse(data, next)) return "error: invalid BLE setup\n";
        std::lock_guard<std::mutex> lock(procedure_lock);
        if (!store_str("setup", next.empty() ? "" : data)) return "error: could not store BLE setup\n";
        procedure_json = data;
        procedure = std::move(next);
        ++configuration_epoch;
        if (queue != nullptr) on_idle(nullptr);
        return "ok\n";
    }
    if (sub == "secret" && args.size() == 4) {
        std::lock_guard<std::mutex> lock(procedure_lock);
        const auto step = std::find_if(procedure.begin(), procedure.end(), [&](const auto& st) { return st.secret == args[2]; });
        if (step == procedure.end()) return "error: unknown local BLE secret\n";
        std::vector<uint8_t> packet;
        const bool clear = args[3] == "-";
        if (!clear && (args[3].size() > 64 || !ble_setup::encode(*step, args[3], packet))) {
            return "error: invalid local BLE secret\n";
        }
        nvs_handle_t nvs;
        if (nvs_open("ble_secrets", NVS_READWRITE, &nvs) != ESP_OK) return "error: could not open BLE storage\n";
        const auto status = clear ? nvs_erase_key(nvs, args[2].c_str()) : nvs_set_str(nvs, args[2].c_str(), args[3].c_str());
        const bool ok = (status == ESP_OK || (clear && status == ESP_ERR_NVS_NOT_FOUND)) && nvs_commit(nvs) == ESP_OK;
        nvs_close(nvs);
        if (ok) {
            ++configuration_epoch;
            if (queue != nullptr) on_idle(nullptr);
        }
        return ok ? "ok\n" : "error: could not store local BLE secret\n";
    }
    if (sub == "test" && args.size() == 2) {
        if (!configured()) return "error: set the device, service and characteristic first\n";
        std::lock_guard<std::mutex> lock(procedure_lock);
        if (test_state == 1) return "error: test in progress\n";
        start_worker();
        Job job = {};
        job.command = -3;
        if (xQueueSend(queue, &job, 0) != pdTRUE) return "error: Bluetooth queue is busy\n";
        test_state = 1;
        test_error.clear();
        return "ok\n";
    }
    if (sub == "target" && (args.size() == 3 || args.size() == 4)) {
        uint8_t mac[6];
        if (!parse_mac(args[2], mac)) return "error: the address is AA:BB:CC:DD:EE:FF\n";
        const bool random = args.size() == 4 && args[3] == "random";
        if (!store_str("mac", mac_text(mac)) || !store_str("atype", random ? "random" : "")) return "storing failed\n";
        std::memcpy(settings.mac, mac, 6);
        settings.have_mac = true;
        settings.random_address = random;
        if (queue != nullptr) on_idle(nullptr);  // the next command connects to the new one
        return "ok\n";
    }
    if ((sub == "service" || sub == "characteristic") && args.size() == 3) {
        if (!valid_uuid(args[2])) return "error: a UUID: 4 hex digits (FFE0) or 0000ffe0-0000-1000-8000-00805f9b34fb\n";
        if (!store_str(sub == "service" ? "svc" : "chr", args[2])) return "storing failed\n";
        (sub == "service" ? settings.service : settings.characteristic) = args[2];
        if (queue != nullptr) on_idle(nullptr);
        return "ok\n";
    }
    if (sub == "write" && args.size() == 3 && (args[2] == "response" || args[2] == "no-response")) {
        if (!store_str("wmode", args[2] == "no-response" ? "no-response" : "")) return "storing failed\n";
        settings.no_response = args[2] == "no-response";
        return "ok\n";
    }
    if (sub == "pair" && args.size() == 3 && (args[2] == "on" || args[2] == "off")) {
        const bool pair = args[2] == "on";
        if (!store_str("pair", pair ? "on" : "")) return "error: storing pairing mode failed\n";
        settings.pair = pair;
        if (queue != nullptr) on_idle(nullptr);
        return "ok\n";
    }
    if (sub == "hello" && args.size() == 3) {
        std::vector<uint8_t> bytes;
        if (args[2] != "none" && !from_hex(args[2], bytes, MAX_BYTES)) return "error: hex bytes (at most 64) or none\n";
        if (!store_str("hello", hex(bytes))) return "storing failed\n";
        settings.hello = bytes;
        return "ok\n";
    }
    if (sub == "idle" && args.size() == 3) {
        const int s = std::atoi(args[2].c_str());
        if (s < 0 || s > 3600) return "error: 0-3600 seconds (3600: stay connected)\n";
        if (!store_str("idle", std::to_string(s))) return "storing failed\n";
        settings.idle_s = static_cast<uint16_t>(s);
        return "ok\n";
    }
    if (sub == "cmd" && (args.size() == 7 || args.size() == 9)) {
        const int n = std::atoi(args[2].c_str());
        Command c;
        std::vector<uint8_t> label;
        if (n < 0 || n >= COMMANDS) return "error: command 0-7\n";
        if (!from_hex(args[3], c.bytes, MAX_BYTES) || c.bytes.empty()) return "error: the bytes as hex, 1 to 64\n";
        const int times = std::atoi(args[4].c_str()), gap = std::atoi(args[5].c_str());
        if (times < 1 || times > MAX_TIMES) return "error: sent 1-50 times\n";
        if (gap < 0 || gap > MAX_GAP_MS) return "error: 0-2000 ms between repeats\n";
        if (!from_hex(args[6], label, 32) || label.empty()) return "error: the label as hex, 1 to 32 bytes\n";
        c.times = static_cast<uint8_t>(times);
        c.gap_ms = static_cast<uint16_t>(gap);
        c.label.assign(label.begin(), label.end());
        if (args.size() == 9) {
            if (!from_hex(args[7], c.finish, MAX_BYTES) || c.finish.empty()) return "error: finish as hex, 1 to 64 bytes\n";
            const int finish_gap = std::atoi(args[8].c_str());
            if (finish_gap < 0 || finish_gap > MAX_FINISH_GAP_MS) return "error: finish gap 0-5000 ms\n";
            c.finish_gap_ms = static_cast<uint16_t>(finish_gap);
        }
        char key[4];
        std::snprintf(key, sizeof(key), "c%d", n);
        if (!store_str(key, command_text(c))) return "storing failed\n";
        settings.commands[n] = c;
        return "ok\n";
    }
    if (sub == "clear" && args.size() == 3) {
        const int n = std::atoi(args[2].c_str());
        if (n < 0 || n >= COMMANDS) return "error: command 0-7\n";
        char key[4];
        std::snprintf(key, sizeof(key), "c%d", n);
        if (!store_str(key, "")) return "storing failed\n";
        settings.commands[n] = Command();
        return "ok\n";
    }
    if (sub == "send" && args.size() == 3) {
        const int n = std::atoi(args[2].c_str());
        if (!configured()) return "error: set the device, service and characteristic first\n";
        return send(n) ? "sending\n" : "error: no such command\n";
    }
    if (sub == "raw" && args.size() == 3) {
        std::vector<uint8_t> bytes;
        if (!configured()) return "error: set the device, service and characteristic first\n";
        if (!from_hex(args[2], bytes, MAX_BYTES) || bytes.empty()) return "error: hex bytes, 1 to 64\n";
        start_worker();
        Job job = {};
        job.command = -1;
        job.raw_len = static_cast<uint8_t>(bytes.size());
        std::memcpy(job.raw, bytes.data(), bytes.size());
        return xQueueSend(queue, &job, 0) == pdTRUE ? "sending\n" : "busy\n";
    }
    if (sub == "disconnect") {
        if (queue != nullptr) on_idle(nullptr);
        return "ok\n";
    }
    return "bledev json | target <mac> [random] | service <uuid> | characteristic <uuid> | write response|no-response |\n"
           "  pair on|off | hello <hex>|none | idle <s> | cmd <n> <hex> <times> <ms> <label hex> [finish hex finish-ms] | clear <n> | send <n> | raw <hex> | disconnect\n";
}

}  // namespace ble_device
