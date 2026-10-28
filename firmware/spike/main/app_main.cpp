// octo-matter F0: BLE spike. Proves that the ESP32 can hold stable,
// authenticated links to both Octo beds before Matter is added on top.

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "esp_log.h"
#include "net.h"
#include "nvs_flash.h"

extern "C" void app_main() {
    // Never erase NVS here: it also holds ESPHome's data, which must survive
    // for a return to the ESPHome slot. Without NVS the beds just cannot be
    // stored; the boot guard still works.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("main", "NVS unavailable (%s); bed settings will not persist", esp_err_to_name(err));
    }

    boot_guard::early();
    net::start();
    bed_link::start();
    console_start();
    ESP_LOGI("main", "octo-matter F0 spike running; type 'help'");
}
