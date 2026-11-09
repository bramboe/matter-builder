#include "commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "boot_guard.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "log_buffer.h"
#include "mb_core.h"
#include "net.h"
#include "nvs.h"

namespace {

using Args = std::vector<std::string>;

const char* HELP =
    "system [json]                           firmware slots, reset reason, network (json: for the page)\n"
    "revert                                  boot the other firmware slot\n"
    "reboot                                  restart\n"
    "log [from]                              the log kept in RAM, from byte <from> on (4 KB at a time)\n";

struct ExtraCommand {
    const char* name;
    const char* help_line;
    CommandHandler handler;
};

constexpr int MAX_EXTRA_COMMANDS = 24;
ExtraCommand extra_commands[MAX_EXTRA_COMMANDS];
int extra_count = 0;
CommandHandler override_handler = nullptr;

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

const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "restart";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout (power dip)";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        default: return "other";
    }
}

std::string json_escaped(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out + "\"";
}

std::string slot_json(const esp_partition_t* part) {
    if (part == nullptr) return "null";
    esp_app_desc_t desc;
    const bool valid = esp_ota_get_partition_description(part, &desc) == ESP_OK;
    char address[16];
    std::snprintf(address, sizeof(address), "0x%lx", static_cast<unsigned long>(part->address));
    return std::string("{\"label\":") + json_escaped(part->label) + ",\"address\":\"" + address + "\"" +
           (valid ? ",\"app\":" + json_escaped(desc.project_name) + ",\"version\":" + json_escaped(desc.version) : "") + "}";
}

// The device's technical details, for the page's System card
std::string system_json() {
    const esp_app_desc_t* app = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BASE);
    char mac_text[18];
    std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_stats_t nvs = {};
    nvs_get_stats(nullptr, &nvs);
    std::string out = "{\"app\":" + json_escaped(app->project_name) + ",\"version\":" + json_escaped(app->version) +
                      ",\"built\":" + json_escaped((std::string(app->date) + " " + app->time).c_str()) +
                      ",\"idf\":" + json_escaped(esp_get_idf_version()) + ",\"chip\":" + json_escaped(CONFIG_IDF_TARGET) +
                      ",\"revision\":\"v" + std::to_string(chip.revision / 100) + "." + std::to_string(chip.revision % 100) +
                      "\",\"cores\":" + std::to_string(chip.cores) + ",\"mhz\":" + std::to_string(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) +
                      ",\"flash\":" + std::to_string(flash) + ",\"mac\":\"" + mac_text + "\"" +
                      ",\"heap\":" + std::to_string(esp_get_free_heap_size()) +
                      ",\"heap_min\":" + std::to_string(esp_get_minimum_free_heap_size()) +
                      ",\"heap_block\":" + std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) +
                      ",\"uptime\":" + std::to_string(esp_timer_get_time() / 1000000) +
                      ",\"reset\":" + json_escaped(reset_reason_name(esp_reset_reason())) +
                      ",\"running\":" + slot_json(esp_ota_get_running_partition()) +
                      ",\"other\":" + slot_json(esp_ota_get_next_update_partition(nullptr)) +
                      ",\"nvs_used\":" + std::to_string(nvs.used_entries) + ",\"nvs_total\":" + std::to_string(nvs.total_entries) +
                      ",\"net\":" + net::health_json() +
                      ",\"network\":\"" +
#if CONFIG_OPENTHREAD_ENABLED
                      "thread"
#else
                      "wifi"
#endif
                      "\"}\n";
    return out;
}

std::string cmd_revert() {
    mb_core::stop();
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

void set_command_override(CommandHandler handler) {
    override_handler = handler;
}

std::string run_command(const std::string& line) {
    Args args;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        args.push_back(word);
    }
    if (override_handler != nullptr) {
        std::string out = override_handler(args);
        if (!out.empty()) {
            return out;
        }
    }
    if (args.empty() || args[0] == "help") {
        std::string out = HELP;
        for (int i = 0; i < extra_count; ++i) {
            out += extra_commands[i].help_line;
        }
        return out;
    }
    const std::string& cmd = args[0];
    if (cmd == "system" && args.size() == 2 && args[1] == "json") return system_json();
    if (cmd == "system") return boot_guard::describe() + nvs_text() + net::describe();
    if (cmd == "revert") return cmd_revert();
    if (cmd == "log") return log_buffer::read(args.size() > 1 ? std::strtoul(args[1].c_str(), nullptr, 10) : 0, 4096);
    if (cmd == "reboot") {
        mb_core::stop();
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

// Not a command of run_command: the HTTP console must not be able to clear
// the password, only someone at the serial port
int console_password(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
        std::fputs(http_api::reset_password().c_str(), stdout);
    } else {
        std::fputs("password reset                          forget the page password (serial only)\n", stdout);
    }
    return 0;
}

}  // namespace

void console_start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mb>";
    repl_config.max_cmdline_length = 600;  // a pasted Thread dataset
    repl_config.task_stack_size = 8192;    // "io json" with sensor values overflowed the default 4 KB
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    // Without memory for it the device runs on without a serial console
    // (it aborted here on a classic ESP32 with Bluetooth on)
    if (esp_console_new_repl_uart(&uart_config, &repl_config, &repl) != ESP_OK) {
        std::fputs("serial console unavailable: not enough memory\n", stdout);
        return;
    }

    for (const char* name : {"help", "system", "revert", "reboot"}) {
        esp_console_cmd_t cmd = {};
        cmd.command = name;
        cmd.func = console_dispatch;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    // Configuration mode answers these itself, before the app registers them
    if (override_handler != nullptr) {
        for (const char* name : {"wifi", "thread", "access"}) {
            bool registered = false;
            for (int i = 0; i < extra_count; ++i) {
                if (std::strcmp(extra_commands[i].name, name) == 0) registered = true;
            }
            if (registered) continue;
            esp_console_cmd_t cmd = {};
            cmd.command = name;
            cmd.func = console_dispatch;
            ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
        }
    }
    for (int i = 0; i < extra_count; ++i) {
        esp_console_cmd_t cmd = {};
        cmd.command = extra_commands[i].name;
        cmd.func = console_dispatch;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    esp_console_cmd_t password_cmd = {};
    password_cmd.command = "password";
    password_cmd.help = "password reset: forget the page password";
    password_cmd.func = console_password;
    ESP_ERROR_CHECK(esp_console_cmd_register(&password_cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
