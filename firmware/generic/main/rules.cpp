#include "rules.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io.h"
#include "nvs.h"

namespace rules {

namespace {

const char* TAG = "rules";
const char* NVS_NAMESPACE = "rules";
constexpr int STEPS = 6;
constexpr int MAX_DEPTH = 3;        // a rule's step may trigger another rule, this deep
constexpr int MAX_PER_SECOND = 10;  // runs of one rule; more is a loop
constexpr int16_t HYSTERESIS_C = 20;   // 0.2 °C before a threshold triggers again
constexpr int16_t HYSTERESIS_H = 100;  // 1 %
constexpr time_t CLOCK_VALID = 1704067200;  // 2024-01-01: earlier means the clock was never set

enum Trigger : uint8_t { T_NONE, T_PRESS, T_LONG, T_ACTIVE, T_INACTIVE, T_ON, T_OFF, T_ABOVE, T_BELOW,
                         T_HUMID_ABOVE, T_HUMID_BELOW, T_TIME, T_BOOT, T_COUNT };
const char* TRIGGER_NAMES[] = {"", "press", "long", "active", "inactive", "on", "off", "above", "below",
                               "humid_above", "humid_below", "time", "boot"};
enum Cond : uint8_t { C_NONE, C_ACTIVE, C_INACTIVE, C_ON, C_OFF, C_ABOVE, C_BELOW, C_HUMID_ABOVE,
                      C_HUMID_BELOW, C_TIME, C_COUNT };
const char* COND_NAMES[] = {"", "active", "inactive", "on", "off", "above", "below", "humid_above",
                            "humid_below", "time"};
enum Act : uint8_t { A_NONE, A_ON, A_OFF, A_TOGGLE, A_LEVEL, A_WAIT, A_ENABLE, A_DISABLE, A_TOGGLE_RULE, A_COUNT };
const char* ACT_NAMES[] = {"", "on", "off", "toggle", "level", "wait", "enable", "disable", "toggle_rule"};

struct Step {
    uint8_t kind;
    uint8_t target;  // connection or rule
    uint16_t value;  // level, or seconds to wait
};

// Stored as one NVS blob; a new field goes at the end with a size check
struct Rule {
    uint8_t used;
    uint8_t enabled;
    uint8_t matter;   // a switch in Matter
    uint8_t trigger;
    uint8_t t_slot;
    uint8_t days;     // time: bit 0 Monday .. bit 6 Sunday, 0 every day
    int16_t t_value;  // press count, centi threshold, minute of day
    uint8_t cond;
    uint8_t c_slot;
    int16_t c_value;
    int16_t c_value2;  // time: until this minute
    Step steps[STEPS];
    char name[24];
};

enum EvKind : uint8_t { EV_PRESS, EV_LONG, EV_INPUT, EV_OUTPUT, EV_CLIMATE, EV_CONTINUE, EV_TICK, EV_BOOT, EV_RUN };
struct Event {
    EvKind kind;
    uint8_t slot;    // connection, or rule for EV_CONTINUE / EV_RUN
    uint8_t depth;
    int16_t a;
    int16_t b;
    uint32_t gen;
};

// What a rule is doing now (not stored)
struct Run {
    esp_timer_handle_t timer = nullptr;
    uint8_t next = 0;      // the step after the wait
    uint32_t gen = 0;      // bumped at every start: an older wait is void
    int8_t side = -1;      // thresholds: -1 unknown, 0 not past, 1 past
    int64_t window_us = 0;
    uint8_t runs = 0;
};

Rule rules_[RULES] = {};
bool exposed_[RULES] = {};  // as at start: Matter's endpoints are fixed then
Run run_[RULES];
QueueHandle_t queue = nullptr;
TaskHandle_t task = nullptr;
int current_depth = 0;  // while the task runs a step
Listener listener = {};
int last_minute = -1;
char zone[64] = "";

bool store() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_blob(nvs, "rules", rules_, sizeof(rules_)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

void post(const Event& e) {
    if (queue == nullptr) return;
    Event copy = e;
    // Caused by a rule's own step: one level deeper
    if (xTaskGetCurrentTaskHandle() == task) copy.depth = static_cast<uint8_t>(current_depth + 1);
    if (copy.depth > MAX_DEPTH) return;
    xQueueSend(queue, &copy, 0);
}

bool clock_valid() { return time(nullptr) > CLOCK_VALID; }

// Minutes since midnight and the weekday bit (Monday = bit 0), local time
bool local_minute(int& minute, uint8_t& day_bit) {
    if (!clock_valid()) return false;
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    minute = t.tm_hour * 60 + t.tm_min;
    day_bit = static_cast<uint8_t>(1 << ((t.tm_wday + 6) % 7));
    return true;
}

bool in_window(int minute, int from, int to) {
    return from <= to ? minute >= from && minute < to : minute >= from || minute < to;  // over midnight
}

bool condition_holds(const Rule& r) {
    int16_t c = 0;
    uint16_t h = 0;
    switch (r.cond) {
        case C_NONE: return true;
        case C_ACTIVE: return io::input_active(r.c_slot);
        case C_INACTIVE: return !io::input_active(r.c_slot);
        case C_ON: return io::output_on(r.c_slot);
        case C_OFF: return !io::output_on(r.c_slot);
        case C_ABOVE: return io::climate(r.c_slot, c, h) && c > r.c_value;
        case C_BELOW: return io::climate(r.c_slot, c, h) && c < r.c_value;
        case C_HUMID_ABOVE: return io::climate(r.c_slot, c, h) && static_cast<int>(h) > r.c_value;
        case C_HUMID_BELOW: return io::climate(r.c_slot, c, h) && static_cast<int>(h) < r.c_value;
        case C_TIME: {
            int minute = 0;
            uint8_t day = 0;
            return local_minute(minute, day) && in_window(minute, r.c_value, r.c_value2);  // no clock: not now
        }
        default: return false;
    }
}

void set_rule_enabled(int i, bool on, bool tell_matter) {
    if (i < 0 || i >= RULES || !rules_[i].used || (rules_[i].enabled != 0) == on) return;
    rules_[i].enabled = on;
    if (!on) {
        ++run_[i].gen;  // a waiting rule stops too
        if (run_[i].timer) esp_timer_stop(run_[i].timer);
    }
    store();
    ESP_LOGI(TAG, "Rule %d '%s' %s", i, rules_[i].name, on ? "on" : "off");
    if (tell_matter && listener.enabled) listener.enabled(i, on);
}

void on_wait_done(void* arg) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Event e = {};
    e.kind = EV_CONTINUE;
    e.slot = static_cast<uint8_t>(i);
    e.gen = run_[i].gen;
    post(e);
}

// Runs a rule's steps from `from` on; a wait sets a timer and returns
void run_steps(int i, int from, int depth) {
    Rule& r = rules_[i];
    current_depth = depth;
    for (int s = from; s < STEPS; ++s) {
        const Step& st = r.steps[s];
        switch (st.kind) {
            case A_NONE: break;
            case A_ON: case A_OFF: case A_TOGGLE: {
                if (st.target >= io::SLOTS) break;
                const bool on = st.kind == A_ON || (st.kind == A_TOGGLE && !io::output_on(st.target));
                io::switch_output(st.target, on, 0);  // tells Matter too
                break;
            }
            case A_LEVEL:
                if (st.target < io::SLOTS) io::switch_output(st.target, true, static_cast<uint8_t>(st.value));
                break;
            case A_WAIT: {
                if (run_[i].timer == nullptr) {
                    esp_timer_create_args_t args = {};
                    args.callback = on_wait_done;
                    args.arg = reinterpret_cast<void*>(static_cast<intptr_t>(i));
                    args.name = "rule_wait";
                    esp_timer_create(&args, &run_[i].timer);
                }
                run_[i].next = static_cast<uint8_t>(s + 1);
                esp_timer_stop(run_[i].timer);
                esp_timer_start_once(run_[i].timer, static_cast<uint64_t>(st.value) * 1000000);
                current_depth = 0;
                return;
            }
            case A_ENABLE: set_rule_enabled(st.target, true, true); break;
            case A_DISABLE: set_rule_enabled(st.target, false, true); break;
            case A_TOGGLE_RULE:
                if (st.target < RULES) set_rule_enabled(st.target, !rules_[st.target].enabled, true);
                break;
            default: break;
        }
    }
    current_depth = 0;
}

// A rule's trigger happened: check, count, and start it (over)
void fire(int i, int depth) {
    Rule& r = rules_[i];
    if (!condition_holds(r)) return;
    Run& run = run_[i];
    const int64_t now = esp_timer_get_time();
    if (now - run.window_us > 1000000) {
        run.window_us = now;
        run.runs = 0;
    }
    if (++run.runs > MAX_PER_SECOND) {
        if (run.runs == MAX_PER_SECOND + 1) ESP_LOGW(TAG, "Rule %d '%s' runs too often: skipped", i, r.name);
        return;
    }
    ++run.gen;
    if (run.timer) esp_timer_stop(run.timer);
    ESP_LOGI(TAG, "Rule %d '%s'", i, r.name);
    run_steps(i, 0, depth);
}

// Whether a threshold trigger fires on this reading (crossing, with hysteresis)
bool crossed(Run& run, bool above, int value, int threshold, int hysteresis) {
    const bool past = above ? value > threshold : value < threshold;
    const bool back = above ? value <= threshold - hysteresis : value >= threshold + hysteresis;
    if (past && run.side != 1) {
        run.side = 1;
        return true;
    }
    if (back) run.side = 0;
    return false;
}

void handle(const Event& e) {
    if (e.kind == EV_CONTINUE) {
        const int i = e.slot;
        if (i < RULES && rules_[i].used && rules_[i].enabled && e.gen == run_[i].gen) {
            run_steps(i, run_[i].next, e.depth);
        }
        return;
    }
    if (e.kind == EV_RUN) {
        if (e.slot < RULES && rules_[e.slot].used) {
            ++run_[e.slot].gen;
            run_steps(e.slot, 0, e.depth);
        }
        return;
    }
    int minute = -1;
    uint8_t day = 0;
    if (e.kind == EV_TICK) {
        if (!local_minute(minute, day) || minute == last_minute) return;
        last_minute = minute;
    }
    for (int i = 0; i < RULES; ++i) {
        Rule& r = rules_[i];
        if (!r.used || !r.enabled) continue;
        bool hit = false;
        switch (r.trigger) {
            case T_PRESS: hit = e.kind == EV_PRESS && e.slot == r.t_slot && e.a == (r.t_value ? r.t_value : 1); break;
            case T_LONG: hit = e.kind == EV_LONG && e.slot == r.t_slot; break;
            case T_ACTIVE: hit = e.kind == EV_INPUT && e.slot == r.t_slot && e.a; break;
            case T_INACTIVE: hit = e.kind == EV_INPUT && e.slot == r.t_slot && !e.a; break;
            case T_ON: hit = e.kind == EV_OUTPUT && e.slot == r.t_slot && e.a; break;
            case T_OFF: hit = e.kind == EV_OUTPUT && e.slot == r.t_slot && !e.a; break;
            case T_ABOVE: case T_BELOW:
                hit = e.kind == EV_CLIMATE && e.slot == r.t_slot &&
                      crossed(run_[i], r.trigger == T_ABOVE, e.a, r.t_value, HYSTERESIS_C);
                break;
            case T_HUMID_ABOVE: case T_HUMID_BELOW:
                hit = e.kind == EV_CLIMATE && e.slot == r.t_slot &&
                      crossed(run_[i], r.trigger == T_HUMID_ABOVE, static_cast<uint16_t>(e.b), r.t_value, HYSTERESIS_H);
                break;
            case T_TIME: hit = e.kind == EV_TICK && minute == r.t_value && (r.days == 0 || (r.days & day)); break;
            case T_BOOT: hit = e.kind == EV_BOOT; break;
            default: break;
        }
        if (hit) fire(i, e.depth);
    }
}

void rules_task(void*) {
    Event e;
    while (true) {
        if (xQueueReceive(queue, &e, portMAX_DELAY) == pdTRUE) handle(e);
    }
}

void on_tick(void*) {
    Event e = {};
    e.kind = EV_TICK;
    post(e);
}

// ---------------------------------------------------------------- clock

void load_zone() {
    nvs_handle_t nvs;
    size_t size = sizeof(zone);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "tz", zone, &size) != ESP_OK) zone[0] = '\0';
        nvs_close(nvs);
    }
    setenv("TZ", zone[0] ? zone : "UTC0", 1);
    tzset();
}

