// Text commands shared by the serial console and the HTTP console.

#pragma once

#include <string>
#include <vector>

// An app-specific command, e.g. "matter" in the bridge. args[0] is the name.
using CommandHandler = std::string (*)(const std::vector<std::string>& args);

// Register before console_start(); help lists `help_line` under the built-ins.
void add_command(const char* name, const char* help_line, CommandHandler handler);

// Execute one command line and return its output.
std::string run_command(const std::string& line);

// Start the interactive serial console (USB/UART).
void console_start();
