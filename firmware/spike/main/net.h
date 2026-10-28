// Wi-Fi plus a small HTTP API, so the spike can be used and updated without
// USB. Every request needs the header "X-Octo-Token: <token from secrets.h>".
//
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// Matter will later provide the Wi-Fi credentials during commissioning; for
// the spike they come from main/secrets.h (not in git).

#pragma once

#include <string>

namespace net {

void start();
std::string describe();

}  // namespace net
