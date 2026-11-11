// A build without Bluetooth (the classic ESP32, sdkconfig.defaults.esp32):
// ble_scan.h and ble_device.h without anything behind them. Bluetooth
// connections cannot be made on such a device; io.cpp says so on the page.

#include "ble_device.h"
#include "ble_scan.h"

namespace ble_scan {

void start(Handler, bool, bool) {}
bool scanning() { return false; }
bool ready() { return false; }
uint8_t own_address_type() { return 0; }
void pause() {}
void resume() {}

}  // namespace ble_scan

namespace ble_device {

const char* const TYPE = "ble_command";

void register_type() {}
void load() {}
bool configured() { return false; }
bool send(int) { return false; }
std::string label(int) { return ""; }
// Not JSON: the page and the add-on take that as "no Bluetooth device here"
std::string command(const std::vector<std::string>&) { return "this firmware has no Bluetooth\n"; }

}  // namespace ble_device
