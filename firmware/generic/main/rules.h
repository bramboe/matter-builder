// Rules on the device itself: "when <trigger> (only if <condition>) then
// <steps>", like HAA's actions or ESPHome's automations, but as data set on
// the page or in the add-on, never compiled. They run without a controller.
//
// When:    press <n> [count] | long <n> | active <n> | inactive <n> | on <n> |
//          off <n> | above <n> <centi °C> | below ... | humid_above <n> <centi %> |
//          humid_below ... | time <minute of day> [days] | boot
// Only if: active <n> | inactive <n> | on <n> | off <n> | above/below <n> <centi °C> |
//          humid_above/humid_below <n> <centi %> | time <from minute> <to minute>
// Then:    on <n> | off <n> | toggle <n> | level <n> <1-254> | wait <seconds> |
//          enable <rule> | disable <rule> | toggle_rule <rule>   (at most 6 steps)
//
// A rule that is triggered again while it waits starts over (a motion light
// stays on while there is motion). Each rule can be a switch in Matter, so a
// controller turns it on and off ("motion light off at night") while the
// rule itself keeps running on the device.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rules {

constexpr int RULES = 12;

// Matter's side: a rule turned on or off by another rule or on the page
struct Listener {
    void (*enabled)(int rule, bool on);
};
void set_listener(const Listener& listener);

// The rules of the device spec ("<id> <rule set options>"), used while none are
// stored yet. Call before load().
void set_factory(const char* const* options, int count);

// Loads the rules (before Matter creates its endpoints) and starts the task
void load();
void start();

// From io: what happened (any task)
void on_press(int slot, uint8_t count);  // a short press, count presses in a row
void on_long(int slot);
void on_input(int slot, bool active);
void on_output(int slot, bool on);
void on_climate(int slot, int16_t centi_celsius, uint16_t centi_percent);

// For Matter: a rule's switch
bool exposed(int rule);  // shown in Matter (from the last start)
bool enabled(int rule);
const char* name(int rule);
void set_enabled(int rule, bool on);  // from Matter: no echo back

// The "rule" command: json | set <n> <options> | clear <n> | enable <n> on|off | run <n>
std::string command(const std::vector<std::string>& args);
// The "time" command: (state) | set <unix seconds> | zone <POSIX TZ>
std::string time_command(const std::vector<std::string>& args);

}  // namespace rules
