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
#include <openthread/link.h>
#include <openthread/netdata.h>
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
bool started = false;  // net::start() ran: OpenThread is up (not in configuration mode)
int detaches = 0;
int64_t last_detach_s = -1;   // uptime of the last detach
int64_t attached_since_s = -1;
// The parent (as a child), looked at every 10 s: how often it changed
otExtAddress parent_address = {};
bool have_parent = false;
int parent_changes = 0;
int64_t last_parent_change_s = -1;

int64_t uptime_s() { return esp_timer_get_time() / 1000000; }

void watch_parent(void*) {
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) return;
    otInstance* instance = esp_openthread_get_instance();
    otRouterInfo parent = {};
    if (otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_CHILD && otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
        if (have_parent && std::memcmp(&parent.mExtAddress, &parent_address, sizeof(parent_address)) != 0) {
            parent_changes++;
            last_parent_change_s = uptime_s();
        }
        parent_address = parent.mExtAddress;
        have_parent = true;
    }
    esp_openthread_lock_release();
}

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

// Waiting for a phone to pair it over Bluetooth, which also brings the network
bool waiting_for_ble_pairing() {
    const auto pairing = mb_core::app().ble_pairing;
    return pairing != nullptr && pairing() && !chip::DeviceLayer::ThreadStackMgr().IsThreadProvisioned();
}

void on_attach_limit(void*) {
    if (!attached && !waiting_for_ble_pairing()) {
        to_configuration_mode("Not attached to the Thread network after 3 minutes");
    }
}

void on_attached() {
    attached = true;
    attached_since_s = uptime_s();
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
        last_detach_s = uptime_s();
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
        if (!thread.IsThreadProvisioned() && waiting_for_ble_pairing()) {
            ESP_LOGW(TAG, "No Thread network yet: waiting to be paired over Bluetooth");
        } else if (!thread.IsThreadProvisioned()) {
            to_configuration_mode("No Thread network set");
        } else if (thread.IsThreadAttached()) {
            on_attached();
        }
        return;
    }
    chip::Thread::OperationalDataset wanted;
    if (wanted.Init(chip::ByteSpan(tlvs, length)) != CHIP_NO_ERROR) {
        return;  // thread_dataset::wanted() only returns valid datasets
    }
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
    started = true;
    chip::DeviceLayer::PlatformMgr().AddEventHandler(on_device_event, 0);
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(provision, 0);
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_attach_limit;
    timer_args.name = "attach_limit";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, ATTACH_LIMIT_US);
    }
    esp_timer_create_args_t watch_args = {};
    watch_args.callback = watch_parent;
    watch_args.name = "watch_parent";
    esp_timer_handle_t watch = nullptr;
    if (esp_timer_create(&watch_args, &watch) == ESP_OK) {
        esp_timer_start_periodic(watch, 10 * 1000000LL);
    }
}

bool up() { return attached; }

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

std::string health_json() {
    std::string out = "{\"kind\":\"thread\",\"attached\":" + std::string(attached ? "true" : "false") +
                      ",\"detaches\":" + std::to_string(detaches) +
                      ",\"last_detach\":" + std::to_string(last_detach_s) +
                      ",\"attached_since\":" + std::to_string(attached_since_s) +
                      ",\"parent_changes\":" + std::to_string(parent_changes) +
                      ",\"last_parent_change\":" + std::to_string(last_parent_change_s);
    if (!started || !esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        return out + "}";
    }
    otInstance* instance = esp_openthread_get_instance();
    const otDeviceRole role = otThreadGetDeviceRole(instance);
    out += std::string(",\"role\":\"") + otThreadDeviceRoleToString(role) + "\"";
    out += ",\"channel\":" + std::to_string(otLinkGetChannel(instance));
    // As a child: the signal to its parent (a router or the border router)
    otRouterInfo parent = {};
    if (role == OT_DEVICE_ROLE_CHILD && otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
        int8_t average = 0, last = 0;
        out += ",\"parent\":{\"lqi_in\":" + std::to_string(parent.mLinkQualityIn) +
               ",\"lqi_out\":" + std::to_string(parent.mLinkQualityOut) + ",\"age\":" + std::to_string(parent.mAge);
        if (otThreadGetParentAverageRssi(instance, &average) == OT_ERROR_NONE) out += ",\"rssi\":" + std::to_string(average);
        if (otThreadGetParentLastRssi(instance, &last) == OT_ERROR_NONE) out += ",\"rssi_last\":" + std::to_string(last);
        out += "}";
    }
    // As a router: how many neighbours it hears
    if (role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
        otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
        otNeighborInfo neighbor;
        int neighbors = 0;
        while (otThreadGetNextNeighborInfo(instance, &it, &neighbor) == OT_ERROR_NONE) neighbors++;
        out += ",\"neighbors\":" + std::to_string(neighbors);
    }
    // The way out of the mesh, from the network data the border routers
    // publish: an on-mesh prefix for addresses on the home network (OMR) and
    // a route out; each border router counted once
    uint16_t routers[8] = {};
    int router_count = 0;
    auto count_router = [&](uint16_t rloc16) {
        for (int i = 0; i < router_count; ++i) if (routers[i] == rloc16) return;
        if (router_count < 8) routers[router_count++] = rloc16;
    };
    bool omr = false, route = false;
    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig prefix;
    while (otNetDataGetNextOnMeshPrefix(instance, &it, &prefix) == OT_ERROR_NONE) {
        if (prefix.mOnMesh && prefix.mSlaac) { omr = true; count_router(prefix.mRloc16); }
        if (prefix.mDefaultRoute) { route = true; count_router(prefix.mRloc16); }
    }
    it = OT_NETWORK_DATA_ITERATOR_INIT;
    otExternalRouteConfig external;
    while (otNetDataGetNextRoute(instance, &it, &external) == OT_ERROR_NONE) {
        route = true;
        count_router(external.mRloc16);
    }
    out += std::string(",\"omr\":") + (omr ? "true" : "false") + ",\"route\":" + (route ? "true" : "false") +
           ",\"border_routers\":" + std::to_string(router_count);
    // Registered with the border router's SRP server: how Matter controllers find it
    const otSrpClientHostInfo* host = otSrpClientGetHostInfo(instance);
    out += std::string(",\"srp\":\"") +
           (otSrpClientIsRunning(instance) && host != nullptr ? otSrpClientItemStateToString(host->mState) : "Off") + "\"";
    // Radio counters since boot: retries and failures say how busy or bad the air is
    const otMacCounters* mac = otLinkGetCounters(instance);
    if (mac != nullptr) {
        out += ",\"tx\":" + std::to_string(mac->mTxTotal) + ",\"tx_retry\":" + std::to_string(mac->mTxRetry) +
               ",\"tx_fail\":" + std::to_string(mac->mTxDirectMaxRetryExpiry + mac->mTxIndirectMaxRetryExpiry) +
               ",\"tx_cca\":" + std::to_string(mac->mTxErrCca) + ",\"rx\":" + std::to_string(mac->mRxTotal);
    }
    esp_openthread_lock_release();
    return out + "}";
}

}  // namespace net
