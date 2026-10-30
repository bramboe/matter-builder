// octo-matter bridge: the Octo beds as standard Matter devices.

#include <cstdlib>
#include <string>
#include <vector>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "esp_log.h"
#include "matter_bridge.h"
#include "net.h"
#include "nvs_flash.h"

namespace {

std::string cmd_matter(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "open") {
        return matter_bridge::open_commissioning_window();
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
    bed_link::start();
    matter_bridge::start();
    net::start();
    add_command("preset", "preset list | preset <0|1> <flat|1-3|match> | preset <0|1> save <1-3>\n",
                matter_bridge::preset_command);
    add_command("matter", "matter [open|fabrics|remove <n>]        pairing codes, endpoints; pairing window; controllers\n", cmd_matter);
    console_start();
    ESP_LOGI("main", "octo-matter bridge running; type 'help'");
}
