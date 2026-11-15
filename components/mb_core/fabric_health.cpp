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

// The controller's stored subscriptions (that the device resumes after a
// restart), dropped when there are any
void drop_resumption(chip::FabricIndex index, uint32_t offline_s) {
#if CHIP_CONFIG_PERSIST_SUBSCRIPTIONS
    auto* storage = chip::Server::GetInstance().GetSubscriptionResumptionStorage();
    if (storage == nullptr) return;
    auto* iterator = storage->IterateSubscriptions();
    if (iterator == nullptr) return;  // busy resuming: next time
    chip::app::SubscriptionResumptionStorage::SubscriptionInfo subscription;
    bool any = false;
    while (iterator->Next(subscription)) {
        if (subscription.mFabricIndex == index) {
            any = true;
            break;
        }
    }
    iterator->Release();
    if (!any) return;
    const CHIP_ERROR err = storage->DeleteAll(index);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Controller %u: no contact for %" PRIu32 " h, its stored subscription is dropped (no more attempts to reach it)",
                 static_cast<unsigned>(index), offline_s / 3600);
    } else {
        ESP_LOGE(TAG, "Controller %u: could not drop its stored subscription: %" CHIP_ERROR_FORMAT, static_cast<unsigned>(index), err.Format());
    }
#else
    (void) index;
    (void) offline_s;
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
    chip::FabricIndex drop[MAX_FABRICS];
    uint32_t drop_offline[MAX_FABRICS];
    int drops = 0;
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
            if (step.drop_resumption && drops < MAX_FABRICS) {
                drop[drops] = fabric.GetFabricIndex();
                drop_offline[drops++] = slot.record.offline_s;
            }
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
    for (int i = 0; i < drops; ++i) drop_resumption(drop[i], drop_offline[i]);
}

Info info(unsigned fabric_index) {
    std::lock_guard<std::mutex> guard(lock);
    if (!loaded || fabric_index == 0 || fabric_index > MAX_FABRICS || !slots[fabric_index].present) return {-1, false};
    const uint32_t offline = slots[fabric_index].record.offline_s;
    return {static_cast<long>(offline), offline >= STALE_S};
}

}  // namespace fabric_health
