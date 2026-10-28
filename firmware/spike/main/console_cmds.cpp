// Serial console for the F0 spike: configure beds and test movements.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bed_link.h"
#include "esp_console.h"

namespace {

bool parse_index(const char* text, int& index) {
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value >= bed_link::MAX_BEDS) {
        std::printf("bed index must be 0..%d\n", bed_link::MAX_BEDS - 1);
        return false;
    }
    index = static_cast<int>(value);
    return true;
}

bool parse_mac(const char* text, uint8_t mac[6]) {
    unsigned int b[6];
    if (std::strlen(text) != 17 ||
        std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        std::printf("MAC must look like AA:BB:CC:DD:EE:01\n");
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

int cmd_scan(int, char**) {
    bed_link::run_in_host([] { bed_link::print_scan(); });
    return 0;
}

int cmd_status(int, char**) {
    bed_link::run_in_host([] { bed_link::print_status(); });
    return 0;
}

int cmd_bed(int argc, char** argv) {
    int index;
    if (argc >= 5 && std::strcmp(argv[1], "set") == 0 && parse_index(argv[2], index)) {
        uint8_t mac[6];
        if (!parse_mac(argv[3], mac)) {
            return 1;
        }
        std::string pin = argv[4];
        std::string name = argc >= 6 ? argv[5] : "Bed " + std::to_string(index);
        bool ok = false;
        bed_link::run_in_host([&] { ok = bed_link::configure(index, mac, pin, name); });
        std::printf(ok ? "stored\n" : "PIN must be 4 digits\n");
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "clear") == 0 && parse_index(argv[2], index)) {
        bed_link::run_in_host([&] { bed_link::clear(index); });
        std::printf("cleared\n");
        return 0;
    }
    std::printf("usage: bed set <0|1> <MAC> <PIN> [name] | bed clear <0|1>\n");
    return 1;
}

int cmd_move(int argc, char** argv) {
    int index;
    if (argc != 5 || !parse_index(argv[1], index)) {
        std::printf("usage: move <0|1> <head|feet|both> <up|down> <seconds>\n");
        return 1;
    }
    bed_link::Part part;
    if (std::strcmp(argv[2], "head") == 0) {
        part = bed_link::Part::Head;
    } else if (std::strcmp(argv[2], "feet") == 0) {
        part = bed_link::Part::Feet;
    } else if (std::strcmp(argv[2], "both") == 0) {
        part = bed_link::Part::Both;
    } else {
        std::printf("part must be head, feet or both\n");
        return 1;
    }
    const bool up = std::strcmp(argv[3], "up") == 0;
    if (!up && std::strcmp(argv[3], "down") != 0) {
        std::printf("direction must be up or down\n");
        return 1;
    }
    const float seconds = std::strtof(argv[4], nullptr);
    if (seconds < 0.3f || seconds > 60.0f) {
        std::printf("seconds must be 0.3..60\n");
        return 1;
    }
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::move(index, part, up, seconds); });
    std::printf(ok ? "moving\n" : "bed not ready\n");
    return ok ? 0 : 1;
}

int cmd_stop(int argc, char** argv) {
    int index;
    if (argc != 2 || !parse_index(argv[1], index)) {
        std::printf("usage: stop <0|1>\n");
        return 1;
    }
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::stop(index); });
    std::printf(ok ? "stopped\n" : "bed not connected\n");
    return ok ? 0 : 1;
}

int cmd_light(int argc, char** argv) {
    int index;
    if (argc != 3 || !parse_index(argv[1], index) ||
        (std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0)) {
        std::printf("usage: light <0|1> <on|off>\n");
        return 1;
    }
    const bool on = std::strcmp(argv[2], "on") == 0;
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::light(index, on); });
    std::printf(ok ? "ok\n" : "bed not ready\n");
    return ok ? 0 : 1;
}

void add(const char* name, const char* help, esp_console_cmd_func_t func) {
    esp_console_cmd_t cmd = {};
    cmd.command = name;
    cmd.help = help;
    cmd.func = func;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace

void console_start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "octo>";
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    esp_console_register_help_command();
    add("scan", "List nearby BLE devices with a name or the Octo service", cmd_scan);
    add("status", "Link state, statistics and free heap", cmd_status);
    add("bed", "bed set <0|1> <MAC> <PIN> [name] | bed clear <0|1>", cmd_bed);
    add("move", "move <0|1> <head|feet|both> <up|down> <seconds>", cmd_move);
    add("stop", "stop <0|1>", cmd_stop);
    add("light", "light <0|1> <on|off>", cmd_light);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
