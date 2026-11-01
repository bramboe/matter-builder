#include "matter_info.h"

#include <cinttypes>
#include <cstdio>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "esp_app_desc.h"
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

std::string open_commissioning_window() {
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
    const CHIP_ERROR err = manager.IsCommissioningWindowOpen()
                               ? CHIP_NO_ERROR
                               : manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300));
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return err == CHIP_NO_ERROR ? "commissioning window open for 5 minutes\n"
                                : std::string("could not open the commissioning window: ") + err.AsString() + "\n";
}

std::string fabrics_text() {
    std::string out;
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    for (const auto& fabric : chip::Server::GetInstance().GetFabricTable()) {
        const chip::CharSpan label = fabric.GetFabricLabel();
        char line[96];
        std::snprintf(line, sizeof(line), "fabric %u: vendor 0x%04X, fabric id 0x%" PRIX64 ", label '%.*s'\n",
                      fabric.GetFabricIndex(), chip::to_underlying(fabric.GetVendorId()), fabric.GetFabricId(),
                      static_cast<int>(label.size()), label.data());
        out += line;
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return out.empty() ? "no fabrics\n" : out;
}

std::string remove_fabric(unsigned index) {
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const CHIP_ERROR err = chip::Server::GetInstance().GetFabricTable().Delete(static_cast<chip::FabricIndex>(index));
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return err == CHIP_NO_ERROR ? "fabric " + std::to_string(index) + " removed\n"
                                : std::string("could not remove fabric: ") + err.AsString() + "\n";
}

std::string matter_json() {
    const mb_core::App& app = mb_core::app();
    char qr[64] = {};
    char manual[32] = {};
    chip::MutableCharSpan qr_span(qr);
    chip::MutableCharSpan manual_span(manual);
    const auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork);
    std::string fabrics;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const bool have_qr = GetQRCode(qr_span, flags) == CHIP_NO_ERROR;
    const bool have_manual = GetManualPairingCode(manual_span, flags) == CHIP_NO_ERROR;
    chip::Server& server = chip::Server::GetInstance();
    const bool window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
    for (const auto& fabric : server.GetFabricTable()) {
        const uint16_t vendor_id = chip::to_underlying(fabric.GetVendorId());
        const char* vendor = vendor_name(vendor_id);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%04X", vendor_id);
        const chip::CharSpan label = fabric.GetFabricLabel();
        fabrics += std::string(fabrics.empty() ? "" : ",") + "{\"index\":" + std::to_string(fabric.GetFabricIndex()) +
                   ",\"vendor\":" + json_text(vendor != nullptr ? vendor : hex) + ",\"vendor_id\":" + json_text(hex) +
                   ",\"label\":" + json_text(std::string(label.data(), label.size())) + "}";
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    int qr_size = 0;
    const std::string path = have_qr ? setup_code::qr_svg_path(qr, qr_size) : "";

    std::string out = "{\"product\":" + json_text(app.product_name) + ",\"vendor\":" + json_text(app.vendor_name) +
                      ",\"version\":" + json_text(esp_app_get_description()->version) +
                      ",\"vid\":\"" + hex16(CONFIG_DEVICE_VENDOR_ID) + "\",\"pid\":\"" + hex16(CONFIG_DEVICE_PRODUCT_ID) + "\"";
    out += ",\"qr\":" + json_text(have_qr ? qr : "") + ",\"manual\":" + json_text(have_manual ? manual : "");
    out += ",\"qr_size\":" + std::to_string(qr_size) + ",\"qr_path\":" + json_text(path);
    out += std::string(",\"window_open\":") + (window_open ? "true" : "false");
    out += ",\"fabrics\":[" + fabrics + "],\"network\":" + json_text(net::describe()) + "}\n";
    return out;
}

}  // namespace matter_info
