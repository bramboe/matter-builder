#include "matter_bridge.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/clusters/operational-state-server/operational-state-server.h>
#include <app/clusters/window-covering-server/window-covering-server.h>
#include <app/reporting/reporting.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/DeviceInfoProvider.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "bed_link.h"
#include "matter_info.h"
#include "matter_thread.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mb_core.h"
#include "net.h"
#include "nvs.h"
#include "setup_code.h"

using namespace esp_matter;
using namespace chip::app::Clusters;
namespace WC = chip::app::Clusters::WindowCovering;
namespace OS = chip::app::Clusters::OperationalState;
namespace MS = chip::app::Clusters::ModeSelect;

namespace matter_bridge {

namespace {

const char* TAG = "bridge";

// Lift100ths: Matter's "percent closed" in hundredths. Closed = flat bed.
constexpr uint16_t FLAT = 10000;
constexpr uint16_t RAISED = 0;

// Targets at either end drive a little longer, into the end stop, so the
// estimate is corrected whenever the bed goes fully up or flat.
constexpr float END_STOP_EXTRA_S = 3.0f;
constexpr uint32_t TICK_MS = 500;
constexpr uint16_t REPORT_STEP = 100;  // report position changes of 1 %

const char* NVS_NAMESPACE = "octo_pos";

// Manufacturer and model come from mb_core::App. Vendor and product ID stay
// the Matter test IDs (0xFFF1/0x8000) that the test certificates need.
constexpr const char* HARDWARE = "ESP32";
constexpr const char* BED_VENDOR = "Octo";
constexpr const char* BED_PRODUCT = "Octo bed";

// The preset choice (Mode Select) acts once and then returns to MODE_NONE,
// so the same choice can be made again.
constexpr int PRESET_SLOTS = 3;
constexpr uint8_t MODE_NONE = 0;
constexpr uint8_t MODE_FLAT = 1;
constexpr uint8_t MODE_PRESET = 2;  // presets 1-3: modes 2-4
constexpr uint8_t MODE_MATCH = 5;   // move to the other bed's position
constexpr uint8_t MODE_SAVE = 6;    // save as preset 1-3: modes 6-8
constexpr int MODE_COUNT = 9;

// The preset choice in the language set on the settings page ("language")
struct Language {
    const char* code;
    const char* description;  // the entity name in Home Assistant
    const char* flat;
    const char* preset;  // %s: slot
    const char* match;   // %s: the other bed
    const char* save;    // %s: slot
    const char* head;    // part names, at most 16 bytes (fixed label)
    const char* legs;
    const char* light;
};
constexpr Language LANGUAGES[] = {
    {"en", "Preset", "Flat", "Preset %s", "Match %s", "Save as preset %s", "Head", "Legs", "Light"},
    {"nl", "Stand", "Plat", "Stand %s", "Zelfde als %s", "Bewaar als stand %s", "Hoofd", "Benen", "Licht"},
    {"de", "Position", "Flach", "Position %s", "Wie %s", "Als Position %s speichern", "Kopf", "Beine", "Licht"},
    {"es", "Posici\u00f3n", "Plano", "Posici\u00f3n %s", "Igual que %s", "Guardar como posici\u00f3n %s", "Cabeza", "Piernas", "Luz"},
};
constexpr int LANGUAGE_COUNT = sizeof(LANGUAGES) / sizeof(LANGUAGES[0]);
int language = 0;

class DeviceInfo : public chip::DeviceLayer::Internal::GenericDeviceInstanceInfoProvider<
                       chip::DeviceLayer::Internal::ESP32Config> {
public:
    DeviceInfo() : GenericDeviceInstanceInfoProvider(chip::DeviceLayer::ConfigurationManagerImpl::GetDefaultInstance()) {}

    CHIP_ERROR GetVendorName(char* buf, size_t size) override { return copy(mb_core::app().vendor_name, buf, size); }
    CHIP_ERROR GetProductName(char* buf, size_t size) override { return copy(mb_core::app().product_name, buf, size); }
    CHIP_ERROR GetHardwareVersionString(char* buf, size_t size) override { return copy(HARDWARE, buf, size); }

private:
    static CHIP_ERROR copy(const char* text, char* buf, size_t size) {
        VerifyOrReturnError(std::strlen(text) < size, CHIP_ERROR_BUFFER_TOO_SMALL);
        std::strcpy(buf, text);
        return CHIP_NO_ERROR;
    }
};

DeviceInfo device_info;

bool is_end(uint16_t position) {
    return position == FLAT || position == RAISED;
}

bool nvs_get(const char* key, uint32_t& value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u32(handle, key, &value) == ESP_OK;
    nvs_close(handle);
    return ok;
}

void nvs_put(const char* key, uint32_t value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, key, value);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// One motor (head or feet) of one bed, exposed as a Window Covering.
class Cover : public WC::Delegate {
public:
    int bed = 0;
    bed_link::Part part = bed_link::Part::Head;
    uint16_t endpoint_id = 0;
    uint16_t position = FLAT;  // last published Lift100ths
    uint16_t current = FLAT;   // latest estimate, also between reports

