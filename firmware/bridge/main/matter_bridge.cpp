#include "matter_bridge.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/window-covering-server/window-covering-server.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "bed_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

using namespace esp_matter;
using namespace chip::app::Clusters;
namespace WC = chip::app::Clusters::WindowCovering;

namespace matter_bridge {

namespace {

const char* TAG = "bridge";

// Lift100ths: Matter's "percent closed" in hundredths. Closed = flat bed.
constexpr uint16_t FLAT = 10000;
constexpr uint16_t RAISED = 0;

// Full travel time of one motor; the octo_bed default. Calibration is F2.
constexpr int64_t FULL_TRAVEL_US = 30 * 1000000LL;
// Targets at either end drive a little longer, into the end stop, so the
// estimate is corrected whenever the bed goes fully up or flat.
constexpr int64_t END_STOP_EXTRA_US = 3 * 1000000LL;
constexpr uint32_t TICK_MS = 500;
constexpr int REACHABLE_EVERY_TICKS = 4;  // every 2 s
constexpr uint16_t REPORT_STEP = 100;     // report position changes of 1 %

const char* NVS_NAMESPACE = "octo_pos";

bool is_end(uint16_t position) {
    return position == FLAT || position == RAISED;
}

// One motor (head or feet) of one bed, exposed as a Window Covering.
class Cover : public WC::Delegate {
public:
    int bed = 0;
    bed_link::Part part = bed_link::Part::Head;
    uint16_t endpoint_id = 0;
    uint16_t position = FLAT;  // last known Lift100ths
    Cover* sibling = nullptr;  // the other motor of the same bed

    bool moving = false;
    uint16_t from = FLAT;
    uint16_t to = FLAT;
    int64_t started_us = 0;
    int64_t travel_us = 0;  // time to reach `to` at full speed
    int64_t run_us = 0;     // travel plus the end-stop extra

    CHIP_ERROR HandleMovement(WC::WindowCoveringType type) override;
    CHIP_ERROR HandleStopMotion() override;

    uint16_t estimate(int64_t now) const {
        if (!moving || travel_us <= 0 || now - started_us >= travel_us) {
            return moving ? to : position;
        }
        const int64_t delta = static_cast<int64_t>(to) - from;
        return static_cast<uint16_t>(from + delta * (now - started_us) / travel_us);
    }

    // Stop tracking a motion at its estimated position and make that the target.
    void freeze() {
        if (!moving) {
            return;
        }
        position = estimate(esp_timer_get_time());
        moving = false;
        publish();
        WC::Attributes::TargetPositionLiftPercent100ths::Set(endpoint_id, position);
        save();
    }

    void publish() { WC::LiftPositionSet(endpoint_id, position); }

    void save() const {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u16(handle, key(), position);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }

