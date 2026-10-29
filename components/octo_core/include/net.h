// Network side of an app: Wi-Fi plus a small HTTP API, so the device can be
// used and updated without USB. Every request needs the header
// "X-Octo-Token: <token from secrets.h>".
//
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// Each app has its own net.cpp: the spike runs Wi-Fi itself, the Matter
// bridge leaves Wi-Fi to the Matter stack and only adds the HTTP API.

#pragma once

#include <string>

namespace net {

void start();
std::string describe();

}  // namespace net
