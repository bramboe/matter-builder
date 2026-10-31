// The HTTP API shared by all apps, so a device can be used and updated
// without USB. Every request needs the header "X-Octo-Token: <password>".
//
//   GET  /      settings page (no token needed to load it)
//   GET  /auth  {"password":true|false}: whether a password was chosen
//   POST /auth  body = new password (8-63 characters); the first one needs
//               no token, a change needs the current one
//   POST /cmd   body = console command line   -> command output
//   POST /ota   body = firmware image (.bin)  -> flashed to the other slot, reboot
//
// Starting it also confirms the running image to the boot guard.

#pragma once

#include <string>

namespace http_api {

// Call once the device has an IP address; later calls do nothing. The
// password comes from NVS; `fallback_token` (a token built into an older
// firmware) is used until one is chosen on the page.
void start(const char* fallback_token);

// Forget the password, so the page asks for a new one. Serial console only:
// it is the way back for someone with the device in hand.
std::string reset_password();

// Answer every unknown path with a redirect to `url` (after start()): on the
// setup access point this opens the page as the phone's sign-in page.
void redirect_unknown_to(const char* url);

}  // namespace http_api
