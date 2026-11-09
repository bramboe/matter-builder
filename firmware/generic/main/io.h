// What is connected to the device, set on the settings page instead of in
// the firmware: up to 8 connections, each a relay, LED, button, contact,
// motion sensor, SHT4x (temperature and humidity), an infrared code to send
// or to receive (NEC), or a Bluetooth thermometer or presence tag.
//
// Changes go into a draft. "io apply" restarts with the draft as a trial;
// "io confirm" within 2 minutes keeps it. Without a confirmation, or after
// repeated crashes during the trial, the device goes back to what it had.
// Connection n is Matter endpoint n + 1, so a controller's entities stay put
// when other connections change.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mb_driver.h"

namespace io {

constexpr int SLOTS = 8;
// Groups: connections in the same group are one device in Matter (one Apple
// Home accessory, one Home Assistant device), e.g. a lamp and its button.
// The group number (1-4, 0 = none) sits in bits 4-6 of a slot's flags.
constexpr int GROUPS = 4;
constexpr uint8_t GROUP_SHIFT = 4;
constexpr uint8_t GROUP_MASK = 0x70;
inline int group_of(const Slot& s) { return (s.flags & GROUP_MASK) >> GROUP_SHIFT; }

// Slot, Type and the component interface: components/mb_driver


enum class Press : uint8_t { initial, short_release, long_press, long_release, multi_complete };

// Matter's side: set before start(), called from the polling task
struct Listener {
    void (*input)(int slot, bool active);                     // contact closed, motion seen
    void (*output)(int slot, bool on);                        // an output that changed by itself
    void (*button)(int slot, Press press, uint8_t count);     // count: presses for multi_complete
    void (*climate)(int slot, int16_t centi_celsius, uint16_t centi_percent);
};

void set_listener(const Listener& listener);

// "nec:00FF:A25D" and "AA:BB:CC:DD:EE:FF" into a slot's data, and back
bool parse_code(const char* text, uint8_t* data);
bool parse_mac(const char* text, uint8_t* data);
std::string code_text(const uint8_t* data);
std::string mac_text(const uint8_t* data);

// The factory setting from the device spec (tools/mb_generate.py): used while
// nothing has been set on the page yet. Call before start().
void set_factory(const Slot* slots, const char* name);

// Loads the connections (the draft during a trial) and starts the pins
void start();

// Bluetooth, for the connections that listen to it: started after Matter,
// sharing its stack when pairing over Bluetooth is on
bool needs_bluetooth();
void start_bluetooth(bool shared_with_pairing);
void set_ble_pairing(bool on);  // for the state: shown on the page

// The connections of this boot
const Slot& slot(int index);

// Last known state of an input (contact closed, motion seen) and of a sensor
bool input_active(int index);
bool climate(int index, int16_t& centi_celsius, uint16_t& centi_percent);

// Outputs: on/off, and the level (1-254) of an LED; "on" of an IR code sends it
void set_output(int index, bool on, uint8_t level);
bool is_momentary(int index);  // an IR code to send: "on" only for a moment
bool output_on(int index);     // the state an output is in (after power-on: as set for it)
// set_output, and Matter hears of it (for the rules on the device)
void switch_output(int index, bool on, uint8_t level);

// Every output off, before a restart
void stop_outputs();

// What a connection is in Matter, for built-in and component types alike
// (0: on/off, 1: dimmable, 2: button, 3: contact, 4: occupancy, 5: climate, -1: none)
int matter_of(int index);

// The generated registry (main/components_registry.cpp), before start()
void register_components();
// A type by its name: built-in or from a component (Type::none: unknown)
Type type_named(const char* name);

// The device's name, shown by controllers and on the page
std::string name();
// A group's name ("io group <g> <name>"); "" when none was given
std::string group_name(int group);

// The "io" command: json | set <n> <type> <pin> [<pin2>] [inverted] [options] <name> |
// set <n> ir_send|ir_receive <pin> <code> <name> | set <n> ble_sensor|ble_presence <mac> <name> |
// clear <n> | test <n> on|off | apply | confirm | revert | cancel | group <g> [<name>]
// Options: power=off|on|last|toggle and off_after=<seconds> for outputs;
// controls=<n> for a button (toggles output n), contact, motion or presence
// (output n follows it; with off_after on the output it goes off that long
// after the input went inactive). They work on the device itself, without a
// controller. group=<0-4> puts a connection in a group (0: none); without
// it a connection keeps the group it had.
std::string command(const std::vector<std::string>& args);

// The "name" command
std::string name_command(const std::vector<std::string>& args);

// The state for the settings page ("json"): {"io":{...}}
std::string state_json();

}  // namespace io
