// octo-matter F0: BLE spike. Proves that the ESP32 can hold stable,
// authenticated links to both Octo beds before Matter is added on top.

#include "bed_link.h"
#include "esp_log.h"
#include "nvs_flash.h"

void console_start();

extern "C" void app_main() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bed_link::start();
    console_start();
    ESP_LOGI("main", "octo-matter F0 spike running; type 'help'");
}
