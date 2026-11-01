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

void start(Handler on_advert);

}  // namespace ble_scan