// SNTP over whatever network the device has: on Thread it needs the border
// router's NAT64/DNS64; when that is missing the add-on sets the clock instead
void start_sntp() {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.wait_for_sync = false;
    if (esp_netif_sntp_init(&config) != ESP_OK) ESP_LOGW(TAG, "SNTP not started: the add-on sets the clock");
}

std::string clock_json() {
    char text[160];
    const time_t now = time(nullptr);
    char local[24] = "";
    if (clock_valid()) {
        struct tm t;
        localtime_r(&now, &t);
        std::strftime(local, sizeof(local), "%Y-%m-%d %H:%M", &t);
    }
    std::snprintf(text, sizeof(text), "{\"set\":%s,\"unix\":%lld,\"local\":\"%s\",\"zone\":\"%s\"}",
                  clock_valid() ? "true" : "false", static_cast<long long>(now), local, zone);
    return text;
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

std::string rule_json(int i) {
    const Rule& r = rules_[i];
    char head[200];
    std::snprintf(head, sizeof(head),
                  "{\"index\":%d,\"enabled\":%s,\"matter\":%s,\"exposed\":%s,\"when\":{\"kind\":\"%s\",\"slot\":%d,"
                  "\"value\":%d,\"days\":%d},\"if\":",
                  i, r.enabled ? "true" : "false", r.matter ? "true" : "false", exposed_[i] ? "true" : "false",
                  r.trigger < T_COUNT ? TRIGGER_NAMES[r.trigger] : "", r.t_slot, r.t_value, r.days);
    std::string out = head;
    if (r.cond == C_NONE || r.cond >= C_COUNT) {
        out += "null";
    } else {
        char cond[96];
        std::snprintf(cond, sizeof(cond), "{\"kind\":\"%s\",\"slot\":%d,\"value\":%d,\"value2\":%d}",
                      COND_NAMES[r.cond], r.c_slot, r.c_value, r.c_value2);
        out += cond;
    }
    out += ",\"do\":[";
    bool first = true;
    for (const Step& st : r.steps) {
        if (st.kind == A_NONE || st.kind >= A_COUNT) continue;
        char step[64];
        std::snprintf(step, sizeof(step), "%s{\"kind\":\"%s\",\"target\":%d,\"value\":%u}", first ? "" : ",",
                      ACT_NAMES[st.kind], st.target, st.value);
        out += step;
        first = false;
    }
    return out + "],\"name\":" + json_text(r.name) + "}";
}

int find(const char* const* names, int count, const std::string& word) {
    for (int k = 1; k < count; ++k) {
        if (word == names[k]) return k;
    }
    return -1;
}

// "kind:a:b:c" into its parts
std::vector<std::string> parts(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t colon = text.find(':', start);
        out.push_back(text.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) return out;
        start = colon + 1;
    }
}

