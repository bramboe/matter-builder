// The connections (io.h) as standard Matter devices: each one a bridged
// device behind an aggregator, so controllers show one device per connection,
// named after it:
//
//   0  root node      1  aggregator      n + 2  connection n (0-7)
//
//   relay    On/Off Plug-in Unit         contact  Contact Sensor
//   LED      Dimmable Light              motion   Occupancy Sensor (PIR)
//   button   Generic Switch (momentary:  sht4x    Temperature Sensor and
//            press, long press, double)           Humidity Sensor
//
// All endpoints are created before Matter starts, in order, so the numbers
// never shift; unused ones are disabled and invisible to controllers.
// Removing connection 2 leaves the other devices as they were.

#pragma once

#include <string>

namespace matter_device {

// Call after io::start(). Starts Matter and creates the endpoints.
void start();

// "matter endpoints": what esp_matter holds per endpoint, for diagnosis
std::string endpoints_text();

}  // namespace matter_device