    CHIP_ERROR HandleMovement(WC::WindowCoveringType type) override;
    CHIP_ERROR HandleStopMotion() override;

    // Drive towards goal; false when the bed is not ready.
    bool move_to(uint16_t goal);

    // Follow the motor's latest run, including runs started elsewhere (the
    // settings page, calibration), and publish the estimated position.
    void follow(const bed_link::RunInfo& run, float full_s);

    // A position known for certain, e.g. the top after calibration.
    void set_known(uint16_t value) {
        following = false;
        current = position = value;
        publish();
        set_target(value);
        save();
    }

    void publish() { WC::LiftPositionSet(endpoint_id, position); }
    void set_target(uint16_t value) { WC::Attributes::TargetPositionLiftPercent100ths::Set(endpoint_id, value); }

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
                current = position = value;
            }
            nvs_close(handle);
        }
    }

private:
    uint32_t run_id = 0;  // the bed_link run being followed
    bool following = false;
    uint16_t from = FLAT;  // position when that run started
    int32_t goal = -1;     // where a run started here ends; -1 for runs started elsewhere

    const char* key() const {
        static char text[24];
        std::snprintf(text, sizeof(text), "p%d%c", bed, part == bed_link::Part::Head ? 'h' : 'f');
        return text;
    }
};

