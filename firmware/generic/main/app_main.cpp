// Matter Builder generic firmware: what is connected (relay, LED, button,
// contact, motion sensor, SHT4x) is set on the settings page, not built in.

#include <cstdlib>
#include <string>
#include <vector>

#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#if CONFIG_ENABLE_CHIPOBLE
#include <platform/internal/BLEManager.h>
#endif

#include "boot_guard.h"
#include "commands.h"
#include "config_mode.h"
#include "esp_log.h"
#include "esp_system.h"
#include "http_api.h"
#include "io.h"
#include "io.h"
#include "matter_device.h"
#include "matter_info.h"
#include "mb_core.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "program_access.h"
#include "thread_dataset.h"

// The settings page (web/index.html, embedded at build time)
extern const char page_start[] asm("_binary_generic_html_gz_start");
extern const char page_end[] asm("_binary_generic_html_gz_end");

// A device generated from a spec (tools/mb_generate.py) has its name,
// factory connections and the spec itself built in
#if __has_include("spec_defaults.h")
#include <cstring>

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
    if (args.size() == 2 && args[1] == "fabrics") {
        return matter_info::fabrics_text();
    }
    if (args.size() == 2 && args[1] == "endpoints") {
        return matter_device::endpoints_text();
    }
    if (args.size() == 3 && args[1] == "remove") {
        return matter_info::remove_fabric(static_cast<unsigned>(std::strtoul(args[2].c_str(), nullptr, 10)));
    }
    return matter_info::matter_json();
}

std::string cmd_json(const std::vector<std::string>&) { return io::state_json(); }

// Pairing over Bluetooth: a phone pairs the device directly, without the
// setup network, and gives it the Thread network. Off by default: Bluetooth
// then stays free (and only runs for connections that listen to it), which
// leaves about 30 KB more memory and the whole radio to Thread.
bool ble_pairing_enabled() {
#if CONFIG_ENABLE_CHIPOBLE
    uint8_t value = 0;
    nvs_handle_t nvs;
    if (nvs_open("io", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "blepair", &value);
        nvs_close(nvs);
    }
    return value != 0;
#else
    return false;
#endif
}

std::string cmd_pairing(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[1] == "ble" && (args[2] == "on" || args[2] == "off")) {
#if CONFIG_ENABLE_CHIPOBLE
        nvs_handle_t nvs;
        if (nvs_open("io", NVS_READWRITE, &nvs) != ESP_OK) return "storing failed\n";
        nvs_set_u8(nvs, "blepair", args[2] == "on" ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        return "ok, after the next restart\n";
#else
        return "this firmware has no pairing over Bluetooth\n";
#endif
    }
    return std::string("pairing over Bluetooth: ") + (ble_pairing_enabled() ? "on" : "off") + "\n";
}

#ifdef HAS_SPEC
void load_spec_factory() {
    io::Slot slots[io::SLOTS] = {};
    for (int i = 0; i < SPEC_CONNECTION_COUNT; ++i) {
        const SpecConnection& c = SPEC_CONNECTIONS[i];
        if (c.slot < 0 || c.slot >= io::SLOTS) continue;
        io::Slot& s = slots[c.slot];
        s.type = io::type_named(c.type);  // built-in or from a component (registered first)
        if (c.code[0]) io::parse_code(c.code, s.data);
        if (c.mac[0]) io::parse_mac(c.mac, s.data);
        if (!c.code[0] && !c.mac[0]) {
            s.data[0] = c.number & 0xFF;
            s.data[1] = (c.number >> 8) & 0xFF;
        }
        s.pin = static_cast<uint8_t>(c.pin);
        s.pin2 = static_cast<uint8_t>(c.scl);
        s.flags = c.inverted ? io::INVERTED : 0;
        std::strncpy(s.name, c.name, sizeof(s.name) - 1);
    }
    io::set_factory(slots, SPEC_NAME);
}
#endif

// BOOT held 5-10 s: forget the page password and open the setup access
// point, so the owner can choose a new one with the device in hand
void recovery() {
    http_api::reset_password();
    config_mode::request(15, true);
    esp_restart();
}

}  // namespace

extern "C" void app_main() {
    // Never erase NVS here: it holds the Matter fabrics and the connections
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("main", "NVS unavailable (%s)", esp_err_to_name(err));
    }

    io::register_components();  // component types, before the connections or the spec load

    mb_core::App app = {};
    app.vendor_name = "Matter Builder";
    app.product_name = "Matter Builder device";
    app.hostname = "matter-builder";
#ifdef HAS_SPEC
    app.hostname = SPEC_SLUG;
    app.spec_start = spec_start;
    app.spec_end = spec_end;
    load_spec_factory();
#endif
    app.ap_prefix = "Matter-Builder";
    app.page_gz_start = page_start;
    app.page_gz_end = page_end;
    app.http_token_fallback = "";
    app.thread_dataset_fallback = "";
    app.stop = io::stop_outputs;
    app.config_mode_start = io::start;  // the connections work without Matter
    app.state_json = io::state_json;
    app.ble_pairing = ble_pairing_enabled;
    mb_core::init(app);
    // Also in configuration mode
    add_command("io", "io [json|set|clear|test|apply|confirm|cancel]  connections\n", io::command);
    add_command("name", "name [<text>]                           the device's name\n", io::name_command);
    add_command("json", "json                                    state for the settings page\n", cmd_json);

    boot_guard::early();
    program_access::start(recovery);  // the BOOT button, also in configuration mode
    // One boot on Wi-Fi for fast updates and setup, asked for on the page
    if (config_mode::requested()) {
        config_mode::run();
    }
    io::start();

    esp_openthread_platform_config_t thread_config = {};
    thread_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    thread_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    thread_config.port_config.storage_partition_name = "nvs";
    thread_config.port_config.netif_queue_size = 10;
    thread_config.port_config.task_queue_size = 10;
    set_openthread_platform_config(&thread_config);

    matter_device::start();
    // Matter starts its Bluetooth layer; without pairing over Bluetooth it lets
    // go of it again, and the connections that listen start their own
    const bool pairing = ble_pairing_enabled();
    io::set_ble_pairing(pairing);
#if CONFIG_ENABLE_CHIPOBLE
    if (!pairing) {
        (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(
            [](intptr_t) { chip::DeviceLayer::Internal::BLEMgr().Shutdown(); });
    }
#endif
    io::start_bluetooth(pairing);
    net::start();
    add_command("wifi", "wifi [ssid <name>|password <pw>|clear|config [minutes]]  Wi-Fi for configuration mode\n",
                config_mode::command);
    add_command("thread", "thread [dataset <hex>|clear|json]        Thread network, joined at the next start\n",
                thread_dataset::command);
    add_command("access", "access [json|revoke <n>]                programs with a token of their own\n",
                program_access::command);
    add_command("pairing", "pairing [ble on|off]                    pairing over Bluetooth (after a restart)\n",
                cmd_pairing);
    add_command("matter", "matter [open|fabrics|remove <n>|json]   pairing codes, pairing window, controllers\n",
                cmd_matter);
    console_start();
    ESP_LOGI("main", "Matter Builder device running; type 'help'");
}
