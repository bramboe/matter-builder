#include "matter_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>

#include <app/reporting/reporting.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <data_model_provider/clusters/electrical_power_measurement/integration.h>

#include <app/clusters/boolean-state-server/BooleanStateCluster.h>
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>
#include <app/clusters/relative-humidity-measurement-server/CodegenIntegration.h>
#include <app/clusters/switch-server/CodegenIntegration.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/server/Server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInfoProvider.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>

#include "esp_log.h"
#include "io.h"
#include "rules.h"
#include "matter_info.h"
#include "matter_thread.h"
#include "mb_core.h"
#include "setup_code.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace matter_device {

namespace {

const char* TAG = "matter";
constexpr char HARDWARE[] = "ESP32-C6";

node_t* node = nullptr;
uint16_t endpoint_of[io::SLOTS] = {};  // 0: none
// Groups (io.h): the first connection of a group (the lowest number) is the
// device, named after the group; the others are parts of it, endpoints under
// it without a name of their own. -1: the group is not used.
int group_head[io::GROUPS + 1] = {-1, -1, -1, -1, -1};

int group_of_slot(int i) {
    const io::Slot& s = io::slot(i);
    return s.type == io::Type::none ? 0 : io::group_of(s);
}
bool is_part(int i) {
    const int g = group_of_slot(i);
    return g > 0 && group_head[g] != i;
}
// What controllers call the device of a connection: its group's name (or,
// without one, the connection's name) for a group, else the connection's name
std::string device_label(int i) {
    const int g = group_of_slot(i);
    const std::string group = g > 0 ? io::group_name(g) : "";
    return group.empty() ? io::slot(i).name : group;
}

// In a group every connection carries its own name as a fixed label: Home
// Assistant adds "ha_entitylabel" to the device name ("Bedside Lamp",
// "Bedside Button"); Apple Home shows the parts within one accessory
class PartLabels : public chip::DeviceLayer::DeviceInfoProvider {
public:
    FixedLabelIterator* IterateFixedLabel(chip::EndpointId endpoint_id) override {
        const char* value = nullptr;
        for (int i = 0; i < io::SLOTS; ++i) {
            if (endpoint_of[i] != 0 && endpoint_of[i] == endpoint_id && group_of_slot(i) > 0) value = io::slot(i).name;
        }
        return chip::Platform::New<Labels>(value);
    }
    UserLabelIterator* IterateUserLabel(chip::EndpointId) override { return nullptr; }
    SupportedLocalesIterator* IterateSupportedLocales() override { return nullptr; }
    SupportedCalendarTypesIterator* IterateSupportedCalendarTypes() override { return nullptr; }

protected:
    CHIP_ERROR SetUserLabelLength(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetUserLabelLength(chip::EndpointId, size_t&) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR SetUserLabelAt(chip::EndpointId, size_t, const UserLabelType&) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId, size_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    class Labels : public FixedLabelIterator {
    public:
        explicit Labels(const char* value) : value_(value) {}
        size_t Count() override { return value_ != nullptr ? 1 : 0; }
        bool Next(FixedLabelType& output) override {
            if (value_ == nullptr || done_) return false;
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
uint16_t rule_endpoint_of[rules::RULES] = {};  // 0: not a switch in Matter

int rule_of(uint16_t endpoint_id) {
    for (int i = 0; i < rules::RULES; ++i) {
        if (rule_endpoint_of[i] != 0 && rule_endpoint_of[i] == endpoint_id) return i;
    }
    return -1;
}

int slot_of(uint16_t endpoint_id) {
    for (int i = 0; i < io::SLOTS; ++i) {
        if (endpoint_of[i] != 0 && endpoint_of[i] == endpoint_id) return i;
    }
    return -1;
}

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

// An IR code was sent: its switch goes back off, so the next "on" sends again
void momentary_off(chip::System::Layer*, void* arg) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (endpoint_of[i] == 0) return;
    esp_matter_attr_val_t off = esp_matter_bool(false);
    attribute::update(endpoint_of[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &off);
}

// The colour Matter keeps for a colour light, with one attribute about to
// change (`changed`, `val`): the mode follows what the controller changed
io::Color stored_color(uint16_t ep, uint32_t changed = 0, const esp_matter_attr_val_t* val = nullptr) {
    auto read = [&](uint32_t id) -> esp_matter_attr_val_t {
        if (val != nullptr && id == changed) return *val;
        esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
        if (attribute_t* a = attribute::get(ep, ColorControl::Id, id)) attribute::get_val(a, &v);
        return v;
    };
    namespace cc = ColorControl::Attributes;
    io::Color c = {};
    c.hue = read(cc::CurrentHue::Id).val.u8;
    c.saturation = read(cc::CurrentSaturation::Id).val.u8;
    c.x = read(cc::CurrentX::Id).val.u16;
    c.y = read(cc::CurrentY::Id).val.u16;
    c.mireds = read(cc::ColorTemperatureMireds::Id).val.u16;
    if (changed == cc::CurrentHue::Id || changed == cc::CurrentSaturation::Id) {
        c.mode = io::Color::hue_saturation;
    } else if (changed == cc::CurrentX::Id || changed == cc::CurrentY::Id) {
        c.mode = io::Color::xy;
    } else if (changed == cc::ColorTemperatureMireds::Id) {
        c.mode = io::Color::temperature;
    } else {
        const uint8_t mode = read(cc::ColorMode::Id).val.u8;
        c.mode = mode <= io::Color::temperature ? mode : static_cast<uint8_t>(io::Color::temperature);
    }
    return c;
}

bool is_color_attribute(uint32_t id) {
    namespace cc = ColorControl::Attributes;
    return id == cc::CurrentHue::Id || id == cc::CurrentSaturation::Id || id == cc::CurrentX::Id ||
           id == cc::CurrentY::Id || id == cc::ColorTemperatureMireds::Id;
}

// Until Matter has started, its own start-up writes are not commands: after a
// power cut the On/Off server applies StartUpOnOff and switched every output
// off, over the connection's own power-on setting (io). after_start() hands
// Matter the real states and then lets writes through.
bool started = false;

// Matter writes an output: switch the pin before the value is stored
esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t* val, void*) {
    const int index = slot_of(endpoint_id);
    if (type != attribute::PRE_UPDATE || !started) {
        return ESP_OK;
    }
    // A rule's switch: the controller turns the rule on or off
    if (const int rule = rule_of(endpoint_id); rule >= 0) {
        if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) rules::set_enabled(rule, val->val.b);
        return ESP_OK;
    }
    if (index < 0) {
        return ESP_OK;
    }
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        io::set_output(index, val->val.b, 0);
        if (io::is_momentary(index) && val->val.b) {
            chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(1000), momentary_off,
                                                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
        }
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id &&
               val->val.u8 > 0) {
        esp_matter_attr_val_t on = esp_matter_invalid(nullptr);
        attribute_t* on_off = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
        const bool is_on = on_off != nullptr && attribute::get_val(on_off, &on) == ESP_OK && on.val.b;
        io::set_output(index, is_on, val->val.u8);
    } else if (cluster_id == ColorControl::Id && is_color_attribute(attribute_id) && io::matter_of(index) == 6) {
        io::set_color(index, stored_color(endpoint_id, attribute_id, val));
    }
    return ESP_OK;
}

esp_err_t on_identify(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void*) { return ESP_OK; }

void on_event(const ChipDeviceEvent* event, intptr_t) {
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete) {
        ESP_LOGI(TAG, "Commissioning complete");
    }
}

// ---------------------------------------------------------------- io → Matter
// Called in the io polling task: hand the values to the Matter thread

// Occupancy is kept by the cluster itself (esp-matter 1.6): an attribute
// write is refused, the cluster's own call reports it
void set_occupancy(uint16_t ep, bool occupied) {
    auto* cluster = esp_matter::data_model::provider::get_instance().registry().Get(
        chip::app::ConcreteClusterPath(ep, OccupancySensing::Id));
    if (cluster != nullptr) static_cast<chip::app::Clusters::OccupancySensingCluster*>(cluster)->SetOccupancy(occupied);
}

// The same for these: in esp-matter 1.6 they are code-driven clusters, an
// attribute write gets err 262 and controllers never see the value
// (esp-matter does not build every cluster's lookup: the registry finds them all)
template <typename T>
T* cluster_on(uint16_t ep, chip::ClusterId id) {
    return static_cast<T*>(esp_matter::data_model::provider::get_instance().registry().Get(chip::app::ConcreteClusterPath(ep, id)));
}

void set_contact(uint16_t ep, bool closed) {
    auto* cluster = cluster_on<BooleanStateCluster>(ep, BooleanState::Id);
    if (cluster == nullptr) {
        ESP_LOGW(TAG, "Contact on endpoint %u: no Boolean State cluster", ep);
        return;
    }
    (void) cluster->SetStateValue(closed);
}

void on_input(int index, bool active) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        const int i = static_cast<int>(arg >> 1);
        const bool on = (arg & 1) != 0;
        const uint16_t ep = endpoint_of[i];
        if (ep == 0) return;
        if (io::matter_of(i) == 3) {
            set_contact(ep, on);
        } else {
            set_occupancy(ep, on);
        }
    }, (static_cast<intptr_t>(index) << 1) | (active ? 1 : 0));
}

