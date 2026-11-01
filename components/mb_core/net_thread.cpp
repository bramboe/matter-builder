// Network side of the bridge on a Thread chip (ESP32-C6). The Matter stack
// owns OpenThread; this joins the Thread network set on the settings page
// (thread_dataset.h) and starts the HTTP API once the bridge is attached.
// Matter is commissioned over Thread (on-network), not over BLE: BLE stays
// free for the beds. Without a Thread network the bridge restarts in
// configuration mode on Wi-Fi, so the page stays reachable.

#include "net.h"

#include <cstdio>
#include <cstring>

#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <lib/support/ThreadOperationalDataset.h>
#include <openthread/ip6.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>

#include "config_mode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_api.h"
#include "mb_core.h"
#include "thread_dataset.h"


namespace net {

namespace {

const char* TAG = "net";

bool attached = false;
int detaches = 0;

// Not attached by then: the network is wrong or away, so go to
// configuration mode, where the page can fix it
constexpr int64_t ATTACH_LIMIT_US = 3 * 60 * 1000000LL;
constexpr int FALLBACK_MINUTES = 15;

void to_configuration_mode(const char* why) {
    if (config_mode::request(FALLBACK_MINUTES)) {
        ESP_LOGW(TAG, "%s: restarting in configuration mode on Wi-Fi", why);
        esp_restart();
    }
    ESP_LOGE(TAG, "%s, and configuration mode could not be requested", why);
}

void on_attach_limit(void*) {
    if (!attached) {
        to_configuration_mode("Not attached to the Thread network after 3 minutes");
    }
}

void on_attached() {
    attached = true;
    ESP_LOGI(TAG, "Attached to the Thread network");
    http_api::start(mb_core::app().http_token_fallback);
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

// Runs in the Matter thread. Joins the network from the settings page (or
// the app) unless the bridge already has that dataset.
void provision(intptr_t) {
    uint8_t tlvs[chip::Thread::kSizeOperationalDataset];
    const char* source = "none";
    const size_t length = thread_dataset::wanted(tlvs, sizeof(tlvs), &source);
    auto& thread = chip::DeviceLayer::ThreadStackMgr();
    if (length == 0) {
        if (!thread.IsThreadProvisioned()) {
            to_configuration_mode("No Thread network set");
        } else if (thread.IsThreadAttached()) {
            on_attached();
        }
        return;
    }
    chip::Thread::OperationalDataset wanted;
    wanted.Init(chip::ByteSpan(tlvs, length));
    chip::Thread::OperationalDataset current;
    if (thread.IsThreadProvisioned() && thread.GetThreadProvision(current) == CHIP_NO_ERROR &&
        current.AsByteSpan().data_equal(wanted.AsByteSpan())) {
        if (thread.IsThreadAttached()) {
            on_attached();
        }
        return;
    }
    ESP_LOGI(TAG, "Joining the Thread network from %s", source);
    if (thread.AttachToThreadNetwork(wanted, nullptr) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not join the Thread network");
    }
}

}  // namespace

// Call after matter_bridge::start(): the Matter stack has set up OpenThread.
void start() {
    chip::DeviceLayer::PlatformMgr().AddEventHandler(on_device_event, 0);
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(provision, 0);
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_attach_limit;
    timer_args.name = "attach_limit";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, ATTACH_LIMIT_US);
    }
}

std::string describe() {
    std::string out = std::string("thread: ") + (attached ? "attached" : "not attached");
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        otInstance* instance = esp_openthread_get_instance();
        const char* role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));
        out += std::string(", role ") + role + ", detaches " + std::to_string(detaches) + "\n";
        // The border router publishes the Matter SRP host name on the LAN
        const otSrpClientHostInfo* host = otSrpClientGetHostInfo(instance);
        if (host != nullptr && host->mName != nullptr) {
            out += std::string("  settings page: http://") + host->mName + ".local/\n";
        }
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