bool number(const std::string& text, long low, long high, long& value) {
    char* end = nullptr;
    value = std::strtol(text.c_str(), &end, 10);
    return !text.empty() && *end == '\0' && value >= low && value <= high;
}

bool from_hex(const std::string& hex, std::string& out) {
    if (hex.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const std::string pair = hex.substr(i, 2);
        const long byte = std::strtol(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return false;
        out += static_cast<char>(byte);
    }
    return true;
}

// rule set <n> [enabled=0|1] [matter=0|1] when=<kind[:slot[:value[:days]]]> [if=<kind:slot[:value[:value2]]>]
//          do=<kind[:target[:value]]> ... [name-hex=<hex>]
std::string cmd_set(const std::vector<std::string>& args) {
    long index = 0;
    if (args.size() < 4 || !number(args[2], 0, RULES - 1, index)) return "rule set <0-11> when=... do=...\n";
    Rule r = {};
    r.used = 1;
    r.enabled = 1;
    int steps = 0;
    std::snprintf(r.name, sizeof(r.name), "Rule %ld", index + 1);
    for (size_t k = 3; k < args.size(); ++k) {
        const std::string& word = args[k];
        const size_t eq = word.find('=');
        if (eq == std::string::npos) return "options are key=value: " + word + "\n";
        const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
        const std::vector<std::string> p = parts(value);
        long a = 0, b = 0, c = 0;
        if (key == "enabled" || key == "matter") {
            if (value != "0" && value != "1") return key + "=0|1\n";
            (key == "enabled" ? r.enabled : r.matter) = value == "1";
        } else if (key == "name-hex") {
            std::string text;
            if (!from_hex(value, text) || text.empty() || text.size() >= sizeof(r.name) || text.find('"') != std::string::npos) {
                return "name: 1-23 characters, no quotes\n";
            }
            std::strncpy(r.name, text.c_str(), sizeof(r.name) - 1);
        } else if (key == "when") {
            const int kind = find(TRIGGER_NAMES, T_COUNT, p[0]);
            if (kind < 0) return "when=press|long|active|inactive|on|off|above|below|humid_above|humid_below|time|boot\n";
            r.trigger = static_cast<uint8_t>(kind);
            if (kind == T_TIME) {
                if (p.size() < 2 || !number(p[1], 0, 1439, a) || (p.size() > 2 && !number(p[2], 0, 127, b))) {
                    return "when=time:<minute 0-1439>[:<days 0-127>]\n";
                }
                r.t_value = static_cast<int16_t>(a);
                r.days = static_cast<uint8_t>(b);
            } else if (kind != T_BOOT) {
                if (p.size() < 2 || !number(p[1], 0, io::SLOTS - 1, a)) return "when=" + p[0] + ":<connection 0-7>...\n";
                r.t_slot = static_cast<uint8_t>(a);
                const bool threshold = kind >= T_ABOVE && kind <= T_HUMID_BELOW;
                if (threshold && (p.size() < 3 || !number(p[2], -4000, 12500, b))) return "when=" + p[0] + ":<n>:<centi value>\n";
                if (kind == T_PRESS && p.size() > 2 && !number(p[2], 1, 5, b)) return "when=press:<n>:<1-5 presses>\n";
                r.t_value = static_cast<int16_t>(threshold || kind == T_PRESS ? (b ? b : 1) : 0);
                if (threshold) r.t_value = static_cast<int16_t>(b);
            }
        } else if (key == "if") {
            const int kind = find(COND_NAMES, C_COUNT, p[0]);
            if (kind < 0) return "if=active|inactive|on|off|above|below|humid_above|humid_below|time\n";
            r.cond = static_cast<uint8_t>(kind);
            if (kind == C_TIME) {
                if (p.size() < 3 || !number(p[1], 0, 1439, a) || !number(p[2], 0, 1439, b)) {
                    return "if=time:<from minute>:<to minute>\n";
                }
                r.c_value = static_cast<int16_t>(a);
                r.c_value2 = static_cast<int16_t>(b);
            } else {
                if (p.size() < 2 || !number(p[1], 0, io::SLOTS - 1, a)) return "if=" + p[0] + ":<connection 0-7>...\n";
                r.c_slot = static_cast<uint8_t>(a);
                if (kind >= C_ABOVE && kind <= C_HUMID_BELOW) {
                    if (p.size() < 3 || !number(p[2], -4000, 12500, b)) return "if=" + p[0] + ":<n>:<centi value>\n";
                    r.c_value = static_cast<int16_t>(b);
                }
            }
        } else if (key == "do") {
            const int kind = find(ACT_NAMES, A_COUNT, p[0]);
            if (kind < 0) return "do=on|off|toggle|level|wait|enable|disable|toggle_rule\n";
            if (steps >= STEPS) return "at most 6 steps\n";
            Step& st = r.steps[steps++];
            st.kind = static_cast<uint8_t>(kind);
            if (kind == A_WAIT) {
                if (p.size() < 2 || !number(p[1], 1, 65535, a)) return "do=wait:<seconds 1-65535>\n";
                st.value = static_cast<uint16_t>(a);
            } else if (kind >= A_ENABLE) {
                if (p.size() < 2 || !number(p[1], 0, RULES - 1, a)) return "do=" + p[0] + ":<rule 0-11>\n";
                st.target = static_cast<uint8_t>(a);
            } else {
                if (p.size() < 2 || !number(p[1], 0, io::SLOTS - 1, a)) return "do=" + p[0] + ":<connection 0-7>\n";
                st.target = static_cast<uint8_t>(a);
                if (kind == A_LEVEL && (p.size() < 3 || !number(p[2], 1, 254, c))) return "do=level:<n>:<1-254>\n";
                st.value = static_cast<uint16_t>(c);
            }
        } else {
            return "unknown option " + key + "\n";
        }
    }
    if (r.trigger == T_NONE) return "a rule needs when=\n";
    if (steps == 0) return "a rule needs at least one do=\n";
    const bool was_exposed = exposed_[index];
    const bool matter_changed = r.matter != rules_[index].matter || (r.matter && std::strcmp(r.name, rules_[index].name) != 0);
    ++run_[index].gen;  // a waiting old version stops
    if (run_[index].timer) esp_timer_stop(run_[index].timer);
    run_[index] = Run();
    rules_[index] = r;
    if (!store()) return "storing failed\n";
    if (was_exposed && listener.enabled) listener.enabled(static_cast<int>(index), r.enabled);
    return matter_changed ? "ok: the switch in Matter changes after the next restart\n" : "ok\n";
}

}  // namespace