void on_button(int index, io::Press press, uint8_t count) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        const int i = static_cast<int>(arg & 0xFF);
        const auto press = static_cast<io::Press>((arg >> 8) & 0xFF);
        const uint8_t count = static_cast<uint8_t>((arg >> 16) & 0xFF);
        const uint16_t ep = endpoint_of[i];
        if (ep == 0) return;
        namespace ev = cluster::switch_cluster::event;
        const bool down = press == io::Press::initial || press == io::Press::long_press;
        if (auto* cluster = Switch::FindClusterOnEndpoint(ep)) (void) cluster->SetCurrentPosition(down ? 1 : 0);
        switch (press) {
            case io::Press::initial: ev::send_initial_press(ep, 1); break;
            case io::Press::long_press: ev::send_long_press(ep, 1); break;
            case io::Press::short_release: ev::send_short_release(ep, 1); break;
            case io::Press::long_release: ev::send_long_release(ep, 1); break;
            case io::Press::multi_complete: ev::send_multi_press_complete(ep, 0, count); break;
        }
    }, static_cast<intptr_t>(index) | (static_cast<intptr_t>(press) << 8) | (static_cast<intptr_t>(count) << 16));
}

// An output that changed by itself (a component, e.g. a pulse that ended)
void on_output(int index, bool on) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        const int i = static_cast<int>(arg >> 1);
        if (endpoint_of[i] == 0) return;
        esp_matter_attr_val_t value = esp_matter_bool((arg & 1) != 0);
        attribute::update(endpoint_of[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &value);
    }, (static_cast<intptr_t>(index) << 1) | (on ? 1 : 0));
}

