#include "commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "bed_link.h"
#include "boot_guard.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"

namespace {

using Args = std::vector<std::string>;

const char* HELP =
    "status                                  link state, statistics, free heap\n"
    "events                                  recent connection events\n"
    "scan                                    nearby BLE devices with a name or the Octo service\n"
    "bed set <0|1> <MAC> <PIN> [name]        configure a bed (PIN is never shown)\n"
    "bed name <0|1> <name>                   rename a bed\n"
    "bed clear <0|1>                         forget a bed\n"
    "move <0|1> <head|legs|both> <up|down> <seconds>\n"
    "stop <0|1>                              stop the motors\n"
    "light <0|1> <on|off>                    under-bed light\n"
    "travel <0|1> <head|legs> <seconds>      full travel time of a motor\n"
    "calibrate <0|1> <head|legs>             raise from flat; then 'calibrate <0|1> stop' at the top\n"
    "json                                    state for the settings page\n"
    "system                                  firmware slots, reset reason, network\n"
    "revert                                  boot the other firmware slot\n"
    "reboot                                  restart\n";

struct ExtraCommand {
    const char* name;
    const char* help_line;
    CommandHandler handler;
};

constexpr int MAX_EXTRA_COMMANDS = 4;
ExtraCommand extra_commands[MAX_EXTRA_COMMANDS];
int extra_count = 0;

bool parse_index(const std::string& text, int& index) {
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || value < 0 || value >= bed_link::MAX_BEDS) {
        return false;
    }
    index = static_cast<int>(value);
    return true;
}

bool parse_mac(const std::string& text, uint8_t mac[6]) {
    unsigned int b[6];
    if (text.size() != 17 ||
        std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

// The words from `first` on, joined with spaces
std::string rest(const Args& args, size_t first) {
    std::string text;
    for (size_t i = first; i < args.size(); ++i) {
        text += (i > first ? " " : "") + args[i];
    }
    return text;
}

std::string cmd_bed(const Args& args) {
    int index;
    if (args.size() >= 5 && args[1] == "set" && parse_index(args[2], index)) {
        uint8_t mac[6];
        if (!parse_mac(args[3], mac)) {
            return "MAC must look like AA:BB:CC:DD:EE:01\n";
        }
        std::string name = rest(args, 5);  // names may contain spaces
        if (name.empty()) {
            name = "Bed " + std::to_string(index + 1);
        }
        bool ok = false;
        bed_link::run_in_host([&] { ok = bed_link::configure(index, mac, args[4], name); });
        return ok ? "stored\n" : "PIN must be 4 digits\n";
    }
    if (args.size() >= 4 && args[1] == "name" && parse_index(args[2], index)) {
        bool ok = false;
        const std::string name = rest(args, 3);
        bed_link::run_in_host([&] { ok = bed_link::rename(index, name); });
        return ok ? "renamed\n" : "no bed in that slot\n";
    }
    if (args.size() == 3 && args[1] == "clear" && parse_index(args[2], index)) {
        bed_link::run_in_host([&] { bed_link::clear(index); });
        return "cleared\n";
    }
    return "usage: bed set <0|1> <MAC> <PIN> [name] | bed name <0|1> <name> | bed clear <0|1>\n";
}

// "legs" and "feet" are the same motor
bool parse_part(const std::string& text, bool allow_both, bed_link::Part& part) {
    if (text == "head") {
        part = bed_link::Part::Head;
    } else if (text == "legs" || text == "feet") {
        part = bed_link::Part::Feet;
    } else if (text == "both" && allow_both) {
        part = bed_link::Part::Both;
    } else {
        return false;
    }
    return true;
}

std::string cmd_travel(const Args& args) {
    int index;
    bed_link::Part part;
    if (args.size() != 4 || !parse_index(args[1], index) || !parse_part(args[2], false, part)) {
        return "usage: travel <0|1> <head|legs> <seconds>\n";
    }
    const float seconds = std::strtof(args[3].c_str(), nullptr);
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::set_travel(index, part, seconds); });
    return ok ? "travel time stored\n" : "bed not configured\n";
}

std::string cmd_calibrate(const Args& args) {
    int index;
    if (args.size() != 3 || !parse_index(args[1], index)) {
        return "usage: calibrate <0|1> <head|legs|stop>\n";
    }
    if (args[2] == "stop") {
        float seconds = -1.0f;
        bed_link::run_in_host([&] { seconds = bed_link::calibrate_stop(index); });
        if (seconds < 0.0f) {
            return "not calibrating, or the time was out of range: nothing stored\n";
        }
        char text[48];
        std::snprintf(text, sizeof(text), "travel time %.1f s stored\n", seconds);
        return text;
    }
    bed_link::Part part;
    if (!parse_part(args[2], false, part)) {
        return "usage: calibrate <0|1> <head|legs|stop>\n";
    }
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::calibrate_start(index, part); });
    return ok ? "raising; send 'calibrate <bed> stop' when it reaches the top\n" : "bed not ready\n";
}

