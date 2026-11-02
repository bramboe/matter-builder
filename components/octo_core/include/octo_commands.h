// The bed commands of the console and the settings page (status, bed, move,
// calibrate, ...), registered with mb_core's command registry.

#pragma once

#include <string>
#include <vector>

namespace octo_commands {

// Call before console_start()
void register_all();

// The beds' state for the settings page ("json")
std::string state_json();

// The Matter endpoints exist: changing the number of beds now restarts
void matter_started();

// Stop every motor: before a restart or a firmware update
void stop_beds();

}  // namespace octo_commands
