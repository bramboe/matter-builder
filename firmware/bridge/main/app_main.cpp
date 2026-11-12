// octo-matter bridge: the Octo beds as standard Matter devices.

#include <cstdlib>
#include <string>
#include <vector>

#include "bed_link.h"
#include "boot_guard.h"
#include "commands.h"
#include "config_mode.h"
#if CONFIG_OPENTHREAD_ENABLED
#include "thread_dataset.h"
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "http_api.h"
#include "matter_bridge.h"
#include "matter_info.h"
#include "mb_core.h"
#include "net.h"
#include "nvs_flash.h"
#include "octo_commands.h"
#include "program_access.h"
#include "secrets.h"

#if CONFIG_OPENTHREAD_ENABLED
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#ifndef OCTO_HTTP_TOKEN
#define OCTO_HTTP_TOKEN ""  // the password is chosen on the settings page
#endif
#ifndef OCTO_THREAD_DATASET
#define OCTO_THREAD_DATASET ""  // set on the settings page
#endif

// The settings page (octo_core/web/index.html, embedded at build time)
extern const char index_html_start[] asm("_binary_index_html_gz_start");
extern const char index_html_end[] asm("_binary_index_html_gz_end");

// A bridge generated from a spec (tools/mb_generate.py) has its name and the
// spec itself built in
#if __has_include("spec_defaults.h")
#include "spec_defaults.h"
extern const char spec_start[] asm("_binary_spec_yaml_start");
extern const char spec_end[] asm("_binary_spec_yaml_end");
#define HAS_SPEC 1
#endif

namespace {

std::string cmd_matter(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "open") {
        return matter_info::open_commissioning_window();
    }
    // A one-time code for another app, and ending it early
    if (args.size() == 2 && args[1] == "share") {
        return matter_info::share_window();
    }
    if (args.size() == 2 && args[1] == "close") {
        return matter_info::close_window();
    }
    if (args.size() == 2 && args[1] == "json") {
        return matter_info::matter_json();
    }
    if (args.size() == 2 && args[1] == "fabrics") {
        return matter_info::fabrics_text();
    }
    if (args.size() == 3 && args[1] == "remove") {
        return matter_info::remove_fabric(static_cast<unsigned>(std::strtoul(args[2].c_str(), nullptr, 10)));
    }
    return matter_bridge::describe();
}

// BOOT menu or six quick power-ons: forget the page password and open the setup access
// point, so the owner can choose a new one with the bridge in hand
void recovery() {
    http_api::reset_password();
    config_mode::request(15, true);
    esp_restart();
}

}  // namespace

extern "C" void app_main() {
    // Never erase NVS here: it holds the Matter fabrics and the bed settings.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("main", "NVS unavailable (%s)", esp_err_to_name(err));
    }

    mb_core::App app = {};
    app.vendor_name = "octo-matter";
    app.product_name = "Octo Bed Bridge";
    app.hostname = "octo-bridge";
#ifdef HAS_SPEC
    app.hostname = SPEC_SLUG;
    app.spec_start = spec_start;
    app.spec_end = spec_end;
#endif
    app.ap_prefix = "Octo-Bridge";
    app.page_gz_start = index_html_start;
    app.page_gz_end = index_html_end;
    app.http_token_fallback = OCTO_HTTP_TOKEN;
    app.thread_dataset_fallback = OCTO_THREAD_DATASET;
    app.stop = octo_commands::stop_beds;
    app.config_mode_start = bed_link::start;  // beds work without Matter
    app.state_json = octo_commands::state_json;
    mb_core::init(app);
    octo_commands::register_all();  // also in configuration mode

    boot_guard::early();
    program_access::start(recovery);  // the BOOT button, also in configuration mode
    // One boot on Wi-Fi for fast updates (Thread), or the setup access point
    if (config_mode::requested()) {
        config_mode::run();
    }
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
    octo_commands::matter_started();
    net::start();
    add_command("preset", "preset list | preset <0|1> <flat|1-3|match> | preset <0|1> save <1-3>\n",
                matter_bridge::preset_command);
    add_command("language", "language [en|nl|de|es]                  settings page and preset names\n",
                matter_bridge::language_command);
    add_command("wifi", "wifi [ssid <name>|password <pw>|clear|config [minutes]]  the Wi-Fi network\n",
                config_mode::command);
#if CONFIG_OPENTHREAD_ENABLED
    add_command("thread", "thread [dataset <hex>|clear|json]        Thread network, joined at the next start\n",
                thread_dataset::command);
#endif
    add_command("access", "access [json|revoke <n>]                programs with a token of their own\n",
                program_access::command);
    add_command("matter", "matter [open|fabrics|remove <n>|json]   pairing codes, endpoints; pairing window; controllers\n", cmd_matter);
    console_start();
    ESP_LOGI("main", "octo-matter bridge running; type 'help'");
}
