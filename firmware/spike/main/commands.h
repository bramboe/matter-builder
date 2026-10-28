// Text commands shared by the serial console and the HTTP console.

#pragma once

#include <string>

// Execute one command line and return its output.
std::string run_command(const std::string& line);

// Start the interactive serial console (USB/UART).
void console_start();
