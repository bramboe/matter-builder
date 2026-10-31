// octo-matter bridge: the Octo beds as standard Matter devices.

#include <cstdlib>
#include <string>
#include <vector>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#if CONFIG_OPENTHREAD_ENABLED
#include "config_mode.h"
#endif
#include "esp_log.h"
#include "matter_bridge.h"
#include "net.h"
#include "nvs_flash.h"

#if CONFIG_OPENTHREAD_ENABLED
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

namespace {

std::string cmd_matter(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "open") {
        return matter_bridge::open_commissioning_window();
    }
    if (args.size() == 2 && args[1] == "json") {
        return matter_bridge::matter_json();
    }
    if (args.size() == 2 && args[1] == "fabrics") {
        return matter_bridge::fabrics_text();
    }
    if (args.size() == 3 && args[1] == "remove") {
        return matter_bridge::remove_fabric(static_cast<unsigned>(std::strtoul(args[2].c_str(), nullptr, 10)));
    }
    return matter_bridge::describe();
}

}  // namespace

extern "C" void app_main() {
    // Never erase NVS here: it holds the Matter fabrics and the bed settings.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("main", "NVS unavailable (%s)", esp_err_to_name(err));
    }

    boot_guard::early();
#if CONFIG_OPENTHREAD_ENABLED
    // One boot on Wi-Fi for fast updates, asked for on the settings page
    if (config_mode::requested()) {
        config_mode::run();
    }
#endif
    bed_link::start();
#if CONFIG_OPENTHREAD_ENABLED
    // OpenThread on the chip's own 802.15.4 radio, sharing it with BLE
    esp_openthread_platform_config_t thread_config = {};
    thread_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    thread_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    thread_config.port_config.storage_partition_name = "nvs";
    thread_config.port_config.netif_queue_size = 10;
    thread_config.port_config.task_queue_size = 10;
    set_openthread_platform_config(&thread_config);
#endif
    matter_bridge::start();
    net::start();
    add_command("preset", "preset list | preset <0|1> <flat|1-3|match> | preset <0|1> save <1-3>\n",
                matter_bridge::preset_command);
    add_command("language", "language [en|nl|de|es]                  settings page and preset names\n",
                matter_bridge::language_command);
#if CONFIG_OPENTHREAD_ENABLED
    add_command("wifi", "wifi [ssid <name>|password <pw>|clear|config [minutes]]  Wi-Fi for configuration mode\n",
                config_mode::command);
#endif
    add_command("matter", "matter [open|fabrics|remove <n>|json]   pairing codes, endpoints; pairing window; controllers\n", cmd_matter);
    console_start();
    ESP_LOGI("main", "octo-matter bridge running; type 'help'");
}
