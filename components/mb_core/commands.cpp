#include "commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "boot_guard.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mb_core.h"
#include "net.h"
#include "nvs.h"

namespace {

using Args = std::vector<std::string>;

const char* HELP =
    "system                                  firmware slots, reset reason, network\n"
    "revert                                  boot the other firmware slot\n"
    "reboot                                  restart\n";

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
    if (cmd == "system") return boot_guard::describe() + nvs_text() + net::describe();
    if (cmd == "revert") return cmd_revert();
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
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

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
