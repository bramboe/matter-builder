// What is connected to the device, set on the settings page instead of in
// the firmware: up to 8 connections, each a relay, LED, button, contact,
// motion sensor or SHT4x (temperature and humidity).
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

namespace io {

constexpr int SLOTS = 8;

enum class Type : uint8_t { none, relay, led, button, contact, motion, sht4x };

struct Slot {
    Type type;
    uint8_t pin;    // the only pin, or SDA for the SHT4x
    uint8_t pin2;   // SCL for the SHT4x
    uint8_t flags;  // INVERTED
    char name[24];
};
constexpr uint8_t INVERTED = 1;  // relay active low, or input active high/low swapped

enum class Press : uint8_t { initial, short_release, long_press, long_release, multi_complete };

// Matter's side: set before start(), called from the polling task
struct Listener {
    void (*input)(int slot, bool active);                     // contact closed, motion seen
    void (*button)(int slot, Press press, uint8_t count);     // count: presses for multi_complete
    void (*climate)(int slot, int16_t centi_celsius, uint16_t centi_percent);
};

void set_listener(const Listener& listener);

// Loads the connections (the draft during a trial) and starts the pins
void start();

// The connections of this boot
const Slot& slot(int index);

// Last known state of an input (contact closed, motion seen) and of a sensor
bool input_active(int index);
bool climate(int index, int16_t& centi_celsius, uint16_t& centi_percent);

// Outputs: on/off, and the level (1-254) of an LED
void set_output(int index, bool on, uint8_t level);

// Every output off, before a restart
void stop_outputs();

// The device's name, shown by controllers and on the page
std::string name();

// The "io" command: json | set <n> <type> <pin> [<pin2>] [inverted] <name> |
// clear <n> | test <n> on|off | apply | confirm | revert | cancel
std::string command(const std::vector<std::string>& args);

// The "name" command
std::string name_command(const std::vector<std::string>& args);

// The state for the settings page ("json"): {"io":{...}}
std::string state_json();

}  // namespace io