std::string cmd_move(const Args& args) {
    int index;
    if (args.size() != 5 || !parse_index(args[1], index)) {
        return "usage: move <0|1> <head|feet|both> <up|down> <seconds>\n";
    }
    bed_link::Part part;
    if (!parse_part(args[2], true, part)) {
        return "part must be head, legs or both\n";
    }
    if (args[3] != "up" && args[3] != "down") {
        return "direction must be up or down\n";
    }
    const bool up = args[3] == "up";
    const float seconds = std::strtof(args[4].c_str(), nullptr);
    if (seconds < 0.3f || seconds > bed_link::MAX_TRAVEL_S + 5.0f) {
        return "seconds must be 0.3..125\n";
    }
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::move(index, part, up, seconds); });
    return ok ? "moving\n" : "bed not ready\n";
}

std::string cmd_stop(const Args& args) {
    int index;
    if (args.size() != 2 || !parse_index(args[1], index)) {
        return "usage: stop <0|1>\n";
    }
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::stop(index); });
    return ok ? "stopped\n" : "bed not connected\n";
}

std::string cmd_light(const Args& args) {
    int index;
    if (args.size() != 3 || !parse_index(args[1], index) || (args[2] != "on" && args[2] != "off")) {
        return "usage: light <0|1> <on|off>\n";
    }
    const bool on = args[2] == "on";
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::light(index, on); });
    return ok ? "ok\n" : "bed not ready\n";
}

void restart_soon(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));  // let the reply go out first
    esp_restart();
}

std::string nvs_text() {
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) != ESP_OK) {
        return "nvs: unavailable\n";
    }
    return "nvs: " + std::to_string(stats.used_entries) + " of " + std::to_string(stats.total_entries) +
           " entries used, " + std::to_string(stats.free_entries) + " free, " +
           std::to_string(stats.namespace_count) + " namespaces\n";
}

std::string cmd_revert() {
    bed_link::run_in_host([] { bed_link::stop_all(); });
    std::string why;
    if (!boot_guard::select_other_slot(why)) {
        return "cannot revert: " + why + "\n";
    }
    xTaskCreate(restart_soon, "restart", 2048, nullptr, 5, nullptr);
    return "booting the other slot (" + why + ")\n";
}

}  // namespace

void add_command(const char* name, const char* help_line, CommandHandler handler) {
    if (extra_count < MAX_EXTRA_COMMANDS) {
        extra_commands[extra_count++] = {name, help_line, handler};
    }
}

std::string run_command(const std::string& line) {
    Args args;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        args.push_back(word);
    }
    if (args.empty() || args[0] == "help") {
        std::string out = HELP;
        for (int i = 0; i < extra_count; ++i) {
            out += extra_commands[i].help_line;
        }
        return out;
    }
    const std::string& cmd = args[0];
    if (cmd == "status") {
        std::string out;
        bed_link::run_in_host([&] { out = bed_link::status_text(); });
        return out;
    }
    if (cmd == "events") {
        std::string out;
        bed_link::run_in_host([&] { out = bed_link::events_text(); });
        return out;
    }
    if (cmd == "scan") {
        std::string out;
        bed_link::run_in_host([&] { out = bed_link::scan_text(); });
        return out;
    }
    if (cmd == "bed") return cmd_bed(args);
    if (cmd == "move") return cmd_move(args);
    if (cmd == "travel") return cmd_travel(args);
    if (cmd == "calibrate") return cmd_calibrate(args);
    if (cmd == "json") {
        std::string out;
        bed_link::run_in_host([&] { out = bed_link::state_json(); });
        return out;
    }
    if (cmd == "stop") return cmd_stop(args);
    if (cmd == "light") return cmd_light(args);
    if (cmd == "system") return boot_guard::describe() + nvs_text() + net::describe();
    if (cmd == "revert") return cmd_revert();
    if (cmd == "reboot") {
        bed_link::run_in_host([] { bed_link::stop_all(); });
        xTaskCreate(restart_soon, "restart", 2048, nullptr, 5, nullptr);
        return "rebooting\n";
    }
    for (int i = 0; i < extra_count; ++i) {
        if (cmd == extra_commands[i].name) {
            return extra_commands[i].handler(args);
        }
    }
    return "unknown command, try 'help'\n";
}

namespace {

// Serial console: every command name forwards its whole line to run_command
int console_dispatch(int argc, char** argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        line += argv[i];
        line += ' ';
    }
    std::fputs(run_command(line).c_str(), stdout);
    return 0;
}

}  // namespace

void console_start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "octo>";
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    for (const char* name : {"help", "status", "events", "scan", "bed", "move", "stop", "light", "travel", "calibrate",
                             "json", "system", "revert", "reboot"}) {
        esp_console_cmd_t cmd = {};
        cmd.command = name;
        cmd.func = console_dispatch;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    for (int i = 0; i < extra_count; ++i) {
        esp_console_cmd_t cmd = {};
        cmd.command = extra_commands[i].name;
        cmd.func = console_dispatch;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