// The bed's stop button: an Operational State cluster that only accepts
// Stop, which controllers such as Home Assistant show as a push button.
// The state is Running while a motor command is active; the cluster ignores
// Stop when the state is Stopped.
class StopButton : public OS::Delegate {
public:
    int bed = 0;

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override { return {}; }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OS::GenericOperationalState& state) override {
        static constexpr OS::OperationalStateEnum STATES[] = {OS::OperationalStateEnum::kStopped,
                                                              OS::OperationalStateEnum::kRunning};
        VerifyOrReturnError(index < sizeof(STATES) / sizeof(STATES[0]), CHIP_ERROR_NOT_FOUND);
        state = OS::GenericOperationalState(chip::to_underlying(STATES[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t, chip::MutableCharSpan&) override { return CHIP_ERROR_NOT_FOUND; }

    void HandlePauseStateCallback(OS::GenericOperationalError& err) override { refuse(err); }
    void HandleResumeStateCallback(OS::GenericOperationalError& err) override { refuse(err); }
    void HandleStartStateCallback(OS::GenericOperationalError& err) override { refuse(err); }
    void HandleStopStateCallback(OS::GenericOperationalError& err) override;

    void set_running(bool running) {
        if (GetInstance() != nullptr) {
            const auto state = running ? OS::OperationalStateEnum::kRunning : OS::OperationalStateEnum::kStopped;
            (void) GetInstance()->SetOperationalState(chip::to_underlying(state));
        }
    }

private:
    static void refuse(OS::GenericOperationalError& err) {
        err.Set(chip::to_underlying(OS::ErrorStateEnum::kCommandInvalidInState));
    }
};

// One bridged device per bed; head, legs and light are its parts. The stop
// button and the preset choice sit on the device itself.
struct BedEndpoints {
    uint16_t device_endpoint = 0;  // name, Reachable, stop button, presets
    Cover head;
    Cover feet;
    StopButton stop;
    uint16_t light_endpoint = 0;
    bool reachable = false;
    char label[32] = {};
    // Head, legs and light are bridged devices of their own ("Sam Head"):
    // Home Assistant names an entity after its device and ignores the part
    // labels, so as parts of one device all three were called "Sam"
    char part_names[3][48] = {};

    // Preset choice labels and options, read by the Mode Select server
    char mode_labels[MODE_COUNT][40] = {};
    MS::Structs::ModeOptionStruct::Type modes[MODE_COUNT];
    size_t mode_count = 0;
};

BedEndpoints beds[bed_link::MAX_BEDS];
// Head, legs and light under their bed (one device, one Apple Home accessory
// per bed) or each a bridged device of its own: the "group" setting, read at
// start. Either way they are created in the same order, so the endpoint
// numbers stay the same.
bool grouped = true;

void name_parts(BedEndpoints& bed);
void set_parts_attribute(BedEndpoints& bed, uint32_t attribute_id, esp_matter_attr_val_t* value);
// Beds with endpoints: bed_link::bed_count() at start. Endpoints of a bed not
// made are 0, the root endpoint, so loops stop at this count.
int bed_total = 1;

// The name of the bed part on an endpoint, or nullptr for other endpoints
const char* part_label(chip::EndpointId endpoint_id) {
    const Language& text = LANGUAGES[language];
    for (int i = 0; i < bed_total; ++i) {
        const BedEndpoints& bed = beds[i];
        if (endpoint_id == bed.head.endpoint_id) {
            return text.head;
        }
        if (endpoint_id == bed.feet.endpoint_id) {
            return text.legs;
        }
        if (endpoint_id == bed.light_endpoint) {
            return text.light;
        }
    }
    return nullptr;
}

// Fixed labels name the parts of a bed. Home Assistant adds the
// "ha_entitylabel" label to the device name for the test vendor and product
// IDs, so the head cover arrives as "Sam Head" (cover.sam_head) instead of
// two covers both called "Sam". The bed name itself is the device name.
// User labels, locales and calendar types are not used (no such clusters).
class PartLabels : public chip::DeviceLayer::DeviceInfoProvider {
public:
    FixedLabelIterator* IterateFixedLabel(chip::EndpointId endpoint_id) override {
        return chip::Platform::New<Labels>(part_label(endpoint_id));
    }
    UserLabelIterator* IterateUserLabel(chip::EndpointId) override { return nullptr; }
    SupportedLocalesIterator* IterateSupportedLocales() override { return nullptr; }
    SupportedCalendarTypesIterator* IterateSupportedCalendarTypes() override { return nullptr; }

protected:
    CHIP_ERROR SetUserLabelLength(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetUserLabelLength(chip::EndpointId, size_t&) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR SetUserLabelAt(chip::EndpointId, size_t, const UserLabelType&) override {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    class Labels : public FixedLabelIterator {
    public:
        explicit Labels(const char* value) : value_(value) {}
        size_t Count() override { return value_ != nullptr ? 1 : 0; }
        bool Next(FixedLabelType& output) override {
            if (value_ == nullptr || done_) {
                return false;
            }
            output.label = chip::CharSpan::fromCharString("ha_entitylabel");
            output.value = chip::CharSpan::fromCharString(value_);
            done_ = true;
            return true;
        }
        void Release() override { chip::Platform::Delete(this); }

    private:
        const char* value_;
        bool done_ = false;
    };
};

PartLabels part_labels;

int bed_of_device(chip::EndpointId endpoint_id) {
    for (int i = 0; i < bed_total; ++i) {
        if (beds[i].device_endpoint == endpoint_id) {
            return i;
        }
    }
    return -1;
}

class PresetModes : public MS::SupportedModesManager {
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpoint_id) const override {
        const int index = bed_of_device(endpoint_id);
        if (index < 0) {
            return ModeOptionsProvider();
        }
        const BedEndpoints& bed = beds[index];
        return ModeOptionsProvider(bed.modes, bed.modes + bed.mode_count);
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpoint_id, uint8_t mode,
        const MS::Structs::ModeOptionStruct::Type** option) const override {
        const ModeOptionsProvider provider = getModeOptionsProvider(endpoint_id);
        for (auto* it = provider.begin(); it != provider.end(); ++it) {
            if (it->mode == mode) {
                *option = it;
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};

PresetModes preset_modes;

bool Cover::move_to(uint16_t target) {
    const uint16_t start = current;
    if (target == start && !is_end(target)) {
        set_target(target);
        return true;
    }
    // At an end the motor still drives into the end stop, to correct drift
    const bool up = target < start || (target == start && target == RAISED);
    const int32_t distance = up ? start - target : target - start;
    const int index = bed;
    const bed_link::Part motor = part;
    uint32_t id = 0;
    bed_link::run_in_host([&] {
        // The calibrated full travel time of this motor
        const bed_link::Info info = bed_link::info(index);
        const float full_s = motor == bed_link::Part::Head ? info.head_travel_s : info.feet_travel_s;
        const float run_s = full_s * distance / FLAT + (is_end(target) ? END_STOP_EXTRA_S : 0.0f);
        id = bed_link::move(index, motor, up, run_s);
    });
    if (id == 0) {
        ESP_LOGW(TAG, "Bed %d not ready, movement refused", bed);
        set_target(position);
        return false;
    }
    run_id = id;
    following = true;
    from = start;
    goal = target;
    set_target(target);
    return true;
}

void Cover::follow(const bed_link::RunInfo& run, float full_s) {
    if (run.id != 0 && run.id != run_id) {
        // Started elsewhere: it starts from the current estimate
        run_id = run.id;
        following = true;
        from = current;
        goal = -1;
        set_target(run.up ? RAISED : FLAT);
    }
    if (!following || full_s <= 0.0f) {
        return;
    }
    const int32_t moved = static_cast<int32_t>(run.done_s / full_s * FLAT);
    int32_t estimate = run.up ? from - moved : from + moved;
    estimate = estimate < RAISED ? RAISED : estimate > FLAT ? FLAT : estimate;
    if (!run.moving) {
        // Done, stopped or the link dropped
        following = false;
        if (run.finished && goal >= 0) {
            estimate = goal;
        }
        current = position = static_cast<uint16_t>(estimate);
        publish();  // current == target: the cluster reports "stopped"
        set_target(position);
        save();
        return;
    }
    current = static_cast<uint16_t>(estimate);
    const int diff = static_cast<int>(current) - position;
    if (diff >= REPORT_STEP || diff <= -REPORT_STEP) {
        position = current;
        publish();
    }
}

CHIP_ERROR Cover::HandleMovement(WC::WindowCoveringType type) {
    if (type != WC::WindowCoveringType::Lift) {
        return CHIP_NO_ERROR;
    }
    chip::app::DataModel::Nullable<chip::Percent100ths> target;
    WC::Attributes::TargetPositionLiftPercent100ths::Get(endpoint_id, target);
    if (target.IsNull()) {
        return CHIP_NO_ERROR;
    }
    // The other motor keeps going; bed_link combines or queues the two
    return move_to(target.Value()) ? CHIP_NO_ERROR : CHIP_ERROR_INCORRECT_STATE;
}

CHIP_ERROR Cover::HandleStopMotion() {
    // Only this motor; the position is settled on the next tick
    const int index = bed;
    const bed_link::Part motor = part;
    bed_link::run_in_host([&] { bed_link::stop(index, motor); });
    return CHIP_NO_ERROR;
}

void StopButton::HandleStopStateCallback(OS::GenericOperationalError& err) {
    const int index = bed;
    bed_link::run_in_host([&] { bed_link::stop(index); });
    set_running(false);
    err.Set(chip::to_underlying(OS::ErrorStateEnum::kNoError));
}

// Presets: head and feet position, stored as (head << 16) | feet.
const char* preset_key(int index, int slot) {
    static char text[24];
    std::snprintf(text, sizeof(text), "s%d%d", index, slot);
    return text;
}

bool preset_get(int index, int slot, uint16_t& head, uint16_t& feet) {
    uint32_t value;
    if (!nvs_get(preset_key(index, slot), value)) {
        return false;
    }
    head = static_cast<uint16_t>(value >> 16);
    feet = static_cast<uint16_t>(value & 0xFFFF);
    return head <= FLAT && feet <= FLAT;
}

void preset_save(int index, int slot) {
    const BedEndpoints& bed = beds[index];
    nvs_put(preset_key(index, slot), (static_cast<uint32_t>(bed.head.current) << 16) | bed.feet.current);
    ESP_LOGI(TAG, "Bed %d preset %d saved", index, slot);
}

bool move_bed(int index, uint16_t head, uint16_t feet) {
    BedEndpoints& bed = beds[index];
    return bed.head.move_to(head) && bed.feet.move_to(feet);
}

// Carry out a preset choice. Runs in the Matter thread.
bool apply_mode(int index, uint8_t mode) {
    if (mode == MODE_FLAT) {
        return move_bed(index, FLAT, FLAT);
    }
    if (mode >= MODE_PRESET && mode < MODE_PRESET + PRESET_SLOTS) {
        uint16_t head;
        uint16_t feet;
        if (!preset_get(index, mode - MODE_PRESET + 1, head, feet)) {
            ESP_LOGW(TAG, "Bed %d preset %d is empty", index, mode - MODE_PRESET + 1);
            return false;
        }
        return move_bed(index, head, feet);
    }
    if (mode == MODE_MATCH) {
        if (bed_total < 2) {
            return false;
        }
        const BedEndpoints& other = beds[1 - index];
        return move_bed(index, other.head.current, other.feet.current);
    }
    if (mode >= MODE_SAVE && mode < MODE_SAVE + PRESET_SLOTS) {
        preset_save(index, mode - MODE_SAVE + 1);
        return true;
    }
    return mode == MODE_NONE;
}

void reset_mode(intptr_t index) {
    esp_matter_attr_val_t none = esp_matter_uint8(MODE_NONE);
    attribute::update(beds[index].device_endpoint, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &none);
}

void add_mode(BedEndpoints& bed, uint8_t mode, const char* format, const char* text = "") {
    char* label = bed.mode_labels[bed.mode_count];
    std::snprintf(label, sizeof(bed.mode_labels[0]), format, text);
    auto& option = bed.modes[bed.mode_count++];
    option.label = chip::CharSpan(label, std::strlen(label));
    option.mode = mode;
}

void build_modes(int index) {
    const Language& text = LANGUAGES[language];
    BedEndpoints& bed = beds[index];
    bed.mode_count = 0;
    add_mode(bed, MODE_NONE, "-");
    add_mode(bed, MODE_FLAT, text.flat);
    for (int slot = 1; slot <= PRESET_SLOTS; ++slot) {
        char number[4];
        std::snprintf(number, sizeof(number), "%d", slot);
        add_mode(bed, MODE_PRESET + slot - 1, text.preset, number);
    }
    if (bed_total == 2) {
        add_mode(bed, MODE_MATCH, text.match, beds[1 - index].label);
    }
    for (int slot = 1; slot <= PRESET_SLOTS; ++slot) {
        char number[4];
        std::snprintf(number, sizeof(number), "%d", slot);
        add_mode(bed, MODE_SAVE + slot - 1, text.save, number);
    }
}

void load_language() {
    uint32_t value;
    if (nvs_get("lang", value) && value < LANGUAGE_COUNT) {
        language = static_cast<int>(value);
    }
}

void update_from_links() {
    bed_link::Info info[bed_link::MAX_BEDS];
    bed_link::run_in_host([&] {
        for (int i = 0; i < bed_total; ++i) {
            info[i] = bed_link::info(i);
        }
    });
    for (int i = 0; i < bed_total; ++i) {
        BedEndpoints& bed = beds[i];
        bed.head.follow(info[i].head, info[i].head_travel_s);
        bed.feet.follow(info[i].feet, info[i].feet_travel_s);
        bed.stop.set_running(info[i].moving);
        // A bed added or renamed on the settings page: new name right away
        const std::string name = info[i].configured && !info[i].name.empty() ? info[i].name : "Bed " + std::to_string(i + 1);
        if (name != bed.label) {
            std::snprintf(bed.label, sizeof(bed.label), "%s", name.c_str());
            esp_matter_attr_val_t label = esp_matter_char_str(bed.label, std::strlen(bed.label));
            attribute::update(bed.device_endpoint, BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &label);
            name_parts(bed);
            if (bed_total == 2) {
                build_modes(1 - i);  // the other bed's "Match <name>"
                MatterReportingAttributeChangeCallback(beds[1 - i].device_endpoint, ModeSelect::Id,
                                                       ModeSelect::Attributes::SupportedModes::Id);
            }
        }
        if (info[i].ready == bed.reachable) {
            continue;
        }
        bed.reachable = info[i].ready;
        esp_matter_attr_val_t value = esp_matter_bool(bed.reachable);
        attribute::update(bed.device_endpoint, BridgedDeviceBasicInformation::Id,
                          BridgedDeviceBasicInformation::Attributes::Reachable::Id, &value);
        set_parts_attribute(bed, BridgedDeviceBasicInformation::Attributes::Reachable::Id, &value);
        ESP_LOGI(TAG, "Bed %d %s", i, bed.reachable ? "reachable" : "unreachable");
    }
}

// Runs in the Matter thread, so attributes can be set directly.
void tick(chip::System::Layer* layer, void*) {
    update_from_links();
    (void) layer->StartTimer(chip::System::Clock::Milliseconds32(TICK_MS), tick, nullptr);
}

int light_owner(uint16_t endpoint_id) {
    for (int i = 0; i < bed_total; ++i) {
        if (beds[i].light_endpoint == endpoint_id) {
            return i;
        }
    }
    return -1;
}

esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t* val, void*) {
    // PRE_UPDATE so a write is refused while the bed is not connected
    if (type != attribute::PRE_UPDATE) {
        return ESP_OK;
    }
    if (cluster_id == ModeSelect::Id && attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        const int index = bed_of_device(endpoint_id);
        if (index < 0 || val->val.u8 == MODE_NONE) {
            return ESP_OK;
        }
        if (!apply_mode(index, val->val.u8)) {
            ESP_LOGW(TAG, "Bed %d preset choice %u failed", index, val->val.u8);
        }
        // Back to "-" so the same choice can be made again
        (void) chip::DeviceLayer::PlatformMgr().ScheduleWork(reset_mode, index);
        return ESP_OK;
    }
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) {
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

endpoint_t* add_bed_device(node_t* node, endpoint_t* aggregator, BedEndpoints& bed) {
    endpoint::bridged_node::config_t config;
    config.bridged_device_basic_information.reachable = false;
    endpoint_t* ep = endpoint::bridged_node::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
    cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    namespace bdbi = cluster::bridged_device_basic_information::attribute;
    bdbi::create_node_label(info, bed.label, std::strlen(bed.label));
    bdbi::create_vendor_name(info, const_cast<char*>(BED_VENDOR), std::strlen(BED_VENDOR));
    bdbi::create_product_name(info, const_cast<char*>(BED_PRODUCT), std::strlen(BED_PRODUCT));
    // Home Assistant reads a bed's vendor and product here, not on the root:
    // only with these does it use the part labels below (head, legs, light)
    bdbi::create_vendor_id(info, CONFIG_DEVICE_VENDOR_ID);
    bdbi::create_product_id(info, CONFIG_DEVICE_PRODUCT_ID);

    cluster::operational_state::config_t stop_config;
    stop_config.delegate = &bed.stop;
    cluster_t* stop_cluster = cluster::operational_state::create(ep, &stop_config, CLUSTER_FLAG_SERVER);
    cluster::operational_state::command::create_stop(stop_cluster);
    cluster::operational_state::command::create_operational_command_response(stop_cluster);

    cluster::mode_select::config_t mode_config;
    std::snprintf(mode_config.description, sizeof(mode_config.description), "%s", LANGUAGES[language].description);
    mode_config.current_mode = MODE_NONE;
    mode_config.delegate = &preset_modes;
    cluster::mode_select::create(ep, &mode_config, CLUSTER_FLAG_SERVER);

    endpoint::set_parent_endpoint(ep, aggregator);
    return ep;
}

// A part of a bed as a bridged device of its own, next to the bed under the
// aggregator; created in the same order as before, so its endpoint number
// (and with it a controller's entity) stays the same
void make_bridged(endpoint_t* ep, const char* name) {
    endpoint::bridged_node::config_t config;
    config.bridged_device_basic_information.reachable = false;
    endpoint::bridged_node::add(ep, &config);
    cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    namespace bdbi = cluster::bridged_device_basic_information::attribute;
    bdbi::create_node_label(info, const_cast<char*>(name), std::strlen(name));
    bdbi::create_vendor_name(info, const_cast<char*>(BED_VENDOR), std::strlen(BED_VENDOR));
    bdbi::create_product_name(info, const_cast<char*>(BED_PRODUCT), std::strlen(BED_PRODUCT));
    bdbi::create_vendor_id(info, CONFIG_DEVICE_VENDOR_ID);
    bdbi::create_product_id(info, CONFIG_DEVICE_PRODUCT_ID);
}

void add_cover(node_t* node, endpoint_t* parent, Cover& cover, const char* name) {
    endpoint::window_covering::config_t config;
    config.window_covering.feature_flags =
        cluster::window_covering::feature::lift::get_id() |
        cluster::window_covering::feature::position_aware_lift::get_id();
    config.window_covering.features.position_aware_lift.current_position_lift_percent_100ths = cover.position;
    config.window_covering.features.position_aware_lift.target_position_lift_percent_100ths = cover.position;
    config.window_covering.delegate = &cover;
    endpoint_t* ep = endpoint::window_covering::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
    if (!grouped) make_bridged(ep, name);
    endpoint::set_parent_endpoint(ep, parent);
    cluster::fixed_label::config_t label_config;
    cluster::fixed_label::create(ep, &label_config, CLUSTER_FLAG_SERVER);
    cover.endpoint_id = endpoint::get_id(ep);
    cover.SetEndpoint(cover.endpoint_id);
}

uint16_t add_light(node_t* node, endpoint_t* parent, const char* name) {
    endpoint::on_off_light::config_t config;
    endpoint_t* ep = endpoint::on_off_light::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
    if (!grouped) make_bridged(ep, name);
    endpoint::set_parent_endpoint(ep, parent);
    cluster::fixed_label::config_t label_config;
    cluster::fixed_label::create(ep, &label_config, CLUSTER_FLAG_SERVER);
    return endpoint::get_id(ep);
}

// The parts' names follow the bed's name and the language (in the Matter thread)
void name_parts(BedEndpoints& bed) {
    const Language& text = LANGUAGES[language];
    const char* parts[3] = {text.head, text.legs, text.light};
    const uint16_t endpoints[3] = {bed.head.endpoint_id, bed.feet.endpoint_id, bed.light_endpoint};
    for (int k = 0; k < 3; ++k) {
        std::snprintf(bed.part_names[k], sizeof(bed.part_names[k]), "%s %s", bed.label, parts[k]);
        if (endpoints[k] == 0 || grouped) continue;  // grouped: parts have no name of their own
        esp_matter_attr_val_t name = esp_matter_char_str(bed.part_names[k], std::strlen(bed.part_names[k]));
        attribute::update(endpoints[k], BridgedDeviceBasicInformation::Id,
                          BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &name);
    }
}

void set_parts_attribute(BedEndpoints& bed, uint32_t attribute_id, esp_matter_attr_val_t* value) {
    if (grouped) return;  // only the bed is a bridged device
    for (uint16_t ep : {bed.head.endpoint_id, bed.feet.endpoint_id, bed.light_endpoint}) {
        if (ep != 0) attribute::update(ep, BridgedDeviceBasicInformation::Id, attribute_id, value);
    }
}

// Calibration ends with the motor at the top: make that the known position.
// Called in the BLE host task, so the update is handed to the Matter thread.
void on_calibrated(int index, bed_link::Part part, float) {
    const intptr_t arg = index * 2 + (part == bed_link::Part::Head ? 0 : 1);
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t value) {
        BedEndpoints& bed = beds[value / 2];
        (value % 2 == 0 ? bed.head : bed.feet).set_known(RAISED);
    }, arg);
}

}  // namespace

void start() {
    node::config_t node_config;
    node_t* node = node::create(&node_config, on_attribute, on_identify);
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t* aggregator = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    load_language();
    bed_total = bed_link::bed_count();
    grouped = bed_link::group_parts();

    for (int i = 0; i < bed_total; ++i) {
        BedEndpoints& bed = beds[i];
        bed_link::Info info;
        bed_link::run_in_host([&] { info = bed_link::info(i); });
        const std::string name = info.configured && !info.name.empty() ? info.name : "Bed " + std::to_string(i + 1);
        std::snprintf(bed.label, sizeof(bed.label), "%s", name.c_str());
    }

    for (int i = 0; i < bed_total; ++i) {
        BedEndpoints& bed = beds[i];
        bed.head.bed = bed.feet.bed = bed.stop.bed = i;
        bed.head.part = bed_link::Part::Head;
        bed.feet.part = bed_link::Part::Feet;
        bed.head.load();
        bed.feet.load();
        build_modes(i);  // needs both labels

        endpoint_t* device = add_bed_device(node, aggregator, bed);
        bed.device_endpoint = endpoint::get_id(device);
        const Language& text = LANGUAGES[language];
        std::snprintf(bed.part_names[0], sizeof(bed.part_names[0]), "%s %s", bed.label, text.head);
        std::snprintf(bed.part_names[1], sizeof(bed.part_names[1]), "%s %s", bed.label, text.legs);
        std::snprintf(bed.part_names[2], sizeof(bed.part_names[2]), "%s %s", bed.label, text.light);
        endpoint_t* parent = grouped ? device : aggregator;
        add_cover(node, parent, bed.head, bed.part_names[0]);
        add_cover(node, parent, bed.feet, bed.part_names[1]);
        bed.light_endpoint = add_light(node, parent, bed.part_names[2]);
    }

    bed_link::run_in_host([] { bed_link::on_calibrated(on_calibrated); });
    esp_matter::set_custom_device_instance_info_provider(&device_info);
    esp_matter::set_custom_device_info_provider(&part_labels);
    setup_code::install();
    ESP_ERROR_CHECK(esp_matter::start(on_event));

    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        // NodeLabel and ConfigStatus are stored in NVS and restored at start,
        // which would keep old values; set them from the bed settings.
        // Without "operational" controllers report a configuration problem.
        const chip::BitMask<WC::ConfigStatus> status(WC::ConfigStatus::kOperational,
                                                     WC::ConfigStatus::kLiftPositionAware);
        // The root NodeLabel is what Apple Home names the bridge ("Accessory" when empty),
        // and it may have been renamed by a controller, so only fill it when empty.
        esp_matter_attr_val_t root_label = esp_matter_invalid(nullptr);
        attribute_t* node_label = attribute::get(0, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id);
        if (node_label != nullptr && attribute::get_val(node_label, &root_label) == ESP_OK &&
            root_label.val.a.s == 0) {
            esp_matter_attr_val_t name = esp_matter_char_str(const_cast<char*>(mb_core::app().product_name), std::strlen(mb_core::app().product_name));
            attribute::update(0, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id, &name);
        }
        // Grouped or not, and how many beds: controllers read it again when it changed
        matter_info::note_structure(static_cast<uint32_t>(bed_total * 2 + (grouped ? 1 : 0)));
        for (int i = 0; i < bed_total; ++i) {
            BedEndpoints& bed = beds[i];
            esp_matter_attr_val_t label = esp_matter_char_str(bed.label, std::strlen(bed.label));
            attribute::update(bed.device_endpoint, BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &label);
            name_parts(bed);
            WC::ConfigStatusSet(bed.head.endpoint_id, status);
            WC::ConfigStatusSet(bed.feet.endpoint_id, status);
            reset_mode(i);
        }
        (void) chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(TICK_MS), tick,
                                                           nullptr);
    });
}