void update_climate(int i) {
    int16_t centi_celsius = 0;
    uint16_t centi_percent = 0;
    if (endpoint_of[i] == 0 || !io::climate(i, centi_celsius, centi_percent)) return;
    if (auto* t = cluster_on<TemperatureMeasurementCluster>(endpoint_of[i], TemperatureMeasurement::Id)) {
        (void) t->SetMeasuredValue(chip::app::DataModel::MakeNullable(centi_celsius));
    }
    if (auto* h = RelativeHumidityMeasurement::FindClusterOnEndpoint(endpoint_of[i])) {
        (void) h->SetMeasuredValue(chip::app::DataModel::MakeNullable(centi_percent));
    }
}

void on_climate(int index, int16_t, uint16_t) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) { update_climate(static_cast<int>(arg)); },
                                                         index);
}

namespace epm = ElectricalPowerMeasurement;
namespace eem = ElectricalEnergyMeasurement;
namespace co2 = CarbonDioxideConcentrationMeasurement;
using chip::app::DataModel::Nullable;

constexpr int64_t MAX_POWER_MW = 360000000000LL;
constexpr int64_t MAX_ENERGY_MWH = 0x3fffffffffffffffLL;

class PulsePower : public epm::Delegate {
public:
    int slot = 0;

    epm::PowerModeEnum GetPowerMode() override { return epm::PowerModeEnum::kAc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 1; }
    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, epm::Structs::MeasurementAccuracyStruct::Type& accuracy) override {
        if (index != 0) return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        static const epm::Structs::MeasurementAccuracyRangeStruct::Type range = {0, MAX_POWER_MW};
        accuracy = {epm::MeasurementTypeEnum::kActivePower, false, 0, MAX_POWER_MW, {&range, 1}};
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, epm::Structs::MeasurementRangeStruct::Type&) override {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, epm::Structs::HarmonicMeasurementStruct::Type&) override {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, epm::Structs::HarmonicMeasurementStruct::Type&) override {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }

    Nullable<int64_t> GetActivePower() override {
        const auto mw = io::measurement(slot, 0);
        return mw < 0 ? Nullable<int64_t>() : Nullable<int64_t>(mw);
    }
    Nullable<int64_t> GetVoltage() override { return {}; }
    Nullable<int64_t> GetActiveCurrent() override { return {}; }
    Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    Nullable<int64_t> GetApparentCurrent() override { return {}; }
    Nullable<int64_t> GetReactivePower() override { return {}; }
    Nullable<int64_t> GetApparentPower() override { return {}; }
    Nullable<int64_t> GetRMSVoltage() override { return {}; }
    Nullable<int64_t> GetRMSCurrent() override { return {}; }
    Nullable<int64_t> GetRMSPower() override { return {}; }
    Nullable<int64_t> GetFrequency() override { return {}; }
    Nullable<int64_t> GetPowerFactor() override { return {}; }
    Nullable<int64_t> GetNeutralCurrent() override { return {}; }
};

