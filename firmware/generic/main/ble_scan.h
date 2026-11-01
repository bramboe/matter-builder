// Bluetooth LE, listening only: advertisements of nearby devices, for
// thermometers that broadcast their readings (BTHome v2, and the pvvx and
// ATC1441 firmware of Xiaomi thermometers) and for presence (a tag with a
// fixed address). Nothing connects and nothing is sent. Started only when a
// connection needs it; Thread shares the radio, so it scans a quarter of
// the time.

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
void start(Handler on_advert, bool shared);
bool scanning();

}  // namespace ble_scan
