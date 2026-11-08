#pragma once

#include <cstdint>
#include <string>

// The device's log, also kept in RAM, so the add-on and the page can show it
// without a USB cable. Lines are copied as they are printed; the oldest go
// once the buffer is full.
namespace log_buffer {

// Starts copying the log (called once, early)
void start();

// "log <next> <lost> <boot>\n" and then the log from byte `from` on, at
// most `max` bytes. <next> is where the following call continues; <lost> is
// 1 when lines before what is returned had already gone; <boot> changes when
// the device restarts (start again from 0 then).
std::string read(uint32_t from, size_t max);

}  // namespace log_buffer