PulsePower pulse_power[io::SLOTS];
std::optional<epm::Instance> power_cluster[io::SLOTS];

void update_measurement(int slot, unsigned channel) {
    const uint16_t ep = endpoint_of[slot];
    if (!ep) return;
    const int64_t value = io::measurement(slot, channel);
    if (io::matter_of(slot) == static_cast<int>(io::Matter::air_quality)) {
        nullable<float> ppm;
        if (value >= 0) ppm = static_cast<float>(value);
        auto attribute = esp_matter_nullable_float(ppm);
        attribute::update(ep, co2::Id, co2::Attributes::MeasuredValue::Id, &attribute);
    } else if (io::matter_of(slot) == static_cast<int>(io::Matter::electrical)) {
        if (channel == 0) {
            MatterReportingAttributeChangeCallback(ep, epm::Id, epm::Attributes::ActivePower::Id);
        } else if (value >= 0 && eem::GetClusterInstance(ep)) {
            eem::Structs::EnergyMeasurementStruct::Type energy = {};
            energy.energy = value;
            eem::NotifyCumulativeEnergyMeasured(ep, Nullable<decltype(energy)>(energy), {});
        }
    }
}

void on_measurement(int slot, unsigned channel) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        update_measurement(static_cast<int>(arg >> 1), arg & 1);
    }, (static_cast<intptr_t>(slot) << 1) | channel);
}

