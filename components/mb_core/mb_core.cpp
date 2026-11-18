#include "mb_core.h"

#include <cstdio>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "log_buffer.h"
#include "mdns.h"
#include "sdkconfig.h"

namespace mb_core {

namespace {

App current = {"Matter Builder", "Matter Builder", "matter-builder", "Matter-Builder", nullptr, nullptr, nullptr, nullptr, "", "",
               nullptr, nullptr, nullptr, nullptr};

}  // namespace

void init(const App& app) {
    current = app;
    log_buffer::start();  // from here on the log is also kept for the add-on and the page
}

const App& app() { return current; }

void advertise_matter_builder(const char* host) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BASE);
    static char id[13];
    std::snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_app_desc_t* desc = esp_app_get_description();
    mdns_txt_item_t txt[] = {
        {"id", id},
        {"app", desc->project_name},
        {"version", desc->version},
#if CONFIG_OPENTHREAD_ENABLED
        {"network", "thread"},
#else
        {"network", "wifi"},
#endif
    };
    const size_t count = sizeof(txt) / sizeof(txt[0]);
    if (host != nullptr) {
        mdns_service_add_for_host(current.product_name, "_mbuilder", "_tcp", host, 80, txt, count);
    } else {
        mdns_service_add(current.product_name, "_mbuilder", "_tcp", 80, txt, count);
    }
}

void stop() {
    if (current.stop != nullptr) {
        current.stop();
    }
}

}  // namespace mb_core
