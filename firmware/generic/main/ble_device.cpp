#include "ble_device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ble_scan.h"
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
constexpr int CONNECT_MS = 10000;
constexpr uint16_t NO_CONN = 0xFFFF;

struct Command {
    std::string label;
    std::vector<uint8_t> bytes;
    uint8_t times = 1;
    uint16_t gap_ms = 0;
};

struct Settings {
    uint8_t mac[6] = {};  // as written: AA:BB:... is {0xAA, ...}
    bool have_mac = false;
    bool random_address = false;
    std::string service = "";
    std::string characteristic = "";
    bool no_response = false;
    std::vector<uint8_t> hello;
    uint16_t idle_s = 30;
    Command commands[COMMANDS];
};

Settings settings;
std::string last_error;
int sent_count = 0;

// The link, owned by the worker task (and the host's callbacks)
volatile uint16_t conn = NO_CONN;
uint16_t value_handle = 0;
uint16_t service_start = 0, service_end = 0;
uint16_t mtu = 23;
EventGroupHandle_t events = nullptr;
constexpr EventBits_t CONNECTED = 1, FAILED = 2, DONE = 4, DISCONNECTED = 8;
volatile int op_status = 0;

QueueHandle_t queue = nullptr;
struct Job {
    int command;                // -1: raw bytes, -2: disconnect
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
    return hex(c.bytes) + "\t" + std::to_string(c.times) + "\t" + std::to_string(c.gap_ms) + "\t" + c.label;
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
    c.label = text.substr(d + 1);
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
            value_handle = 0;
            mtu = 23;
            xEventGroupSetBits(events, DISCONNECTED | FAILED);
            ESP_LOGI(TAG, "Disconnected (%d)", event->disconnect.reason);
            break;
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

int on_characteristic(uint16_t, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
    if (error->status == 0 && chr != nullptr) {
        if (value_handle == 0) value_handle = chr->val_handle;
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

bool write(const uint8_t* data, size_t len) {
    if (conn == NO_CONN || value_handle == 0) return false;
    const int rc = settings.no_response ? ble_gattc_write_no_rsp_flat(conn, value_handle, data, len)
                                        : ble_gattc_write_flat(conn, value_handle, data, len, nullptr, nullptr);
    if (rc != 0) {
        last_error = "write failed (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

bool connect() {
    if (conn != NO_CONN && value_handle != 0) return true;
    if (!settings.have_mac || settings.service.empty() || settings.characteristic.empty()) {
        last_error = "not set up: device, service and characteristic first";
        return false;
    }
    if (!ble_scan::ready()) {
        last_error = "Bluetooth is not running yet";
        return false;
    }
    ble_uuid_any_t service_uuid, chr_uuid;
    if (!to_uuid(settings.service, service_uuid) || !to_uuid(settings.characteristic, chr_uuid)) {
        last_error = "service or characteristic is not a UUID";
        return false;
    }
    ble_addr_t addr = {};
    addr.type = settings.random_address ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; ++i) addr.val[i] = settings.mac[5 - i];  // NimBLE keeps it reversed

    ble_scan::pause();
    bool ok = false;
    xEventGroupClearBits(events, CONNECTED | FAILED | DONE | DISCONNECTED);
    op_status = 0;
    int rc = ble_gap_connect(ble_scan::own_address_type(), &addr, CONNECT_MS, nullptr, on_gap, nullptr);
    if (rc != 0) {
        last_error = "connecting failed (" + std::to_string(rc) + ")";
    } else if (!wait(CONNECTED, CONNECT_MS + 2000)) {
        last_error = "the device did not answer (in range, and not connected to something else?)";
        ble_gap_conn_cancel();
    } else {
        service_start = service_end = 0;
        value_handle = 0;
        xEventGroupClearBits(events, DONE);
        rc = ble_gattc_disc_svc_by_uuid(conn, &service_uuid.u, on_service, nullptr);
        if (rc != 0 || !wait(DONE, 5000) || service_start == 0) {
            last_error = "the service " + settings.service + " was not found on the device";
        } else {
            xEventGroupClearBits(events, DONE);
            rc = ble_gattc_disc_chrs_by_uuid(conn, service_start, service_end, &chr_uuid.u, on_characteristic, nullptr);
            if (rc != 0 || !wait(DONE, 5000) || value_handle == 0) {
                last_error = "the characteristic " + settings.characteristic + " was not found in that service";
            } else {
                ok = true;
            }
        }
        if (ok) {
            size_t longest = settings.hello.size();
            for (const Command& c : settings.commands) longest = std::max(longest, c.bytes.size());
            if (longest > 20) {
                xEventGroupClearBits(events, DONE);
                if (ble_gattc_exchange_mtu(conn, on_mtu, nullptr) == 0) wait(DONE, 3000);
            }
            if (!settings.hello.empty()) {
                write(settings.hello.data(), settings.hello.size());
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            ESP_LOGI(TAG, "Connected to %s", mac_text(settings.mac).c_str());
        } else if (conn != NO_CONN) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
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
        if (!connect()) {
            ESP_LOGW(TAG, "%s", last_error.c_str());
            continue;
        }
        if (job.command == -1) {
            if (write(job.raw, job.raw_len)) sent_count++;
        } else {
            const Command& c = settings.commands[job.command];
            for (int i = 0; i < c.times; ++i) {
                if (!write(c.bytes.data(), c.bytes.size())) break;
                if (i + 1 < c.times && c.gap_ms > 0) vTaskDelay(pdMS_TO_TICKS(c.gap_ms));
            }
            sent_count++;
            ESP_LOGI(TAG, "Sent '%s'", c.label.c_str());
        }
        last_error.clear();
        if (settings.idle_s == 0) disconnect();
        else keep_alive();
    }
}

void start_worker() {
    if (queue != nullptr) return;
    events = xEventGroupCreate();
    queue = xQueueCreate(8, sizeof(Job));
    esp_timer_create_args_t args = {};
    args.callback = on_idle;
    args.name = "bledev_idle";
    esp_timer_create(&args, &idle_timer);
    xTaskCreate(worker, "bledev", 4096, nullptr, 5, nullptr);
}

std::string json() {
    std::string out = "{\"bledev\":{";
    out += "\"mac\":" + json_text(settings.have_mac ? mac_text(settings.mac) : "");
    out += ",\"random\":" + std::string(settings.random_address ? "true" : "false");
    out += ",\"service\":" + json_text(settings.service);
    out += ",\"characteristic\":" + json_text(settings.characteristic);
    out += ",\"no_response\":" + std::string(settings.no_response ? "true" : "false");
    out += ",\"hello\":" + json_text(hex(settings.hello));
    out += ",\"idle\":" + std::to_string(settings.idle_s);
    out += ",\"connected\":" + std::string(conn != NO_CONN && value_handle != 0 ? "true" : "false");
    out += ",\"sent\":" + std::to_string(sent_count);
    out += ",\"error\":" + json_text(last_error);
    out += ",\"commands\":[";
    for (int i = 0; i < COMMANDS; ++i) {
        const Command& c = settings.commands[i];
        out += std::string(i ? "," : "") + "{\"label\":" + json_text(c.label) + ",\"bytes\":" + json_text(hex(c.bytes)) +
               ",\"times\":" + std::to_string(c.times) + ",\"gap\":" + std::to_string(c.gap_ms) + "}";
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
    if (sub == "cmd" && args.size() == 7) {
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
           "  hello <hex>|none | idle <s> | cmd <n> <hex> <times> <ms> <label hex> | clear <n> | send <n> | raw <hex> | disconnect\n";
}

}  // namespace ble_device
