// The Matter side every app shares on its settings page: pairing codes,
// the commissioning window and the paired controllers.

#pragma once

#include <string>

namespace matter_info {

// "matter open": let another controller pair for 5 minutes (multi-admin)
std::string open_commissioning_window();

// "matter fabrics" / "matter remove <index>": the paired controllers, e.g.
// to drop one that was removed without unpairing
std::string fabrics_text();
std::string remove_fabric(unsigned index);

// "matter json": pairing codes (with the QR code as an SVG path), the
// commissioning window, controllers and network, for the settings page
std::string matter_json();

}  // namespace matter_info
