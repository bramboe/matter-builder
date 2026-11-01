// Infrared on the ESP32-C6's RMT peripheral: sending and receiving NEC codes
// (the most common remote control protocol), as 16-bit address and 16-bit
// command like ESPHome writes them ("nec:00FF:A25D"). The C6 has two send
// and two receive channels, so at most two pins of each.

#pragma once

#include <cstdint>

namespace ir {

constexpr int MAX_PINS = 2;

// A pin with an IR LED (through a transistor), 38 kHz carrier
bool start_send(int pin);
bool send_nec(int pin, uint16_t address, uint16_t command);

// A pin with an IR receiver (TSOP-style, output low while it sees light);
// every decoded code goes to on_code, from the receive task
using CodeHandler = void (*)(int pin, uint16_t address, uint16_t command);
bool start_receive(int pin, CodeHandler on_code);

}  // namespace ir
