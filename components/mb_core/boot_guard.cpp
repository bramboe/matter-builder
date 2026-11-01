#include "boot_guard.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

namespace boot_guard {

namespace {

const char* TAG = "guard";
constexpr uint32_t RTC_MAGIC = 0x0C70B007;
constexpr int MAX_QUICK_CRASHES = 3;
constexpr uint64_t STABLE_UPTIME_US = 60ULL * 1000000;
constexpr uint64_t NETWORK_DEADLINE_US = 10ULL * 60 * 1000000;

// Survives software resets and crashes, not power loss
RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_quick_boots;

bool confirmed = false;
esp_timer_handle_t stable_timer = nullptr;
esp_timer_handle_t deadline_timer = nullptr;

std::string image_id() {
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    return sha;
}

bool load_confirmed() {
    nvs_handle_t nvs;
    if (nvs_open("octo", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    char stored[17] = {};
    size_t len = sizeof(stored);
    bool match = nvs_get_str(nvs, "confirmed", stored, &len) == ESP_OK && image_id() == stored;
    nvs_close(nvs);
    return match;
}

void store_confirmed() {
    nvs_handle_t nvs;
    if (nvs_open("octo", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "confirmed", image_id().c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
}

void revert(const char* why) {
    std::string info;
    ESP_LOGE(TAG, "Unconfirmed firmware: %s", why);
    if (select_other_slot(info)) {
        ESP_LOGE(TAG, "Reverting to the other slot (%s)", info.c_str());
        esp_restart();
    }
    ESP_LOGE(TAG, "Cannot revert: %s", info.c_str());
}

void on_stable(void*) { rtc_quick_boots = 0; }

void on_deadline(void*) {
    if (!confirmed) {
        revert("no network 10 minutes after boot");
    }
}

std::string describe_partition(const esp_partition_t* part) {
    if (part == nullptr) {
        return "none";
    }
    char buf[128];
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        std::snprintf(buf, sizeof(buf), "%s @0x%" PRIx32 ": %s %s (%s %s)", part->label, part->address,
                      desc.project_name, desc.version, desc.date, desc.time);
    } else {
        std::snprintf(buf, sizeof(buf), "%s @0x%" PRIx32 ": no valid image", part->label, part->address);
    }
    return buf;
}

}  // namespace

void early() {
    confirmed = load_confirmed();
    if (rtc_magic != RTC_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        rtc_magic = RTC_MAGIC;
        rtc_quick_boots = 0;
    }
    if (confirmed) {
        rtc_quick_boots = 0;
        return;
    }
    rtc_quick_boots++;
    ESP_LOGW(TAG, "Firmware not confirmed yet (quick boot %" PRIu32 " of %d)", rtc_quick_boots,
             MAX_QUICK_CRASHES);
    if (rtc_quick_boots >= MAX_QUICK_CRASHES) {
        rtc_quick_boots = 0;
        revert("crashed repeatedly right after boot");
    }

    esp_timer_create_args_t stable = {};
    stable.callback = on_stable;
    stable.name = "guard_stable";
    esp_timer_create(&stable, &stable_timer);
    esp_timer_start_once(stable_timer, STABLE_UPTIME_US);

    esp_timer_create_args_t deadline = {};
    deadline.callback = on_deadline;
    deadline.name = "guard_deadline";
    esp_timer_create(&deadline, &deadline_timer);
    esp_timer_start_once(deadline_timer, NETWORK_DEADLINE_US);
}

void network_ok() {
    // Also tells a bootloader with rollback support that this image is good
    esp_ota_mark_app_valid_cancel_rollback();
    if (confirmed) {
        return;
    }
    confirmed = true;
    store_confirmed();
    if (deadline_timer != nullptr) {
        esp_timer_stop(deadline_timer);
    }
    ESP_LOGI(TAG, "Firmware confirmed: reachable over the network");
}

bool select_other_slot(std::string& info) {
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (other == nullptr) {
        info = "no second firmware slot";
        return false;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        info = "the other slot holds no valid firmware";
        return false;
    }
    // Another project's firmware does not know this one's settings and may
    // wipe them: staying is better than that
    if (std::strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        info = std::string("the other slot holds other firmware (") + desc.project_name + ")";
        return false;
    }
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        info = std::string("selecting it failed: ") + esp_err_to_name(err);
        return false;
    }
    info = describe_partition(other);
    return true;
}

std::string describe() {
    std::string out;
    out += "running: " + describe_partition(esp_ota_get_running_partition()) + "\n";
    out += "other:   " + describe_partition(esp_ota_get_next_update_partition(nullptr)) + "\n";
    out += std::string("confirmed: ") + (confirmed ? "yes" : "no (reverts if not online within 10 min)") + "\n";
    out += "reset reason: " + std::to_string(static_cast<int>(esp_reset_reason())) + "\n";
    return out;
}

}  // namespace boot_guard