const char* const* factory_rules = nullptr;
int factory_count = 0;

void set_listener(const Listener& l) { listener = l; }

void set_factory(const char* const* options, int count) {
    factory_rules = options;
    factory_count = count;
}

void load() {
    nvs_handle_t nvs;
    bool stored = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = 0;
        stored = nvs_get_blob(nvs, "rules", nullptr, &size) == ESP_OK;
        size = 0;
        if (nvs_get_blob(nvs, "rules", nullptr, &size) == ESP_OK && size == sizeof(rules_)) {
            nvs_get_blob(nvs, "rules", rules_, &size);
        } else if (size != 0) {
            ESP_LOGE(TAG, "Stored rules unreadable (%u bytes): none", static_cast<unsigned>(size));
        }
        nvs_close(nvs);
    }
    if (!stored) {
        // Nothing set on the page yet: the spec's rules, through the same parser
        for (int i = 0; i < factory_count && i < RULES; ++i) {
            std::vector<std::string> args = {"rule", "set"};  // "<id> <options>": the id comes first
            const std::string text = factory_rules[i];
            size_t start = 0;
            while (start < text.size()) {
                const size_t space = text.find(' ', start);
                args.push_back(text.substr(start, space == std::string::npos ? std::string::npos : space - start));
                if (space == std::string::npos) break;
                start = space + 1;
            }
            const std::string reply = cmd_set(args);
            if (reply.rfind("ok", 0) != 0) ESP_LOGE(TAG, "Spec rule %d: %s", i, reply.c_str());
        }
    }
    for (int i = 0; i < RULES; ++i) {
        rules_[i].name[sizeof(rules_[i].name) - 1] = '\0';
        exposed_[i] = rules_[i].used && rules_[i].matter;
    }
    load_zone();
}

