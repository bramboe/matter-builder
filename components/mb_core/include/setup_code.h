// The bridge's own pairing code. On first start a random passcode,
// discriminator and salt are generated and kept in NVS, so every bridge has
// a code of its own instead of the well-known Matter test code. Existing
// pairings are not affected: the code is only used to pair a controller.

#pragma once

#include <string>

#include <platform/CommissionableDataProvider.h>

namespace setup_code {

// Call before esp_matter::start().
void install();

// The QR text and manual code, without Matter running (configuration mode)
bool codes(std::string& qr, std::string& manual);

// The QR text and manual code for another passcode and discriminator: a
// one-time code to share an already paired device (matter_info::share_window)
bool codes_for(uint32_t passcode, uint16_t discriminator, std::string& qr, std::string& manual);

// The QR code as an SVG path of `size` modules (0 when it failed)
std::string qr_svg_path(const char* qr, int& size);

}  // namespace setup_code