void add_energy_meter(endpoint_t* ep, int slot) {
    namespace topology = cluster::power_topology;
    namespace power = cluster::electrical_power_measurement;
    namespace energy = cluster::electrical_energy_measurement;
    endpoint::electrical_sensor::config_t config;
    config.power_topology.feature_flags = topology::feature::node_topology::get_id();
    config.with_electrical_power_measurement().feature_flags = power::feature::alternating_current::get_id();
    config.with_electrical_energy_measurement().feature_flags =
        energy::feature::imported_energy::get_id() | energy::feature::cumulative_energy::get_id();
    if (endpoint::electrical_sensor::add(ep, &config) != ESP_OK) return;
    pulse_power[slot].slot = slot;
    power_cluster[slot].emplace(endpoint::get_id(ep), pulse_power[slot], epm::Feature::kAlternatingCurrent,
                               chip::BitMask<epm::OptionalAttributes>());
    if (power_cluster[slot]->Init() != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Connection %d: could not register power measurements", slot);
        power_cluster[slot].reset();
    }
}

// ---------------------------------------------------------------- endpoints

// A momentary switch with short, long and double presses
void add_generic_switch(endpoint_t* ep) {
    // The Switch cluster needs exactly one kind (momentary here) when
    // it is created; multi-press needs release first, so it comes after
    namespace feature = cluster::switch_cluster::feature;
    endpoint::generic_switch::config_t config;
    config.switch_cluster.feature_flags = feature::momentary_switch::get_id() |
                                          feature::momentary_switch_release::get_id() |
                                          feature::momentary_switch_long_press::get_id();
    endpoint::generic_switch::add(ep, &config);
    feature::momentary_switch_multi_press::config_t multi;
    multi.multi_press_max = 2;
    feature::momentary_switch_multi_press::add(cluster::get(ep, Switch::Id), &multi);
}

void add_occupancy_sensor(endpoint_t* ep, int i) {
    endpoint::occupancy_sensor::config_t config;
    config.occupancy_sensing.occupancy = io::input_active(i) ? 1 : 0;
    config.occupancy_sensing.occupancy_sensor_type = 0;         // PIR
    config.occupancy_sensing.occupancy_sensor_type_bitmap = 1;  // PIR
    config.occupancy_sensing.feature_flags = cluster::occupancy_sensing::feature::passive_infrared::get_id();
    endpoint::occupancy_sensor::add(ep, &config);
}

void add_climate_sensor(endpoint_t* ep) {
    endpoint::temperature_sensor::config_t temperature;
    endpoint::temperature_sensor::add(ep, &temperature);
    endpoint::humidity_sensor::config_t humidity;
    endpoint::humidity_sensor::add(ep, &humidity);
}

void add_co2_sensor(endpoint_t* ep) {
    endpoint::air_quality_sensor::config_t air;
    endpoint::air_quality_sensor::add(ep, &air);
    namespace concentration = cluster::carbon_dioxide_concentration_measurement;
    concentration::config_t config;
    config.measurement_medium = static_cast<uint8_t>(co2::MeasurementMediumEnum::kAir);
    config.feature_flags = concentration::feature::numeric_measurement::get_id();
    config.features.numeric_measurement.min_measured_value = 0.0f;
    config.features.numeric_measurement.max_measured_value = 40000.0f;
    config.features.numeric_measurement.measurement_unit = static_cast<uint8_t>(co2::MeasurementUnitEnum::kPpm);
    concentration::create(ep, &config, CLUSTER_FLAG_SERVER);
    add_climate_sensor(ep);
}

// An Extended Color Light: on/off, level, colour temperature (2000-6500 K),
// xy and hue/saturation, so every controller finds the colour model it uses
void add_color_light(endpoint_t* ep) {
    endpoint::extended_color_light::config_t config;
    config.color_control.color_mode = static_cast<uint8_t>(ColorControl::ColorMode::kColorTemperature);
    config.color_control.enhanced_color_mode = static_cast<uint8_t>(ColorControl::ColorMode::kColorTemperature);
    config.color_control_color_temperature.color_temperature_mireds = 370;
    config.color_control_color_temperature.color_temp_physical_min_mireds = 153;
    config.color_control_color_temperature.color_temp_physical_max_mireds = 500;
    config.color_control_color_temperature.couple_color_temp_to_level_min_mireds = 153;
    config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr;  // the last one
    endpoint::extended_color_light::add(ep, &config);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation;
    cluster::color_control::feature::hue_saturation::add(cluster::get(ep, ColorControl::Id), &hue_saturation);
}

