// The HTTP API shared by all apps, so a device can be used and updated
// without USB. Every request needs the header "X-Octo-Token: <token>".
//
//   GET  /      settings page (no token needed to load it)
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// Starting it also confirms the running image to the boot guard.

#pragma once

namespace http_api {

// Call once the device has an IP address; later calls do nothing.
void start(const char* token);

}  // namespace http_api
