// Keeps a network-only install recoverable.
//
// The device has two firmware slots. After the first network install the
// other slot still holds ESPHome. A firmware image that has never reached the
// network is "unconfirmed"; while unconfirmed it goes back to the other slot
// when it
//   * crashes 3 times within its first minute of uptime, or
//   * has no working network (Wi-Fi + HTTP console) 10 minutes after boot.
// Once it has been online, the image is confirmed (stored in NVS by its ELF
// SHA-256) and later reboots or Wi-Fi outages never trigger a revert.

#pragma once

#include <string>

namespace boot_guard {

// Call first thing in app_main (after NVS init).
void early();

// Call once the network console is reachable.
void network_ok();

// Point the bootloader at the other slot if it holds a valid image. On
// success `info` describes that image, otherwise why it is not possible.
bool select_other_slot(std::string& info);

std::string describe();

}  // namespace boot_guard
