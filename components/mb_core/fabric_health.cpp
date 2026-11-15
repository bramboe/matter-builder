#include "fabric_health.h"

#include <cinttypes>
#include <cstdio>
#include <mutex>

#include <app/InteractionModelEngine.h>
#include <app/SubscriptionResumptionStorage.h>
#include <app/server/Server.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "fabric_health_logic.h"
#include "net.h"
#include "nvs.h"

namespace fabric_health {

namespace {

const char* TAG = "fabric_health";
const char* NVS_NAMESPACE = "mb_fabric";
constexpr int MAX_FABRICS = 16;
constexpr int64_t EVERY_US = 60LL * 1000000;

struct Stored {
    uint64_t id;
    uint32_t offline_s;
};

struct Slot {
    bool present = false;  // a fabric (or a stored record) with this index
    Record record;
};

std::mutex lock;
Slot slots[MAX_FABRICS + 1];
bool loaded = false;
int64_t last_us = 0;

void key_for(unsigned index, char (&key)[12]) { std::snprintf(key, sizeof(key), "f%u", index); }

void load() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;  // nothing stored yet
    for (unsigned i = 1; i <= MAX_FABRICS; ++i) {
        char key[12];
        key_for(i, key);
        Stored stored = {};
        size_t size = sizeof(stored);
        if (nvs_get_blob(nvs, key, &stored, &size) == ESP_OK && size == sizeof(stored)) {
            slots[i].present = true;
            slots[i].record.id = stored.id;
            slots[i].record.offline_s = stored.offline_s;
        }
    }
    nvs_close(nvs);
}

void save(unsigned index, const Record& r) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[12];
    key_for(index, key);
    const Stored stored = {r.id, r.offline_s};
    nvs_set_blob(nvs, key, &stored, sizeof(stored));
    nvs_commit(nvs);
    nvs_close(nvs);
}

void forget(unsigned index) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[12];
    key_for(index, key);
    nvs_erase_key(nvs, key);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Stored subscriptions whose node has no session: since when (uptime), by
// subscription. Kept in RAM only: the Matter stack keeps the failed attempts
// across restarts itself.
struct Orphan {
    bool used = false;
    chip::FabricIndex fabric = 0;
    chip::NodeId node = 0;
    chip::SubscriptionId id = 0;
    int64_t since_us = 0;
    bool seen = false;
};
constexpr int MAX_ORPHANS = 16;
Orphan orphans[MAX_ORPHANS];

Orphan* orphan_for(chip::FabricIndex fabric, chip::NodeId node, chip::SubscriptionId id, int64_t now) {
    Orphan* free_slot = nullptr;
    for (Orphan& o : orphans) {
        if (o.used && o.fabric == fabric && o.node == node && o.id == id) return &o;
        if (!o.used && free_slot == nullptr) free_slot = &o;
    }
    if (free_slot == nullptr) return nullptr;
    *free_slot = Orphan();
    free_slot->used = true;
    free_slot->fabric = fabric;
    free_slot->node = node;
    free_slot->id = id;
    free_slot->since_us = now;
    return free_slot;
}

// Has the node a CASE session with the device (it holds or makes a
// subscription, or talks to it in any other way)?
bool reachable(chip::FabricIndex fabric, chip::NodeId node) {
    auto session = chip::Server::GetInstance().GetSecureSessionManager().FindSecureSessionForNode(
        chip::ScopedNodeId(node, fabric), chip::MakeOptional(chip::Transport::SecureSession::Type::kCASE));
    return session.HasValue();
}

