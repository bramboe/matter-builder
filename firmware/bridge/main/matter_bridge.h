// Presents the beds as standard Matter devices behind an Aggregator. Each
// bed is one bridged device (name and Reachable) whose parts are the head and
// legs (Window Covering) and the light (On/Off Light), so Apple Home and Home
// Assistant show one device per bed.
//
// Endpoints are created before the Matter stack starts, always in the same
// order, so their IDs never change (controllers key their entities on them):
//
//   0  root node   1  aggregator
//   2  bed 0       3  head   4  legs   5  light
//   6  bed 1       7  head   8  legs   9  light
//
// Both bed slots always exist; an unconfigured or disconnected bed shows as
// not reachable. The bed device itself also carries a stop button
// (Operational State, Stop only) and a preset choice (Mode Select: flat,
// presets 1-3, match the other bed, save the current position as a preset).
//
// Covers use Matter's "percent closed": 0 % = fully raised, 100 % = flat.
// Home Assistant shows that as position 100 = raised, 0 = flat. The bed does
// not report its position, so it is estimated from the time each motor was
// driven, like the octo_bed integration does.

#pragma once

#include <string>
#include <vector>

namespace matter_bridge {

// Call after bed_link::start(). Creates the endpoints and starts Matter.
void start();

// Text for the "matter" console command: pairing codes, fabrics, endpoints.
std::string describe();

// "matter open": let another controller pair for 5 minutes (multi-admin)
std::string open_commissioning_window();

// "matter fabrics" / "matter remove <index>": the controllers paired with
// the bridge, e.g. to drop one that was removed without unpairing
std::string fabrics_text();
std::string remove_fabric(unsigned index);

// "matter json": pairing codes (with the QR code as an SVG path), the
// commissioning window, controllers and network, for the settings page
std::string matter_json();

// The "preset" console command (also used by the settings page):
//   preset list                        saved presets and positions as JSON
//   preset <bed> <flat|1|2|3|match>    move to a preset or the other bed
//   preset <bed> save <1|2|3>          store the current position
std::string preset_command(const std::vector<std::string>& args);

}  // namespace matter_bridge
