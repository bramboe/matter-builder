// The Matter side every app shares on its settings page: pairing codes,
// the commissioning window and the paired controllers.

#pragma once

#include <string>

namespace matter_info {

// "matter open": let another controller pair for 5 minutes (multi-admin)
std::string open_commissioning_window();

// "matter share": add an already paired device to another app (Apple Home,
// Google Home, Alexa, a second Home Assistant) with a one-time code: a new
// random passcode, valid for 10 minutes in a window of its own (Matter's
// enhanced commissioning), as Home Assistant's "Share device" does. The
// device's own pairing code stays secret. Answers JSON with the code, the QR
// code (SVG path) and the seconds left; "matter close" ends it early.
std::string share_window();
std::string close_window();

// "matter fabrics" / "matter remove <index>": the paired controllers, e.g.
// to drop one that was removed without unpairing
std::string fabrics_text();
std::string remove_fabric(unsigned index);
// Every controller: the device is unpaired (the BOOT button's menu)
std::string remove_all_fabrics();

// "matter json": pairing codes (with the QR code as an SVG path), the
// commissioning window, controllers and network, for the settings page
std::string matter_json();

// The device's endpoints are built from its settings (connections, groups,
// beds). When they come out differently than last time, the root's
// ConfigurationVersion goes up, so controllers read the structure again
// instead of keeping the old one. `signature` sums up the structure; call
// it in the Matter thread once Matter runs.
void note_structure(uint32_t signature);

}  // namespace matter_info
