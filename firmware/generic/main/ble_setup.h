#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ble_setup {

enum class Type { pair, discover, delay, subscribe, write, read, wait };

struct Step {
    Type type;
    std::string name, service, characteristic, secret;
    int ms = 3000;
    bool digits = false, indicate = false, checksum = false;
    int frame = -1, escape_byte = -1;
    std::vector<uint8_t> bytes, expect, reject, escapes;
};

bool parse(const std::string& json, std::vector<Step>& steps);
bool encode(const Step& step, const std::string& secret, std::vector<uint8_t>& packet);

}  // namespace ble_setup
