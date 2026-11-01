// The Thread network of the device (ESP32-C6): the operational dataset from
// Home Assistant, pasted on the settings page ("thread dataset <hex>") and
// kept in NVS. A dataset built into the app (mb_core::App, e.g. from a
// developer's secrets.h) is used only when none was set there. A new
// dataset is joined at the next start.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thread_dataset {

// The dataset to join, as TLVs. Returns its length, 0 when there is none;
// `source` is "page", "secrets" or "none".
size_t wanted(uint8_t* tlvs, size_t max, const char** source);

// The "thread" command: dataset <hex> | clear | json | (status)
std::string command(const std::vector<std::string>& args);

}  // namespace thread_dataset