// The device type of a connection on its endpoint (io::matter_of: built-in
// and component types alike)
void add_device_type(endpoint_t* ep, int i) {
    switch (io::matter_of(i)) {
        case 0: {
            endpoint::on_off_plug_in_unit::config_t config;
            endpoint::on_off_plug_in_unit::add(ep, &config);
            break;
        }
        case 1: {
            endpoint::dimmable_light::config_t config;
            endpoint::dimmable_light::add(ep, &config);
            break;
        }
        case 2:
            add_generic_switch(ep);
            break;
        case 3: {
            endpoint::contact_sensor::config_t config;
            config.boolean_state.state_value = io::input_active(i);
            endpoint::contact_sensor::add(ep, &config);
            break;
        }
        case 4:
            add_occupancy_sensor(ep, i);
            break;
        case 5:
            add_climate_sensor(ep);
            break;
        case 6:
            add_color_light(ep);
            break;
        case static_cast<int>(io::Matter::air_quality):
            add_co2_sensor(ep);
            break;
        case static_cast<int>(io::Matter::electrical):
            add_energy_meter(ep, i);
            break;
        default:
            break;
    }
}

// Every connection is a bridged device behind an aggregator (endpoint 1),
// so a controller shows one device per connection and removing one leaves
// the others alone. All eight are created before Matter starts, in order, so
// connection n is always endpoint n + 2; unused ones are disabled once Matter
// runs, so controllers never see them.
endpoint_t* slot_endpoint[io::SLOTS] = {};
// After them one endpoint per rule, the same way: a rule shown in Matter is
// a switch that turns the rule on and off, at a fixed endpoint
endpoint_t* rule_endpoint[rules::RULES] = {};
// As many rule endpoints as the endpoint table holds next to the root, the
// aggregator and the connections: all 12 on the C6, 2 on the classic ESP32,
// whose memory does not hold more (it ran out starting Matter with 12)
constexpr int RULE_ENDPOINTS =
    std::min(rules::RULES, CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT - 2 - io::SLOTS);

