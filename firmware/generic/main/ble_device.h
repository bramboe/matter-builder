// A Bluetooth LE device the ESP32 sends commands to: a bed, a fan, a blind,
// a desk, anything that takes bytes written to one GATT characteristic.
// Its commands become connections of the type "ble_command" (a component
// type registered here: its number is the command, 1-8).
// The device, its service and characteristic, and up to 8 commands (a label
// and the bytes to write, sent once or repeated) are set on the page or in
// the add-on, never built in. Each command can be a connection of the type
// "ble_command": a momentary switch in Matter, as an infrared code to send is.
//
// The device is connected when a command is sent and stays connected for a
// while (idle seconds), so a few commands in a row go out at once; some
// devices want a first message right after connecting ("hello", e.g. a PIN).
//
// Commands sent to it:
//   bledev json
//   bledev target <AA:BB:CC:DD:EE:FF> [public|random]
//   bledev service <uuid> | characteristic <uuid>   (16-bit "FFE0" or 128-bit)
//   bledev write response|no-response
//   bledev pair on|off                    initiate GAP security before GATT discovery
//   bledev hello <hex>|none                 sent after every connect
//   bledev setup <compact JSON array>       ordered connection steps; [] restores defaults
//   bledev secret <name> <value>|-          set or clear a local setup secret
//   bledev test                            rerun setup; status is reported by json
//   bledev idle <seconds>                   stay connected after the last command (0-3600)
//   bledev cmd <n> <hex> <times> <ms> <label as hex> [finish as hex finish-ms]  command n (0-7)
//   bledev clear <n> | send <n> | raw <hex> | disconnect
//   bledev template <name>                  fill in a known device (octo)

#pragma once

#include <string>
#include <vector>

namespace ble_device {

constexpr int COMMANDS = 8;

// The connection type, before io::start(); its name
void register_type();
extern const char* const TYPE;

// Loads the settings
void load();
bool configured();

// Sends command n (queued; from any task). False: unknown or not set up.
bool send(int command);
std::string label(int command);

std::string command(const std::vector<std::string>& args);

}  // namespace ble_device