std::string language_command(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        return std::string(LANGUAGES[language].code) + "\n";
    }
    for (int i = 0; i < LANGUAGE_COUNT; ++i) {
        if (args.size() == 2 && args[1] == LANGUAGES[i].code) {
            nvs_put("lang", static_cast<uint32_t>(i));
            const bool ran = matter_thread::run([&] {
                language = i;
                for (int b = 0; b < bed_total; ++b) {
                    build_modes(b);
                    esp_matter_attr_val_t description = esp_matter_char_str(
                        const_cast<char*>(LANGUAGES[i].description), std::strlen(LANGUAGES[i].description));
                    attribute::update(beds[b].device_endpoint, ModeSelect::Id, ModeSelect::Attributes::Description::Id,
                                      &description);
                    MatterReportingAttributeChangeCallback(beds[b].device_endpoint, ModeSelect::Id,
                                                           ModeSelect::Attributes::SupportedModes::Id);
                }
            });
            // Stored either way: without Matter's turn now, it shows after the next start
            return ran ? std::string(LANGUAGES[i].code) + "\n" : "stored: shown after a restart (Matter is busy)\n";
        }
    }
    return "usage: language [en|nl|de|es]\n";
}

std::string preset_command(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "list") {
        std::string out = "{\"beds\":[";
        matter_thread::run([&] {
            for (int i = 0; i < bed_total; ++i) {
                const BedEndpoints& bed = beds[i];
                char text[64];
                std::snprintf(text, sizeof(text), "%s{\"head\":%u,\"legs\":%u,\"presets\":[", i ? "," : "",
                              (FLAT - bed.head.current) / 100, (FLAT - bed.feet.current) / 100);
                out += text;
                for (int slot = 1; slot <= PRESET_SLOTS; ++slot) {
                    uint16_t head;
                    uint16_t feet;
                    if (preset_get(i, slot, head, feet)) {
                        std::snprintf(text, sizeof(text), "{\"head\":%u,\"legs\":%u}", (FLAT - head) / 100,
                                      (FLAT - feet) / 100);
                    } else {
                        std::snprintf(text, sizeof(text), "null");
                    }
                    out += text;
                    out += slot < PRESET_SLOTS ? "," : "]}";
                }
            }
        });
        return out + "]}\n";
    }
    if (args.size() < 3) {
        return "usage: preset list | preset <0|1> <flat|1|2|3|match> | preset <0|1> save <1|2|3>\n";
    }
    const int index = std::atoi(args[1].c_str());
    if (index < 0 || index >= bed_total || (args[1] != "0" && args[1] != "1")) {
        return bed_total == 2 ? "bed must be 0 or 1\n" : "bed must be 0\n";
    }
    uint8_t mode = MODE_NONE;
    const int slot = args.size() > 3 ? std::atoi(args[3].c_str()) : std::atoi(args[2].c_str());
    if (args[2] == "flat") {
        mode = MODE_FLAT;
    } else if (args[2] == "match") {
        mode = MODE_MATCH;
    } else if (args[2] == "save" && args.size() == 4 && slot >= 1 && slot <= PRESET_SLOTS) {
        mode = MODE_SAVE + slot - 1;
    } else if (args.size() == 3 && slot >= 1 && slot <= PRESET_SLOTS) {
        mode = MODE_PRESET + slot - 1;
    } else {
        return "usage: preset list | preset <0|1> <flat|1|2|3|match> | preset <0|1> save <1|2|3>\n";
    }
    bool ok = false;
    if (!matter_thread::run([&] { ok = apply_mode(index, mode); })) {
        return "Matter is busy: try again in a moment\n";
    }
    if (!ok) {
        return mode >= MODE_PRESET && mode < MODE_MATCH ? "preset is empty or bed not ready\n" : "bed not ready\n";
    }
    return mode >= MODE_SAVE ? "saved\n" : "OK\n";
}




