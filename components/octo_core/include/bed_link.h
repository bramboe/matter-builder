// Links to up to two Octo beds: connect, authenticate with the PIN, keep the
// session alive and drive the motors. Used by the Matter bridge.
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

// How many beds this bridge serves, 1 or MAX_BEDS: a setting in NVS that takes
// effect at the next start (the Matter endpoints are made at start). A fresh
// bridge has one bed; one from before the setting with a second bed keeps
// two. Beds above the count stay stored but are not connected. Any task.
int bed_count();
bool set_bed_count(int count);
// Head, legs and light under their bed as one device (Apple Home: one
// accessory per bed), or each a device of its own. Stored; on by default.
bool group_parts();
bool set_group_parts(bool on);

enum class Part { Head, Feet, Both };

void start();

// Run fn in the NimBLE host task and wait for it to finish. Does nothing
// before start() (configuration mode, BLE off).
void run_in_host(const std::function<void()>& fn);

// Full travel time of a motor, flat to fully raised. 30 s matches the
// octo_bed default; calibration measures the real value.
constexpr float DEFAULT_TRAVEL_S = 30.0f;
constexpr float MIN_TRAVEL_S = 5.0f;
constexpr float MAX_TRAVEL_S = 120.0f;

// A motor's latest run, for tracking its position.
struct RunInfo {
    uint32_t id = 0;  // changes with every new run; 0 when never moved
    bool moving = false;
    bool up = false;
    float done_s = 0.0f;    // time driven so far
    bool finished = false;  // ran its full time instead of being stopped
};

struct Info {
    bool configured = false;
    bool ready = false;  // connected and PIN accepted
    bool moving = false; // a motor command is running
    RunInfo head;
    RunInfo feet;
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
// Beds in use from now on, without a restart (configuration mode, where there
// are no Matter endpoints yet); bed_count() is not changed
void set_active_beds(int count);
// A bed in a slot above the count also raises the beds in use; the caller
// stores the new count
bool configure(int index, const uint8_t mac[6], const std::string& pin, const std::string& name);
// A new name without reconnecting; the PIN stays.
bool rename(int index, const std::string& name);
void clear(int index);
// Drives a motor (or both) for the given time and returns the id of the new
// run, or 0 when the bed is not ready. The other motor's run continues: in
// the same direction both move together, otherwise one after the other.
uint32_t move(int index, Part part, bool up, float seconds);
// Stops one motor, or both; a stop also ends a calibration of that motor.
bool stop(int index, Part part = Part::Both);
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
