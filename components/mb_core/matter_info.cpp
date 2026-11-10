#include "matter_info.h"

#include <cinttypes>
#include <cstdio>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/reporting/reporting.h>
#include <nvs.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// Opening the window advertises the device again (on Thread: an SRP update
// at the border router), too deep for the HTTP task's stack: it crashed the
// bed bridge. Like removing a fabric, it runs on the Matter thread.
struct OpenRequest {
    CHIP_ERROR err;
    SemaphoreHandle_t done;
};

std::string open_commissioning_window() {
    OpenRequest request{CHIP_ERROR_INTERNAL, xSemaphoreCreateBinary()};
    if (request.done == nullptr) return "could not open the commissioning window: no memory\n";
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            auto* r = reinterpret_cast<OpenRequest*>(arg);
            auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
            r->err = manager.IsCommissioningWindowOpen()
                         ? CHIP_NO_ERROR
                         : manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300));
            xSemaphoreGive(r->done);
        },
        reinterpret_cast<intptr_t>(&request));
    if (err == CHIP_NO_ERROR) {
        xSemaphoreTake(request.done, portMAX_DELAY);  // the request lives on this stack
        err = request.err;
    }
    vSemaphoreDelete(request.done);
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

// Deleting a fabric runs every cleanup that listens to it (sessions, access
// control, subscriptions): too deep for the console or HTTP task's stack, so
// it runs on the Matter thread and this task waits for it
struct RemoveRequest {
    chip::FabricIndex index;
    CHIP_ERROR err;
    SemaphoreHandle_t done;
};

std::string remove_fabric(unsigned index) {
    RemoveRequest request{static_cast<chip::FabricIndex>(index), CHIP_ERROR_INTERNAL, xSemaphoreCreateBinary()};
    if (request.done == nullptr) return "could not remove fabric: no memory\n";
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            auto* r = reinterpret_cast<RemoveRequest*>(arg);
            r->err = chip::Server::GetInstance().GetFabricTable().Delete(r->index);
            xSemaphoreGive(r->done);
        },
        reinterpret_cast<intptr_t>(&request));
    if (err == CHIP_NO_ERROR) {
        // The request lives on this stack: wait for it, however long it takes
        xSemaphoreTake(request.done, portMAX_DELAY);
        err = request.err;
    }
    vSemaphoreDelete(request.done);
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
