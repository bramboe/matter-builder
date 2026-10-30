// The bridge's own pairing code. On first start a random passcode,
// discriminator and salt are generated and kept in NVS, so every bridge has
// a code of its own instead of the well-known Matter test code. Existing
// pairings are not affected: the code is only used to pair a controller.

#pragma once

#include <platform/CommissionableDataProvider.h>

namespace setup_code {

// Call before esp_matter::start().
void install();

}  // namespace setup_code