// Drops the stored subscriptions of nodes that are gone (drop_orphan)
void prune_subscriptions(int64_t now, bool counting, uint32_t elapsed_s) {
#if CHIP_CONFIG_PERSIST_SUBSCRIPTIONS
    // Off the network itself: nobody could reach the device, so that time
    // does not count, and nothing is dropped
    if (!counting) {
        for (Orphan& o : orphans) {
            if (o.used) o.since_us += int64_t(elapsed_s) * 1000000;
        }
        return;
    }
    auto* storage = chip::Server::GetInstance().GetSubscriptionResumptionStorage();
    if (storage == nullptr) return;
    auto* iterator = storage->IterateSubscriptions();
    if (iterator == nullptr) return;  // busy resuming: next time
    struct Drop {
        chip::FabricIndex fabric;
        chip::NodeId node;
        chip::SubscriptionId id;
        uint32_t failed;
        uint32_t orphan_s;
    };
    Drop drops[MAX_ORPHANS];
    int count = 0;
    for (Orphan& o : orphans) o.seen = false;
    chip::app::SubscriptionResumptionStorage::SubscriptionInfo subscription;
    while (iterator->Next(subscription)) {
        if (reachable(subscription.mFabricIndex, subscription.mNodeId)) continue;
        Orphan* o = orphan_for(subscription.mFabricIndex, subscription.mNodeId, subscription.mSubscriptionId, now);
        if (o == nullptr) continue;
        o->seen = true;
        uint32_t failed = 0;
#if CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION
        failed = subscription.mResumptionRetries;
#endif
        const uint32_t orphan_s = static_cast<uint32_t>((now - o->since_us) / 1000000);
        if (drop_orphan(false, orphan_s, failed) && count < MAX_ORPHANS) {
            drops[count++] = {subscription.mFabricIndex, subscription.mNodeId, subscription.mSubscriptionId, failed, orphan_s};
        }
    }
    iterator->Release();
    // Reachable again, or gone from the storage: forget it
    for (Orphan& o : orphans) {
        if (o.used && !o.seen) o = Orphan();
    }
    for (int i = 0; i < count; ++i) {
        const Drop& d = drops[i];
        const CHIP_ERROR err = storage->Delete(d.node, d.fabric, d.id);
        if (err == CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Controller %u, node %08" PRIX32 "%08" PRIX32 ": unreachable (%" PRIu32 " failed attempts, %" PRIu32
                     " min without a session), its stored subscription is dropped: no more attempts to reach it",
                     static_cast<unsigned>(d.fabric), static_cast<uint32_t>(d.node >> 32), static_cast<uint32_t>(d.node), d.failed,
                     d.orphan_s / 60);
        } else {
            ESP_LOGE(TAG, "Controller %u: could not drop a stored subscription: %" CHIP_ERROR_FORMAT,
                     static_cast<unsigned>(d.fabric), err.Format());
        }
        for (Orphan& o : orphans) {
            if (o.used && o.fabric == d.fabric && o.node == d.node && o.id == d.id) o = Orphan();
        }
    }
#else
    (void) now;
    (void) counting;
    (void) elapsed_s;
#endif
}

}  // namespace

void poll() {
    const int64_t now = esp_timer_get_time();
    if (loaded && now - last_us < EVERY_US) return;
    const uint32_t elapsed = loaded ? static_cast<uint32_t>((now - last_us) / 1000000) : 0;
    last_us = now;

    // Not paired (or the fabric table not there yet): nothing to look at, and
    // stored records are kept rather than taken for removed controllers
    auto& table = chip::Server::GetInstance().GetFabricTable();
    if (table.FabricCount() == 0) return;
    auto* engine = chip::app::InteractionModelEngine::GetInstance();
    const bool counting = net::up();
    bool seen[MAX_FABRICS + 1] = {};
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!loaded) {
            load();
            loaded = true;
        }
        for (const auto& fabric : table) {
            const unsigned index = fabric.GetFabricIndex();
            if (index == 0 || index > MAX_FABRICS) continue;
            seen[index] = true;
            const bool live = engine != nullptr &&
                engine->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe, fabric.GetFabricIndex()) > 0;
            Slot& slot = slots[index];
            slot.present = true;
            const Step step = advance(slot.record, fabric.GetCompressedFabricId(), live, counting, elapsed);
            if (step.save) save(index, slot.record);
            if (step.note_stale) {
                ESP_LOGW(TAG, "Controller %u (vendor 0x%04X): no contact for %" PRIu32 " days. Probably removed in its app; "
                         "disconnect it on the settings page or with \"matter remove %u\" if it is no longer used",
                         index, chip::to_underlying(fabric.GetVendorId()), slot.record.offline_s / 86400, index);
            }
        }
        // Fabrics that are gone (unpaired): their record too
        for (unsigned i = 1; i <= MAX_FABRICS; ++i) {
            if (slots[i].present && !seen[i]) {
                slots[i] = Slot();
                forget(i);
            }
        }
    }
    prune_subscriptions(now, counting, elapsed);
}

Info info(unsigned fabric_index) {
    std::lock_guard<std::mutex> guard(lock);
    if (!loaded || fabric_index == 0 || fabric_index > MAX_FABRICS || !slots[fabric_index].present) return {-1, false};
    const uint32_t offline = slots[fabric_index].record.offline_s;
    return {static_cast<long>(offline), offline >= STALE_S};
}

}  // namespace fabric_health
