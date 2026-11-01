// What a component sees of the generic firmware: a connection (Slot) and
// the interface to add a connection type of its own (Driver). Header only;
// the generic firmware (firmware/generic/main/io.cpp) implements it.
//
// A component (components/<type> with a component.yaml) calls
// io::register_driver() from its mb_register_<type>() function, which the
// generated main/components_registry.cpp calls at start.

#pragma once

#include <cstdint>

namespace io {

enum class Type : uint8_t { none, relay, led, button, contact, motion, sht4x, ir_send, ir_receive, ble_sensor, ble_presence };

struct Slot {
    Type type;
    uint8_t pin;    // the only pin, or SDA for the SHT4x; none for Bluetooth
    uint8_t pin2;   // SCL for the SHT4x
    uint8_t flags;  // INVERTED
    char name[24];
    uint8_t data[8];  // IR: protocol (1 = NEC), address, command (little endian); Bluetooth: MAC
};
constexpr uint8_t INVERTED = 1;  // relay active low, or input active high/low swapped
constexpr uint8_t IR_NEC = 1;

// What a connection of this type is in Matter (and so in Home Assistant)
enum class Matter : uint8_t { on_off, dimmable, button, contact, occupancy, climate };

struct Driver {
    const char* type;          // as in YAML and on the page, e.g. "pulse_relay"
    const char* title;         // shown in the editors
    Matter matter;
    uint8_t pins;              // 0, 1 or 2 (the second is "pin2")
    const char* number_label;  // one setting (0-65535) in data[0..1], or nullptr
    bool (*setup)(int slot, const Slot& s);                           // false: not usable
    void (*poll)(int slot, const Slot& s, int64_t now_us);            // every 20 ms, or nullptr
    void (*output)(int slot, const Slot& s, bool on, uint8_t level);  // on_off and dimmable
};

void register_driver(const Driver& driver);

// A driver tells Matter what happened (from its poll or its own task)
void report_input(int slot, bool active);         // contact closed, occupancy
void report_press(int slot);                      // a short press of a button
void report_climate(int slot, int16_t centi_celsius, uint16_t centi_percent);
void report_output(int slot, bool on);            // an output that changed by itself

uint16_t number_of(const Slot& s);                // the setting

}  // namespace io
