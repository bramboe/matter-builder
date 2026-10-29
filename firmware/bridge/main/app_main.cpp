// octo-matter bridge: the Octo beds as standard Matter devices.

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

std::string cmd_matter(const std::vector<std::string>&) {
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
    add_command("matter", "matter                                  pairing codes, fabrics, endpoints\n", cmd_matter);
    console_start();
    ESP_LOGI("main", "octo-matter bridge running; type 'help'");
}