std::string describe() {
    char qr[64] = {};
    char manual[32] = {};
    chip::MutableCharSpan qr_span(qr);
    chip::MutableCharSpan manual_span(manual);
    const auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork);

    bool have_qr = false, have_manual = false, window_open = false;
    unsigned fabrics = 0;
    std::string beds_text;
    matter_thread::run([&] {
        have_qr = GetQRCode(qr_span, flags) == CHIP_NO_ERROR;
        have_manual = GetManualPairingCode(manual_span, flags) == CHIP_NO_ERROR;
        chip::Server& server = chip::Server::GetInstance();
        fabrics = server.GetFabricTable().FabricCount();
        window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
        for (int i = 0; i < bed_total; ++i) {
            const BedEndpoints& bed = beds[i];
            char line[240];
            std::snprintf(line, sizeof(line),
                          "bed %d '%s': endpoints device=%u head=%u legs=%u light=%u, %s, head %u%% raised, legs %u%% raised, "
                          "config status 0x%02x\n",
                          i, bed.label, bed.device_endpoint, bed.head.endpoint_id, bed.feet.endpoint_id, bed.light_endpoint,
                          bed.reachable ? "reachable" : "unreachable", (FLAT - bed.head.position) / 100,
                          (FLAT - bed.feet.position) / 100, WC::ConfigStatusGet(bed.head.endpoint_id).Raw());
            beds_text += line;
        }
    });

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
