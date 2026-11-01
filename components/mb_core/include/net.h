// Network side of the bridge: Wi-Fi or Thread plus a small HTTP API, so it can be
// used and updated without USB. Every request needs the header
// "X-Octo-Token: <token from secrets.h>".
//
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// The Matter stack runs the network; net.cpp (Wi-Fi, classic ESP32) or
// net_thread.cpp (Thread, ESP32-C6) only hands it the credentials from
// secrets.h and adds the HTTP API.

#pragma once

#include <string>

namespace net {

void start();
std::string describe();

}  // namespace net