void start() {
    queue = xQueueCreate(24, sizeof(Event));
    xTaskCreate(rules_task, "rules", 4096, nullptr, 4, &task);
    esp_timer_create_args_t args = {};
    args.callback = on_tick;
    args.name = "rules_tick";
    esp_timer_handle_t tick = nullptr;
    if (esp_timer_create(&args, &tick) == ESP_OK) esp_timer_start_periodic(tick, 10 * 1000000);
    start_sntp();
    Event e = {};
    e.kind = EV_BOOT;
    post(e);
}

void on_press(int slot, uint8_t count) {
    Event e = {};
    e.kind = EV_PRESS;
    e.slot = static_cast<uint8_t>(slot);
    e.a = count;
    post(e);
}

void on_long(int slot) {
    Event e = {};
    e.kind = EV_LONG;
    e.slot = static_cast<uint8_t>(slot);
    post(e);
}

void on_input(int slot, bool active) {
    Event e = {};
    e.kind = EV_INPUT;
    e.slot = static_cast<uint8_t>(slot);
    e.a = active;
    post(e);
}

void on_output(int slot, bool on) {
    Event e = {};
    e.kind = EV_OUTPUT;
    e.slot = static_cast<uint8_t>(slot);
    e.a = on;
    post(e);
}

void on_climate(int slot, int16_t centi_celsius, uint16_t centi_percent) {
    Event e = {};
    e.kind = EV_CLIMATE;
    e.slot = static_cast<uint8_t>(slot);
    e.a = centi_celsius;
    e.b = static_cast<int16_t>(centi_percent);
    post(e);
}