    void load() {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            uint16_t value;
            if (nvs_get_u16(handle, key(), &value) == ESP_OK && value <= FLAT) {
                position = value;
            }
            nvs_close(handle);
        }
    }

private:
    const char* key() const {
        static char text[8];
        std::snprintf(text, sizeof(text), "p%d%c", bed, part == bed_link::Part::Head ? 'h' : 'f');
        return text;
    }
};

// One bridged device per bed; head, legs and light are its parts.
struct BedEndpoints {
    uint16_t device_endpoint = 0;  // carries the name and Reachable
    Cover head;
    Cover feet;
    uint16_t light_endpoint = 0;
    bool reachable = false;
    char label[32] = {};
};

BedEndpoints beds[bed_link::MAX_BEDS];
int tick_count = 0;

CHIP_ERROR Cover::HandleMovement(WC::WindowCoveringType type) {
    if (type != WC::WindowCoveringType::Lift) {
        return CHIP_NO_ERROR;
    }
    chip::app::DataModel::Nullable<chip::Percent100ths> target;
    WC::Attributes::TargetPositionLiftPercent100ths::Get(endpoint_id, target);
    if (target.IsNull()) {
        return CHIP_NO_ERROR;
    }
    const int64_t now = esp_timer_get_time();
    // A new command replaces the bed's current motor command, so whatever
    // the bed was doing stops here.
    const uint16_t start = estimate(now);
    moving = false;
    position = start;
    sibling->freeze();

    const uint16_t goal = target.Value();
    if (goal == start && !is_end(goal)) {
        publish();
        return CHIP_NO_ERROR;
    }
    const bool up = goal < start;  // fewer hundredths closed = higher
    const int64_t distance = up ? start - goal : goal - start;
    const int64_t travel = FULL_TRAVEL_US * distance / FLAT;
    const int64_t run = travel + (is_end(goal) ? END_STOP_EXTRA_US : 0);

    bool ok = false;
    const int index = bed;
    const bed_link::Part motor = part;
    bed_link::run_in_host(
        [&] { ok = bed_link::move(index, motor, up, static_cast<float>(run) / 1000000.0f); });
    if (!ok) {
        ESP_LOGW(TAG, "Bed %d not ready, movement refused", bed);
        WC::Attributes::TargetPositionLiftPercent100ths::Set(endpoint_id, position);
        return CHIP_ERROR_INCORRECT_STATE;
    }
    moving = true;
    from = start;
    to = goal;
    started_us = now;
    travel_us = travel;
    run_us = run;
    return CHIP_NO_ERROR;
}

CHIP_ERROR Cover::HandleStopMotion() {
    const int index = bed;
    bed_link::run_in_host([&] { bed_link::stop(index); });
    // The stop command halts both motors of this bed
    freeze();
    sibling->freeze();
    return CHIP_NO_ERROR;
}

void update_reachable() {
    bed_link::Info info[bed_link::MAX_BEDS];
    bed_link::run_in_host([&] {
        for (int i = 0; i < bed_link::MAX_BEDS; ++i) {
            info[i] = bed_link::info(i);
        }
    });
    for (int i = 0; i < bed_link::MAX_BEDS; ++i) {
        BedEndpoints& bed = beds[i];
        if (!info[i].ready) {
            // bed_link stops the motors when the link drops
            bed.head.freeze();
            bed.feet.freeze();
        }
        if (info[i].ready == bed.reachable) {
            continue;
        }
        bed.reachable = info[i].ready;
        esp_matter_attr_val_t value = esp_matter_bool(bed.reachable);
        attribute::update(bed.device_endpoint, BridgedDeviceBasicInformation::Id,
                          BridgedDeviceBasicInformation::Attributes::Reachable::Id, &value);
        ESP_LOGI(TAG, "Bed %d %s", i, bed.reachable ? "reachable" : "unreachable");
    }
}

void track(Cover& cover, int64_t now) {
    if (!cover.moving) {
        return;
    }
    if (now - cover.started_us >= cover.run_us) {
        cover.moving = false;
        cover.position = cover.to;
        cover.publish();  // current == target: the cluster reports "stopped"
        cover.save();
        return;
    }
    const uint16_t estimate = cover.estimate(now);
    const int diff = static_cast<int>(estimate) - cover.position;
    if (diff >= REPORT_STEP || diff <= -REPORT_STEP) {
        cover.position = estimate;
        cover.publish();
    }
}

// Runs in the Matter thread, so attributes can be set directly.
void tick(chip::System::Layer* layer, void*) {
    const int64_t now = esp_timer_get_time();
    for (auto& bed : beds) {
        track(bed.head, now);
        track(bed.feet, now);
    }
    if (++tick_count % REACHABLE_EVERY_TICKS == 0) {
        update_reachable();
    }
    (void) layer->StartTimer(chip::System::Clock::Milliseconds32(TICK_MS), tick, nullptr);
}

int light_owner(uint16_t endpoint_id) {
    for (int i = 0; i < bed_link::MAX_BEDS; ++i) {
        if (beds[i].light_endpoint == endpoint_id) {
            return i;
        }
    }
    return -1;
}

esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t* val, void*) {
    // PRE_UPDATE so a write is refused while the bed is not connected
    if (type != attribute::PRE_UPDATE || cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) {
        return ESP_OK;
    }
    const int index = light_owner(endpoint_id);
    if (index < 0) {
        return ESP_OK;
    }
    const bool on = val->val.b;
    bool ok = false;
    bed_link::run_in_host([&] { ok = bed_link::light(index, on); });
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t on_identify(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void*) {
    return ESP_OK;
}

void on_event(const ChipDeviceEvent* event, intptr_t) {
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Commissioning complete");
            break;
        case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
            ESP_LOGW(TAG, "Commissioning failed, fail-safe timer expired");
            break;
        default:
            break;
    }
}

endpoint_t* add_bed_device(node_t* node, endpoint_t* aggregator, char* label) {
    endpoint::bridged_node::config_t config;
    config.bridged_device_basic_information.reachable = false;
    endpoint_t* ep = endpoint::bridged_node::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
    cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    cluster::bridged_device_basic_information::attribute::create_node_label(info, label, std::strlen(label));
    endpoint::set_parent_endpoint(ep, aggregator);
    return ep;
}

