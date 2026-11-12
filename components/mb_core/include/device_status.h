// What state the device is in, for the status LED, the page and the add-on:
//
//   setup       its own setup network is open (configuration over Wi-Fi)
//   connecting  no network yet (joining Wi-Fi or Thread)
//   unpaired    on the network, no Matter controller paired
//   paired      paired, but no controller is connected right now (what Apple
//               Home shows as "No Response")
//   connected   at least one controller has a live connection
//
// A controller counts as connected while it holds a subscription: that is
// how Home Assistant, Apple Home and Google Home stay in touch. Per
// controller it is kept since when it is connected, or when it last was.
//
// The status LED (optional: "led set <pin>") shows the state at a glance:
//
//   setup       two quick blinks, every second
//   connecting  1 s on, 1 s off
//   unpaired    two blinks every 3 s
//   paired      on, with two short dips every 3 s
//   connected   on (or off with "quiet": no light when all is well)
//
// and the BOOT button's menu while that is open (program_access.cpp).

#pragma once

#include <string>
#include <vector>

namespace device_status {

enum class State { starting, setup, connecting, unpaired, paired, connected };

// The LED from the stored setting and the ticker; in every mode, early
void start();

State state();
const char* state_name();

// Configuration mode tells where it is
void set_setup(bool own_network);

// One controller (Matter fabric index): connected now, and for how many
// seconds, or how long ago it last was (-1: not since this start)
struct Controller {
    bool connected;
    long since_s;
    long last_seen_s;
};
Controller controller(unsigned fabric_index);

// {"state":"connected","fabrics":2,"connected":1}
std::string json();

// The BOOT button's menu on the LED: action n as n blinks (0: back to the
// state), and a fast flicker while the button is held long enough to act
void show_menu(int action);
void show_hold(bool holding, bool off = false);

// "led": json | set <pin>|none [inverted] [quiet]
std::string led_command(const std::vector<std::string>& args);
int led_pin();  // -1: none

}  // namespace device_status
