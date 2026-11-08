// Configuration mode of a Thread device (ESP32-C6): one boot on Wi-Fi, for
// fast firmware updates and a fast first setup. Thread and Matter stay off;
// what works without Matter keeps working (whatever the app starts in
// mb_core::App::config_mode_start). The settings page and /ota run on http://<hostname>.local/ (the
// app's name, mb_core::App).
//
// The mode lasts one boot. The bridge goes back to normal (Thread) after a
// firmware update, after the given minutes without a command from the page
// (at most an hour in all), when Wi-Fi does not connect within a minute, or
// on "wifi exit". The Wi-Fi network is set on the settings page ("wifi ssid",
// "wifi password") and kept in NVS.
//
// Without a Wi-Fi network (a new bridge), or when it does not connect and
// there is no Thread network either, the bridge opens its own open network
// "<ap_prefix>-XXXX" instead: the page is then at http://192.168.4.1/ and
// phones show it as the network's sign-in page.

#pragma once

#include <string>
#include <vector>

namespace config_mode {

// True once per request: call right after NVS init. When it returns true,
// call run() instead of starting the bridge.
bool requested();

// This boot runs in configuration mode (run() was entered)
bool active();

// Wi-Fi, HTTP API and console only. Never returns.
[[noreturn]] void run();

// The "wifi" command of the normal bridge
std::string command(const std::vector<std::string>& args);

// Ask for configuration mode at the next start. Without a Wi-Fi network, or
// with `setup_ap` (recovery), it opens the setup access point. False when
// storing failed.
bool request(int minutes, bool setup_ap = false);

}  // namespace config_mode
