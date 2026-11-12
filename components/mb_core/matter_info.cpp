#include "matter_info.h"

#include <cinttypes>
#include <cstdio>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/reporting/reporting.h>
#include <nvs.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "device_status.h"
#include "matter_thread.h"
#include "freertos/task.h"
#include "mb_core.h"
#include "net.h"
#include "sdkconfig.h"
#include "setup_code.h"

namespace matter_info {

namespace {

std::string hex16(unsigned value) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04X", value);
    return text;
}

std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    return out + "\"";
}

// Controllers by their Matter vendor ID
const char* vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x1349: return "Apple Home";
        case 0x134B: return "Home Assistant";
        case 0x6006: return "Google Home";
        case 0x1217: return "Amazon Alexa";
        case 0x110A: return "SmartThings";
        default: return nullptr;
    }
}

}  // namespace

const char* BUSY = "Matter is busy: try again in a moment\n";

// Opening the window advertises the device again (on Thread: an SRP update
// at the border router): it crashed the bed bridge on the HTTP task's stack
std::string open_commissioning_window() {
    CHIP_ERROR err = CHIP_ERROR_INTERNAL;
    const bool ran = matter_thread::run([&] {
        auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
        err = manager.IsCommissioningWindowOpen()
                  ? CHIP_NO_ERROR
                  : manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300));
    });
    if (!ran) return BUSY;
    return err == CHIP_NO_ERROR ? "commissioning window open for 5 minutes\n"
                                : std::string("could not open the commissioning window: ") + err.AsString() + "\n";
}

namespace {

// The one-time code of "matter share" while its window is open
struct Share {
    bool active = false;
    std::string qr, manual;
    uint32_t passcode = 0;       // of this one-time code only, never the device's own
    uint16_t discriminator = 0;  // the Home Assistant app needs both to hand it to another app
    int64_t until_us = 0;
} share;
constexpr uint32_t SHARE_SECONDS = 600;
constexpr uint32_t SHARE_ITERATIONS = 1000;  // PBKDF2 rounds of the verifier (Matter's minimum)

std::string share_json() {
    int size = 0;
    const std::string path = setup_code::qr_svg_path(share.qr.c_str(), size);
    const int64_t left = (share.until_us - esp_timer_get_time()) / 1000000;
    return "{\"manual\":" + json_text(share.manual) + ",\"qr\":" + json_text(share.qr) + ",\"qr_size\":" +
           std::to_string(size) + ",\"qr_path\":" + json_text(path) + ",\"left\":" + std::to_string(left > 0 ? left : 0) +
           ",\"passcode\":" + std::to_string(share.passcode) + ",\"discriminator\":" + std::to_string(share.discriminator) +
           ",\"seconds\":" + std::to_string(SHARE_SECONDS) + "}";
}

}  // namespace