void add_cover(node_t* node, endpoint_t* device, Cover& cover) {
    endpoint::window_covering::config_t config;
    config.window_covering.feature_flags =
        cluster::window_covering::feature::lift::get_id() |
        cluster::window_covering::feature::position_aware_lift::get_id();
    config.window_covering.features.position_aware_lift.current_position_lift_percent_100ths = cover.position;
    config.window_covering.features.position_aware_lift.target_position_lift_percent_100ths = cover.position;
    config.window_covering.delegate = &cover;
    // Without "operational" controllers report a configuration problem
    config.window_covering.config_status = static_cast<uint8_t>(
        chip::to_underlying(WC::ConfigStatus::kOperational) | chip::to_underlying(WC::ConfigStatus::kLiftPositionAware));
    endpoint_t* ep = endpoint::window_covering::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    endpoint::set_parent_endpoint(ep, device);
    cover.endpoint_id = endpoint::get_id(ep);
    cover.SetEndpoint(cover.endpoint_id);
}

uint16_t add_light(node_t* node, endpoint_t* device) {
    endpoint::on_off_light::config_t config;
    endpoint_t* ep = endpoint::on_off_light::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    endpoint::set_parent_endpoint(ep, device);
    return endpoint::get_id(ep);
}

}  // namespace

void start() {
    node::config_t node_config;
    node_t* node = node::create(&node_config, on_attribute, on_identify);
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t* aggregator = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);

    for (int i = 0; i < bed_link::MAX_BEDS; ++i) {
        BedEndpoints& bed = beds[i];
        bed_link::Info info;
        bed_link::run_in_host([&] { info = bed_link::info(i); });
        const std::string name = info.configured && !info.name.empty() ? info.name : "Bed " + std::to_string(i + 1);
        std::snprintf(bed.label, sizeof(bed.label), "%s", name.c_str());

        bed.head.bed = bed.feet.bed = i;
        bed.head.part = bed_link::Part::Head;
        bed.feet.part = bed_link::Part::Feet;
        bed.head.sibling = &bed.feet;
        bed.feet.sibling = &bed.head;
        bed.head.load();
        bed.feet.load();

        endpoint_t* device = add_bed_device(node, aggregator, bed.label);
        bed.device_endpoint = endpoint::get_id(device);
        add_cover(node, device, bed.head);
        add_cover(node, device, bed.feet);
        bed.light_endpoint = add_light(node, device);
    }

    ESP_ERROR_CHECK(esp_matter::start(on_event));

    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        // NodeLabel is stored in NVS and restored at start, which would keep an
        // old name; the bed settings are the source of the name.
        for (auto& bed : beds) {
            esp_matter_attr_val_t label = esp_matter_char_str(bed.label, std::strlen(bed.label));
            attribute::update(bed.device_endpoint, BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &label);
        }
        (void) chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(TICK_MS), tick,
                                                           nullptr);
    });
}

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

std::string describe() {
    char qr[64] = {};
    char manual[32] = {};
    chip::MutableCharSpan qr_span(qr);
    chip::MutableCharSpan manual_span(manual);
    const auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const bool have_qr = GetQRCode(qr_span, flags) == CHIP_NO_ERROR;
    const bool have_manual = GetManualPairingCode(manual_span, flags) == CHIP_NO_ERROR;
    chip::Server& server = chip::Server::GetInstance();
    const unsigned fabrics = server.GetFabricTable().FabricCount();
    const bool window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
    std::string beds_text;
    for (int i = 0; i < bed_link::MAX_BEDS; ++i) {
        const BedEndpoints& bed = beds[i];
        char line[200];
        std::snprintf(line, sizeof(line),
                      "bed %d '%s': endpoints device=%u head=%u legs=%u light=%u, %s, head %u%% raised, legs %u%% raised\n",
                      i, bed.label, bed.device_endpoint, bed.head.endpoint_id, bed.feet.endpoint_id, bed.light_endpoint,
                      bed.reachable ? "reachable" : "unreachable", (FLAT - bed.head.position) / 100,
                      (FLAT - bed.feet.position) / 100);
        beds_text += line;
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    std::string out;
    out += "fabrics: " + std::to_string(fabrics) + ", commissioning window " + (window_open ? "open" : "closed") + "\n";
    if (have_qr) {
        out += std::string("QR code:     ") + qr + "\n";
    }
    if (have_manual) {
        out += std::string("manual code: ") + manual + "\n";
    }
    return out + beds_text;
}

}  // namespace matter_bridge
