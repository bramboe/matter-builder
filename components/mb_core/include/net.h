// Network side of a device: Wi-Fi or Thread plus a small HTTP API, so it can be
// used and updated without USB. Every request needs the device's password
// (chosen on the settings page) or a program token.
//
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// The Matter stack runs the network; net_wifi.cpp (Wi-Fi) or net_thread.cpp
// (Thread) only hands it the network set on the settings page and adds the
// HTTP API. Which one is built follows CONFIG_OPENTHREAD_ENABLED.

#pragma once

#include <string>

namespace net {

void start();
std::string describe();
bool up();  // on the network (Thread attached, or Wi-Fi with an address)

// How the network is doing, as a JSON object (in "system json"), for the
// add-on's network health: Thread (role, parent signal, route to the home
// network, SRP registration, radio counters) or Wi-Fi (signal, disconnects).
// Never the network's name, key or dataset.
std::string health_json();

}  // namespace net