void create_endpoints() {
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t* aggregator = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    for (int i = 0; i < io::SLOTS; ++i) {
        const int g = group_of_slot(i);
        if (g > 0 && group_head[g] < 0) group_head[g] = i;
    }
    for (int i = 0; i < io::SLOTS; ++i) {
        // A part of a group: a plain endpoint under its group's first connection
        const bool part = is_part(i);
        endpoint_t* ep = nullptr;
        if (part) {
            ep = endpoint::create(node, ENDPOINT_FLAG_BRIDGE, nullptr);
            if (ep != nullptr) {
                cluster::descriptor::config_t descriptor;
                cluster::descriptor::create(ep, &descriptor, CLUSTER_FLAG_SERVER);
            }
        } else {
            endpoint::bridged_node::config_t config;
            config.bridged_device_basic_information.reachable = true;
            ep = endpoint::bridged_node::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
        }
        slot_endpoint[i] = ep;
        if (ep == nullptr) {
            ESP_LOGE(TAG, "Connection %d: no endpoint", i);
            continue;
        }
        endpoint::set_parent_endpoint(ep, part ? slot_endpoint[group_head[group_of_slot(i)]] : aggregator);
        const io::Slot& s = io::slot(i);
        if (s.type == io::Type::none) {
            continue;
        }
        if (!part) {
            // The device name in Home Assistant and Apple Home: the connection's, or its group's
            const std::string label = device_label(i);
            namespace bdbi = cluster::bridged_device_basic_information::attribute;
            cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
            bdbi::create_node_label(info, const_cast<char*>(label.c_str()), label.size());
            bdbi::create_vendor_name(info, const_cast<char*>(mb_core::app().vendor_name),
                                     std::strlen(mb_core::app().vendor_name));
        }
        if (group_of_slot(i) > 0) {
            cluster::fixed_label::config_t labels;
            cluster::fixed_label::create(ep, &labels, CLUSTER_FLAG_SERVER);
        }
        add_device_type(ep, i);
        endpoint_of[i] = endpoint::get_id(ep);
        ESP_LOGI(TAG, "Connection %d '%s' on endpoint %u", i, s.name, endpoint_of[i]);
    }
    for (int i = 0; i < RULE_ENDPOINTS; ++i) {
        endpoint::bridged_node::config_t config;
        config.bridged_device_basic_information.reachable = true;
        endpoint_t* ep = endpoint::bridged_node::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
        rule_endpoint[i] = ep;
        if (ep == nullptr) continue;
        endpoint::set_parent_endpoint(ep, aggregator);
        if (!rules::exposed(i)) continue;
        namespace bdbi = cluster::bridged_device_basic_information::attribute;
        cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
        const char* label = rules::name(i);
        bdbi::create_node_label(info, const_cast<char*>(label), std::strlen(label));
        bdbi::create_vendor_name(info, const_cast<char*>(mb_core::app().vendor_name),
                                 std::strlen(mb_core::app().vendor_name));
        endpoint::on_off_plug_in_unit::config_t plug;
        plug.on_off.on_off = rules::enabled(i);
        endpoint::on_off_plug_in_unit::add(ep, &plug);
        rule_endpoint_of[i] = endpoint::get_id(ep);
        ESP_LOGI(TAG, "Rule %d '%s' on endpoint %u", i, label, rule_endpoint_of[i]);
    }
}

// A rule turned on or off on the device (another rule, the page): its switch follows
void on_rule_enabled(int rule, bool on) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        const int i = static_cast<int>(arg >> 1);
        if (i >= rules::RULES || rule_endpoint_of[i] == 0) return;
        esp_matter_attr_val_t value = esp_matter_bool((arg & 1) != 0);
        attribute::update(rule_endpoint_of[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &value);
    }, (static_cast<intptr_t>(rule) << 1) | (on ? 1 : 0));
}