std::string share_window() {
    // Only for a paired device: a new one pairs with its own code. The window
    // is opened in the name of the first controller (Administrator
    // Commissioning reports who opened it)
    size_t fabrics = 0;
    chip::FabricIndex opener = chip::kUndefinedFabricIndex;
    chip::VendorId opener_vendor = chip::VendorId::NotSpecified;
    bool open = false;
    if (!matter_thread::run([&] {
            chip::Server& server = chip::Server::GetInstance();
            for (const auto& fabric : server.GetFabricTable()) {
                if (fabrics++ == 0) {
                    opener = fabric.GetFabricIndex();
                    opener_vendor = fabric.GetVendorId();
                }
            }
            open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
        })) {
        return BUSY;
    }
    if (fabrics == 0) return "error: not paired yet: pair it with its own pairing code first\n";
    if (share.active && open && share.until_us > esp_timer_get_time()) return share_json() + "\n";

    // A new random passcode and discriminator, and a salt for the verifier
    uint32_t passcode = 0;
    do {
        passcode = 1 + esp_random() % chip::kSetupPINCodeMaximumValue;
    } while (!chip::SetupPayload::IsValidSetupPIN(passcode));
    const uint16_t discriminator = esp_random() & 0x0FFF;
    uint8_t salt[32];
    esp_fill_random(salt, sizeof(salt));
    chip::Crypto::Spake2pVerifier verifier;
    if (verifier.Generate(SHARE_ITERATIONS, chip::ByteSpan(salt), passcode) != CHIP_NO_ERROR) {
        return "error: could not make a code\n";
    }
    std::string qr, manual;
    if (!setup_code::codes_for(passcode, discriminator, qr, manual)) return "error: could not make a code\n";

    CHIP_ERROR err = CHIP_ERROR_INTERNAL;
    if (!matter_thread::run([&] {
            auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (manager.IsCommissioningWindowOpen()) manager.CloseCommissioningWindow();  // the code of its own ("matter open")
            err = manager.OpenEnhancedCommissioningWindow(chip::System::Clock::Seconds32(SHARE_SECONDS), discriminator,
                                                          verifier, SHARE_ITERATIONS, chip::ByteSpan(salt), opener,
                                                          opener_vendor);
        })) {
        return BUSY;
    }
    if (err != CHIP_NO_ERROR) return std::string("error: could not open the commissioning window: ") + err.AsString() + "\n";
    share.active = true;
    share.qr = qr;
    share.manual = manual;
    share.passcode = passcode;
    share.discriminator = discriminator;
    share.until_us = esp_timer_get_time() + int64_t(SHARE_SECONDS) * 1000000;
    return share_json() + "\n";
}

std::string close_window() {
    share.active = false;
    if (!matter_thread::run([] { chip::Server::GetInstance().GetCommissioningWindowManager().CloseCommissioningWindow(); })) {
        return BUSY;
    }
    return "commissioning window closed\n";
}

std::string fabrics_text() {
    std::string out;
    const bool ran = matter_thread::run([&] {
        for (const auto& fabric : chip::Server::GetInstance().GetFabricTable()) {
            const chip::CharSpan label = fabric.GetFabricLabel();
            char line[96];
            std::snprintf(line, sizeof(line), "fabric %u: vendor 0x%04X, fabric id 0x%" PRIX64 ", label '%.*s'\n",
                          fabric.GetFabricIndex(), chip::to_underlying(fabric.GetVendorId()), fabric.GetFabricId(),
                          static_cast<int>(label.size()), label.data());
            out += line;
        }
    });
    if (!ran) return BUSY;
    return out.empty() ? "no fabrics\n" : out;
}

// Deleting a fabric runs every cleanup that listens to it (sessions, access
// control, subscriptions)
std::string remove_fabric(unsigned index) {
    CHIP_ERROR err = CHIP_ERROR_INTERNAL;
    const bool ran = matter_thread::run([&] {
        err = chip::Server::GetInstance().GetFabricTable().Delete(static_cast<chip::FabricIndex>(index));
    }, 60000);
    if (!ran) return BUSY;
    return err == CHIP_NO_ERROR ? "fabric " + std::to_string(index) + " removed\n"
                                : std::string("could not remove fabric: ") + err.AsString() + "\n";
}

std::string remove_all_fabrics() {
    if (xTaskGetHandle(CHIP_DEVICE_CONFIG_CHIP_TASK_NAME) == nullptr) return "Matter is not running: nothing unpaired\n";
    int removed = 0;
    const bool ran = matter_thread::run([&] {
        auto& table = chip::Server::GetInstance().GetFabricTable();
        chip::FabricIndex indexes[CHIP_CONFIG_MAX_FABRICS];
        int count = 0;
        for (const auto& fabric : table) {
            if (count < CHIP_CONFIG_MAX_FABRICS) indexes[count++] = fabric.GetFabricIndex();
        }
        for (int i = 0; i < count; ++i) {
            if (table.Delete(indexes[i]) == CHIP_NO_ERROR) ++removed;
        }
    }, 60000);
    if (!ran) return BUSY;
    return "unpaired from " + std::to_string(removed) + " controller" + (removed == 1 ? "" : "s") + "\n";
}

