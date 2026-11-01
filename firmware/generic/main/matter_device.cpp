#include "matter_device.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>

#include "esp_log.h"
#include "io.h"
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

// Matter writes an output: switch the pin before the value is stored
esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t* val, void*) {
    const int index = slot_of(endpoint_id);
    if (type != attribute::PRE_UPDATE || index < 0) {
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

void on_input(int index, bool active) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        const int i = static_cast<int>(arg >> 1);
        const bool on = (arg & 1) != 0;
        const uint16_t ep = endpoint_of[i];
        if (ep == 0) return;
        if (io::matter_of(i) == 3) {
            esp_matter_attr_val_t value = esp_matter_bool(on);
            attribute::update(ep, BooleanState::Id, BooleanState::Attributes::StateValue::Id, &value);
        } else {
            esp_matter_attr_val_t value = esp_matter_bitmap8(on ? 1 : 0);
            attribute::update(ep, OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id, &value);
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
        esp_matter_attr_val_t position = esp_matter_uint8(down ? 1 : 0);
        attribute::update(ep, Switch::Id, Switch::Attributes::CurrentPosition::Id, &position);
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
    esp_matter_attr_val_t temperature = esp_matter_nullable_int16(centi_celsius);
    attribute::update(endpoint_of[i], TemperatureMeasurement::Id,
                      TemperatureMeasurement::Attributes::MeasuredValue::Id, &temperature);
    esp_matter_attr_val_t humidity = esp_matter_nullable_uint16(centi_percent);
    attribute::update(endpoint_of[i], RelativeHumidityMeasurement::Id,
                      RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &humidity);
}

void on_climate(int index, int16_t, uint16_t) {
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) { update_climate(static_cast<int>(arg)); },
                                                         index);
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

void create_endpoints() {
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t* aggregator = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    for (int i = 0; i < io::SLOTS; ++i) {
        endpoint::bridged_node::config_t config;
        config.bridged_device_basic_information.reachable = true;
        endpoint_t* ep = endpoint::bridged_node::create(node, &config, ENDPOINT_FLAG_BRIDGE, nullptr);
        slot_endpoint[i] = ep;
        if (ep == nullptr) {
            ESP_LOGE(TAG, "Connection %d: no endpoint", i);
            continue;
        }
        endpoint::set_parent_endpoint(ep, aggregator);
        const io::Slot& s = io::slot(i);
        if (s.type == io::Type::none) {
            continue;
        }
        // The connection's name is the device name in Home Assistant and Apple Home
        namespace bdbi = cluster::bridged_device_basic_information::attribute;
        cluster_t* info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
        bdbi::create_node_label(info, const_cast<char*>(s.name), std::strlen(s.name));
        bdbi::create_vendor_name(info, const_cast<char*>(mb_core::app().vendor_name),
                                 std::strlen(mb_core::app().vendor_name));
        add_device_type(ep, i);
        endpoint_of[i] = endpoint::get_id(ep);
        ESP_LOGI(TAG, "Connection %d '%s' on endpoint %u", i, s.name, endpoint_of[i]);
    }
}

void after_start() {
    for (int i = 0; i < io::SLOTS; ++i) {
        if (slot_endpoint[i] != nullptr && endpoint_of[i] == 0) {
            endpoint::disable(slot_endpoint[i]);
        }
    }
    (void) chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        // The root NodeLabel: what controllers call the device
        const std::string name = io::name();
        esp_matter_attr_val_t label = esp_matter_char_str(const_cast<char*>(name.c_str()), name.size());
        attribute::update(0, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id, &label);
        for (int i = 0; i < io::SLOTS; ++i) {
            if (endpoint_of[i] == 0) continue;
            // Stored names come back at start; the connection's name wins
            const char* text = io::slot(i).name;
            esp_matter_attr_val_t device_name = esp_matter_char_str(const_cast<char*>(text), std::strlen(text));
            attribute::update(endpoint_of[i], BridgedDeviceBasicInformation::Id,
                              BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &device_name);
            const int kind = io::matter_of(i);
            if (kind == 5) update_climate(i);
            // Inputs may have changed before Matter listened (Bluetooth starts first)
            if (kind == 3) {
                esp_matter_attr_val_t value = esp_matter_bool(io::input_active(i));
                attribute::update(endpoint_of[i], BooleanState::Id, BooleanState::Attributes::StateValue::Id, &value);
            } else if (kind == 4) {
                esp_matter_attr_val_t value = esp_matter_bitmap8(io::input_active(i) ? 1 : 0);
                attribute::update(endpoint_of[i], OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id, &value);
            }
        }
    });
}

}  // namespace

std::string endpoints_text() {
    std::string out;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
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
    return out;
}

void start() {
    node::config_t node_config;
    node = node::create(&node_config, on_attribute, on_identify);
    io::Listener listener = {};
    listener.input = on_input;
    listener.button = on_button;
    listener.climate = on_climate;
    listener.output = on_output;
    io::set_listener(listener);
    esp_matter::set_custom_device_instance_info_provider(&device_info);
    create_endpoints();
    setup_code::install();
    ESP_ERROR_CHECK(esp_matter::start(on_event));
    after_start();
}

}  // namespace matter_device
