// Who may use the HTTP API besides the page's password, and proof that
// someone has the device in hand.
//
// Programs (the Home Assistant add-on, a script) get a token of their own
// from the device. A program asks with the page password (POST /link); the
// device then waits 2 minutes for physical presence: a short press of the
// BOOT button, or three quick power-ons (each under 10 s). Presence in the
// 2 minutes before the request counts too, so the power can be cycled
// first. The device keeps only a SHA-256 of each token; the page lists the
// programs and can revoke them.
//
// The BOOT button also has two long presses, acted on when released:
//   5-10 s   recovery: forget the page password (programs keep their tokens)
//   10-30 s  factory reset: erase all settings, Matter pairings included
// Longer than 30 s is ignored, so a stuck button does nothing.

#pragma once

#include <string>
#include <vector>

namespace program_access {

// Call right after NVS init. `recovery` runs after a 5-10 s press; the
// factory reset is built in.
void start(void (*recovery)());

// True when `token` belongs to a linked program
bool token_valid(const char* token);

// POST /link: a program asks for a token. Returns the JSON reply.
std::string link_request(const std::string& name);

// GET /link: the state of request `id`: waiting, linked (with the token,
// given once) or expired.
std::string link_poll(const std::string& id);

// The "access" command: (list) | json | revoke <n>
std::string command(const std::vector<std::string>& args);

}  // namespace program_access
