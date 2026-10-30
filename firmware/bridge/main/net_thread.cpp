// Network side of the bridge on a Thread chip (ESP32-C6). The Matter stack
// owns OpenThread; this joins the Thread network whose operational dataset
// is in secrets.h and starts the HTTP API once the bridge is attached. Matter
// is commissioned over Thread (on-network), not over BLE: BLE stays free for
// the beds.

#include "net.h"

#include <cstdio>
#include <cstring>

#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <lib/support/ThreadOperationalDataset.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>

#include "esp_log.h"
#include "http_api.h"
#include "secrets.h"

#ifndef OCTO_THREAD_DATASET
#define OCTO_THREAD_DATASET ""  // reported at start: see secrets.example.h
#endif

namespace net {

namespace {

const char* TAG = "net";

bool attached = false;
int detaches = 0;

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns the number of bytes, or 0 when the text is not valid hex.
size_t parse_hex(const char* text, uint8_t* out, size_t max) {
    const size_t length = std::strlen(text);
    if (length == 0 || length % 2 != 0 || length / 2 > max) {
        return 0;
    }
    for (size_t i = 0; i < length / 2; ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        out[i] = static_cast<uint8_t>(high << 4 | low);
    }
    return length / 2;
}

void on_attached() {
    attached = true;
    ESP_LOGI(TAG, "Attached to the Thread network");
    http_api::start(OCTO_HTTP_TOKEN);
}

void on_device_event(const chip::DeviceLayer::ChipDeviceEvent* event, intptr_t) {
    if (event->Type != chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange) {
        return;
    }
    if (event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established) {
        on_attached();
    } else if (event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Lost) {
        attached = false;
        detaches++;
        ESP_LOGW(TAG, "Detached from the Thread network");
    }
}

// Runs in the Matter thread. Joins the network from secrets.h unless the
// bridge already has that dataset (also when a controller provisioned it).
void provision(intptr_t) {
    uint8_t tlvs[chip::Thread::kSizeOperationalDataset];
    const size_t length = parse_hex(OCTO_THREAD_DATASET, tlvs, sizeof(tlvs));
    chip::Thread::OperationalDataset wanted;
    if (length == 0 || wanted.Init(chip::ByteSpan(tlvs, length)) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OCTO_THREAD_DATASET in secrets.h is missing or not a valid dataset");
        return;
    }
    auto& thread = chip::DeviceLayer::ThreadStackMgr();
    chip::Thread::OperationalDataset current;
    if (thread.IsThreadProvisioned() && thread.GetThreadProvision(current) == CHIP_NO_ERROR &&
        current.AsByteSpan().data_equal(wanted.AsByteSpan())) {
        if (thread.IsThreadAttached()) {
            on_attached();
        }
        return;
    }
    ESP_LOGI(TAG, "Joining the Thread network from secrets.h");
    if (thread.AttachToThreadNetwork(wanted, nullptr) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not join the Thread network");
    }
}

}  // namespace

// Call after matter_bridge::start(): the Matter stack has set up OpenThread.
void start() {
    chip::DeviceLayer::PlatformMgr().AddEventHandler(on_device_event, 0);
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(provision, 0);
}

std::string describe() {
    std::string out = std::string("thread: ") + (attached ? "attached" : "not attached");
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        otInstance* instance = esp_openthread_get_instance();
        const char* role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));
        out += std::string(", role ") + role + ", detaches " + std::to_string(detaches) + "\n";
        // The routable addresses, to reach the settings page from the LAN
        for (const otNetifAddress* address = otIp6GetUnicastAddresses(instance); address != nullptr;
             address = address->mNext) {
            if (address->mAddress.mFields.m8[0] == 0xfe || address->mRloc) {
                continue;  // link-local or routing locator
            }
            char text[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&address->mAddress, text, sizeof(text));
            out += std::string("  ") + text + "\n";
        }
        esp_openthread_lock_release();
    } else {
        out += "\n";
    }
    return out;
}

}  // namespace net
