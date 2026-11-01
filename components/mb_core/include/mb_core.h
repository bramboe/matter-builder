// What every Matter Builder app shares (settings page, HTTP API, program
// tokens, boot guard, console, and on the ESP32-C6 Thread with configuration
// mode and the setup access point), and the few things an app tells it.

#pragma once

#include <string>

namespace mb_core {

struct App {
    // What controllers show as manufacturer and model
    const char* vendor_name;
    const char* product_name;
    // Configuration mode answers at <hostname>.local; the setup access point
    // is "<ap_prefix>-XXXX"
    const char* hostname;
    const char* ap_prefix;
    // The settings page, gzip-compressed (target_add_binary_data)
    const char* page_gz_start;
    const char* page_gz_end;
    // The device spec the firmware was generated from (main/spec.yaml), or
    // nullptr: served at GET /spec, so adopting the device recovers it
    const char* spec_start;
    const char* spec_end;
    // Built into older firmware, used until the page sets its own (may be "")
    const char* http_token_fallback;
    const char* thread_dataset_fallback;
    // Before a restart or firmware update: stop whatever moves
    void (*stop)();
    // Configuration mode: start what works without Matter, and its state for
    // the page as a JSON object ("{...}"), merged with the mode's own fields
    void (*config_mode_start)();
    std::string (*state_json)();
    // Pairing over Bluetooth is on (nullptr: never): a device without a Thread
    // network then waits for a phone to pair it, which brings the network,
    // instead of going to configuration mode
    bool (*ble_pairing)();
};

// Call first thing in app_main, before anything else of mb_core
void init(const App& app);

const App& app();

// Runs the app's stop hook, if any
void stop();

}  // namespace mb_core