void after_start() {
    for (int i = 0; i < io::SLOTS; ++i) {
        if (slot_endpoint[i] != nullptr && endpoint_of[i] == 0) {
            endpoint::disable(slot_endpoint[i]);
        }
    }
    for (int i = 0; i < rules::RULES; ++i) {
        if (rule_endpoint[i] != nullptr && rule_endpoint_of[i] == 0) {
            endpoint::disable(rule_endpoint[i]);
        }
    }
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        // The root NodeLabel: what controllers call the device
        const std::string name = io::name();
        esp_matter_attr_val_t label = esp_matter_char_str(const_cast<char*>(name.c_str()), name.size());
        attribute::update(0, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id, &label);
        // The structure (types, groups, rule switches): when it changed,
        // controllers are told to read it again
        uint32_t signature = 2166136261u;
        for (int i = 0; i < io::SLOTS; ++i) {
            signature = (signature ^ (static_cast<uint32_t>(io::slot(i).type) * 8 + group_of_slot(i))) * 16777619u;
        }
        for (int i = 0; i < rules::RULES; ++i) signature = (signature ^ (rule_endpoint_of[i] != 0)) * 16777619u;
        matter_info::note_structure(signature);
        for (int i = 0; i < io::SLOTS; ++i) {
            if (endpoint_of[i] == 0) continue;
            // Stored names come back at start; the connection's (or group's) name wins
            if (!is_part(i)) {
                const std::string text = device_label(i);
                esp_matter_attr_val_t device_name = esp_matter_char_str(const_cast<char*>(text.c_str()), text.size());
                attribute::update(endpoint_of[i], BridgedDeviceBasicInformation::Id,
                                  BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &device_name);
            }
            const int kind = io::matter_of(i);
            if (kind == 5) update_climate(i);
            // The output as it really is after power-on (Matter keeps its own
            // stored value, which may differ): off, on, or as set for it
            if ((kind == 0 || kind == 1 || kind == 6) && !io::is_momentary(i)) {
                esp_matter_attr_val_t on = esp_matter_bool(io::output_on(i));
                attribute::update(endpoint_of[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
            }
            // A colour light comes back in the colour and brightness Matter kept
            if (kind == 6) {
                io::set_color(i, stored_color(endpoint_of[i]));
                esp_matter_attr_val_t level = esp_matter_invalid(nullptr);
                attribute_t* a = attribute::get(endpoint_of[i], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
                if (a != nullptr && attribute::get_val(a, &level) == ESP_OK && level.val.u8 > 0) {
                    io::set_output(i, io::output_on(i), level.val.u8);
                }
            }
            if (kind == static_cast<int>(io::Matter::air_quality)) {
                update_measurement(i, 0);
                update_climate(i);
            } else if (kind == static_cast<int>(io::Matter::electrical)) {
                static const eem::Structs::MeasurementAccuracyRangeStruct::Type range = {0, MAX_ENERGY_MWH};
                const eem::Structs::MeasurementAccuracyStruct::Type accuracy = {
                    eem::MeasurementTypeEnum::kElectricalEnergy, true, 0, MAX_ENERGY_MWH, {&range, 1}
                };
                (void) eem::SetMeasurementAccuracy(endpoint_of[i], accuracy);
                update_measurement(i, 1);
            }
            // Inputs may have changed before Matter listened (Bluetooth starts first)
            if (kind == 3) {
                set_contact(endpoint_of[i], io::input_active(i));
            } else if (kind == 4) {
                set_occupancy(endpoint_of[i], io::input_active(i));
            }
        }
        for (int i = 0; i < rules::RULES; ++i) {
            if (rule_endpoint_of[i] == 0) continue;
            const char* label = rules::name(i);
            esp_matter_attr_val_t rule_name = esp_matter_char_str(const_cast<char*>(label), std::strlen(label));
            attribute::update(rule_endpoint_of[i], BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &rule_name);
            esp_matter_attr_val_t on = esp_matter_bool(rules::enabled(i));
            attribute::update(rule_endpoint_of[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
        }
        started = true;  // from now on a write is a command
    });
}

}  // namespace

std::string endpoints_text() {
    std::string out;
    matter_thread::run([&] {
        for (endpoint_t* ep = endpoint::get_first(node); ep != nullptr; ep = endpoint::get_next(ep)) {
            int clusters = 0;
            for (cluster_t* c = cluster::get_first(ep); c != nullptr; c = cluster::get_next(c)) ++clusters;
            char line[96];
            std::snprintf(line, sizeof(line), "endpoint %u: %s, parent %u, %u device types, %d clusters\n",
                          endpoint::get_id(ep), endpoint::is_enabled(ep) ? "enabled" : "disabled",
                          endpoint::get_parent_endpoint_id(ep), static_cast<unsigned>(endpoint::get_device_type_count(ep)),
                          clusters);
            out += line;
        }
    });
    return out;
}

void start() {
    node::config_t node_config;
    node = node::create(&node_config, on_attribute, on_identify);
    io::Listener listener = {};
    listener.input = on_input;
    listener.button = on_button;
    listener.climate = on_climate;
    listener.measurement = on_measurement;
    listener.output = on_output;
    io::set_listener(listener);
    rules::Listener rule_listener = {};
    rule_listener.enabled = on_rule_enabled;
    rules::set_listener(rule_listener);
    esp_matter::set_custom_device_instance_info_provider(&device_info);
    esp_matter::set_custom_device_info_provider(&part_labels);
    create_endpoints();
    setup_code::install();
    ESP_ERROR_CHECK(esp_matter::start(on_event));
    after_start();
}

}  // namespace matter_device
