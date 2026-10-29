// Presents the beds as standard Matter devices behind an Aggregator.
//
// Endpoints are created before the Matter stack starts, always in the same
// order, so their IDs never change (Home Assistant keys its entities on them):
//
//   0  root node             1  aggregator
//   2  bed 0 head (cover)    3  bed 0 feet (cover)    4  bed 0 light
//   5  bed 1 head (cover)    6  bed 1 feet (cover)    7  bed 1 light
//
// Both bed slots always exist; an unconfigured or disconnected bed shows as
// not reachable.
//
// Covers use Matter's "percent closed": 0 % = fully raised, 100 % = flat.
// Home Assistant shows that as position 100 = raised, 0 = flat. The bed does
// not report its position, so it is estimated from the drive time, like the
// octo_bed integration does.

#pragma once

#include <string>

namespace matter_bridge {

// Call after bed_link::start(). Creates the endpoints and starts Matter.
void start();

// Text for the "matter" console command: pairing codes, fabrics, endpoints.
std::string describe();

}  // namespace matter_bridge