std::string matter_json() {
    const mb_core::App& app = mb_core::app();
    char qr[64] = {};
    char manual[32] = {};
    chip::MutableCharSpan qr_span(qr);
    chip::MutableCharSpan manual_span(manual);
    const auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork);
    std::string fabrics;

    bool have_qr = false, have_manual = false, window_open = false;
    matter_thread::run([&] {
        have_qr = GetQRCode(qr_span, flags) == CHIP_NO_ERROR;
        have_manual = GetManualPairingCode(manual_span, flags) == CHIP_NO_ERROR;
        chip::Server& server = chip::Server::GetInstance();
        window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
        for (const auto& fabric : server.GetFabricTable()) {
            const uint16_t vendor_id = chip::to_underlying(fabric.GetVendorId());
            const char* vendor = vendor_name(vendor_id);
            char hex[8];
            std::snprintf(hex, sizeof(hex), "0x%04X", vendor_id);
            const chip::CharSpan label = fabric.GetFabricLabel();
            fabrics += std::string(fabrics.empty() ? "" : ",") + "{\"index\":" + std::to_string(fabric.GetFabricIndex()) +
                       ",\"vendor\":" + json_text(vendor != nullptr ? vendor : hex) + ",\"vendor_id\":" + json_text(hex) +
                       ",\"label\":" + json_text(std::string(label.data(), label.size()));
            // Connected now (it holds a subscription), since when, or when last
            const device_status::Controller c = device_status::controller(fabric.GetFabricIndex());
            fabrics += std::string(",\"connected\":") + (c.connected ? "true" : "false") + ",\"since\":" + std::to_string(c.since_s) +
                       ",\"last_seen\":" + std::to_string(c.last_seen_s) + "}";
        }
    });

    int qr_size = 0;
    const std::string path = have_qr ? setup_code::qr_svg_path(qr, qr_size) : "";

    std::string out = "{\"product\":" + json_text(app.product_name) + ",\"vendor\":" + json_text(app.vendor_name) +
                      ",\"version\":" + json_text(esp_app_get_description()->version) +
                      ",\"vid\":\"" + hex16(CONFIG_DEVICE_VENDOR_ID) + "\",\"pid\":\"" + hex16(CONFIG_DEVICE_PRODUCT_ID) + "\"";
    out += ",\"qr\":" + json_text(have_qr ? qr : "") + ",\"manual\":" + json_text(have_manual ? manual : "");
    out += ",\"qr_size\":" + std::to_string(qr_size) + ",\"qr_path\":" + json_text(path);
    out += std::string(",\"window_open\":") + (window_open ? "true" : "false");
    // The one-time code while its window is open (gone once a controller paired or it ran out)
    if (share.active && (!window_open || share.until_us <= esp_timer_get_time())) share.active = false;
    out += ",\"share\":" + (share.active ? share_json() : std::string("null"));
    out += ",\"fabrics\":[" + fabrics + "],\"network\":" + json_text(net::describe()) + "}\n";
    return out;
}

void note_structure(uint32_t signature) {
    nvs_handle_t nvs;
    if (nvs_open("mb_core", NVS_READWRITE, &nvs) != ESP_OK) return;
    uint32_t before = 0;
    const bool known = nvs_get_u32(nvs, "structure", &before) == ESP_OK;
    if (!known || before != signature) {
        uint32_t version = 1;
        auto& config = chip::DeviceLayer::ConfigurationMgr();
        if (config.GetConfigurationVersion(version) != CHIP_NO_ERROR || version == 0) version = 1;
        // The first time only records it: nothing changed for controllers yet
        if (known && config.StoreConfigurationVersion(version + 1) == CHIP_NO_ERROR) {
            MatterReportingAttributeChangeCallback(0, chip::app::Clusters::BasicInformation::Id,
                                                   chip::app::Clusters::BasicInformation::Attributes::ConfigurationVersion::Id);
            std::printf("structure changed: configuration version %" PRIu32 "\n", version + 1);
        }
        nvs_set_u32(nvs, "structure", signature);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

}  // namespace matter_info