bool exposed(int rule) { return rule >= 0 && rule < RULES && exposed_[rule]; }
bool enabled(int rule) { return rule >= 0 && rule < RULES && rules_[rule].used && rules_[rule].enabled; }
const char* name(int rule) { return rule >= 0 && rule < RULES ? rules_[rule].name : ""; }
void set_enabled(int rule, bool on) { set_rule_enabled(rule, on, false); }

std::string command(const std::vector<std::string>& args) {
    const std::string sub = args.size() >= 2 ? args[1] : "json";
    long index = 0;
    if (sub == "set") return cmd_set(args);
    if (sub == "clear" && args.size() == 3 && number(args[2], 0, RULES - 1, index)) {
        ++run_[index].gen;
        if (run_[index].timer) esp_timer_stop(run_[index].timer);
        const bool was = rules_[index].matter;
        rules_[index] = Rule();
        if (!store()) return "storing failed\n";
        return was ? "ok: the switch leaves Matter after the next restart\n" : "ok\n";
    }
    if (sub == "enable" && args.size() == 4 && number(args[2], 0, RULES - 1, index)) {
        if (!rules_[index].used) return "no such rule\n";
        set_rule_enabled(static_cast<int>(index), args[3] == "on", true);
        return "ok\n";
    }
    if (sub == "run" && args.size() == 3 && number(args[2], 0, RULES - 1, index)) {
        if (!rules_[index].used) return "no such rule\n";
        Event e = {};
        e.kind = EV_RUN;
        e.slot = static_cast<uint8_t>(index);
        post(e);
        return "ok\n";
    }
    if (sub != "json") return "rule [json|set <n> ...|clear <n>|enable <n> on|off|run <n>]\n";
    std::string out = "{\"max\":" + std::to_string(RULES) + ",\"steps\":" + std::to_string(STEPS) +
                      ",\"clock\":" + clock_json() + ",\"rules\":[";
    bool first = true;
    for (int i = 0; i < RULES; ++i) {
        if (!rules_[i].used) continue;
        out += (first ? "" : ",") + rule_json(i);
        first = false;
    }
    return out + "]}\n";
}

std::string time_command(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[1] == "set") {
        long long value = std::strtoll(args[2].c_str(), nullptr, 10);
        if (value < CLOCK_VALID) return "time set <unix seconds>\n";
        struct timeval tv = {static_cast<time_t>(value), 0};
        settimeofday(&tv, nullptr);
        last_minute = -1;
        return "ok\n";
    }
    if (args.size() == 3 && args[1] == "zone") {
        if (args[2].size() >= sizeof(zone)) return "zone: a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3\n";
        std::strncpy(zone, args[2].c_str(), sizeof(zone) - 1);
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "tz", zone);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        setenv("TZ", zone, 1);
        tzset();
        last_minute = -1;
        return "ok\n";
    }
    return clock_json() + "\n";
}

}  // namespace rules
