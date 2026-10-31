// Configuration mode of a Thread bridge (ESP32-C6): one boot on Wi-Fi, for
// fast firmware updates and a fast first setup. Thread and Matter stay off;
// BLE stays on, so beds can be added and calibrated (presets need Matter). The
// settings page and /ota run on http://octo-bridge.local/.
//
// The mode lasts one boot. The bridge goes back to normal (Thread) after a
// firmware update, after the given minutes without a command from the page
// (at most an hour in all), when Wi-Fi does not connect within a minute, or
// on "wifi exit". The Wi-Fi network is set on the settings page ("wifi ssid",
// "wifi password") and kept in NVS.

#pragma once

#include <string>
#include <vector>

namespace config_mode {

// True once per request: call right after NVS init. When it returns true,
// call run() instead of starting the bridge.
bool requested();

// Wi-Fi, HTTP API and console only. Never returns.
[[noreturn]] void run();

// The "wifi" command of the normal bridge
std::string command(const std::vector<std::string>& args);

}  // namespace config_mode
