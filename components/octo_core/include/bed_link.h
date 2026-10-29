// Links to up to two Octo beds: connect, authenticate with the PIN, keep the
// session alive and drive the motors. Used by the spike and the Matter bridge.
//
// All BLE state lives in the NimBLE host task. Other tasks (the console) hand
// work to that task with bed_link::run_in_host(), so no locking is needed and
// NimBLE is never called while holding our own lock.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bed_link {

constexpr int MAX_BEDS = 2;

enum class Part { Head, Feet, Both };

void start();

// Run fn in the NimBLE host task and wait for it to finish.
void run_in_host(const std::function<void()>& fn);

struct Info {
    bool configured = false;
    bool ready = false;  // connected and PIN accepted
    std::string name;
};

// The functions below must run in the host task (use run_in_host).
Info info(int index);
bool configure(int index, const uint8_t mac[6], const std::string& pin, const std::string& name);
void clear(int index);
bool move(int index, Part part, bool up, float seconds);
bool stop(int index);
void stop_all();
bool light(int index, bool on);
std::string status_text();
std::string scan_text();
std::string events_text();

}  // namespace bed_link
