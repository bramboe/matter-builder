// Bluetooth LE: the host, and listening to advertisements of nearby devices,
// for thermometers that broadcast their readings (BTHome v2, and the pvvx and
// ATC1441 firmware of Xiaomi thermometers) and for presence (a tag with a
// fixed address). Started only when a connection needs it; Thread shares the
// radio, so it scans a quarter of the time. Connecting to a device to send it
// commands is ble_device.h, on this host.

#pragma once

#include <cstdint>

namespace ble_scan {

struct Advert {
    uint8_t mac[6];        // as written: AA:BB:CC:DD:EE:FF is {0xAA, ..., 0xFF}
    int8_t rssi;
    bool has_climate;
    int16_t centi_celsius;
    uint16_t centi_percent;
    int8_t battery;        // percent, -1 when not sent
};

using Handler = void (*)(const Advert& advert);

// shared: Matter runs the Bluetooth stack (pairing over Bluetooth is on), so
// scanning joins it once it is up; otherwise the stack is started here, after
// Matter has let go of it
// listen: scan for advertisements (otherwise only the host, for ble_device)
void start(Handler on_advert, bool shared, bool listen = true);
bool scanning();
bool ready();  // the host is up
uint8_t own_address_type();
// Around making a connection: the scan stops meanwhile
void pause();
void resume();

}  // namespace ble_scan
