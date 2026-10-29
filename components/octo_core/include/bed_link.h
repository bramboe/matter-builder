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

// Full travel time of a motor, flat to fully raised. 30 s matches the
// octo_bed default; calibration measures the real value.
constexpr float DEFAULT_TRAVEL_S = 30.0f;
constexpr float MIN_TRAVEL_S = 5.0f;
constexpr float MAX_TRAVEL_S = 120.0f;

struct Info {
    bool configured = false;
    bool ready = false;  // connected and PIN accepted
    std::string name;
    float head_travel_s = DEFAULT_TRAVEL_S;
    float feet_travel_s = DEFAULT_TRAVEL_S;
};

// Called in the host task when calibration has measured a motor; the motor
// is then fully raised.
using CalibratedCallback = std::function<void(int index, Part part, float travel_s)>;
void on_calibrated(CalibratedCallback callback);

// The functions below must run in the host task (use run_in_host).
Info info(int index);
bool configure(int index, const uint8_t mac[6], const std::string& pin, const std::string& name);
void clear(int index);
bool move(int index, Part part, bool up, float seconds);
bool stop(int index);
void stop_all();
bool light(int index, bool on);

// Travel time of one motor (Head or Feet), clamped and stored in NVS.
bool set_travel(int index, Part part, float seconds);

// Calibration: with the motor flat, start drives it up and starts a clock;
// stop (pressed when it reaches the top) stores the elapsed time as its
// travel time. Returns the measured seconds, or a negative value on failure.
bool calibrate_start(int index, Part part);
float calibrate_stop(int index);

// Machine-readable state for the settings page.
std::string state_json();
std::string status_text();
std::string scan_text();
std::string events_text();

}  // namespace bed_link
