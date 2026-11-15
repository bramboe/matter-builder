// The bookkeeping of fabric_health.cpp, without ESP-IDF or Matter, so the
// host tests can run it (test/host).

#pragma once

#include <cstdint>

#include "fabric_health.h"

namespace fabric_health {

// Stored at most this often while a controller is away (flash wear); the
// count is lost by at most this much at a restart
constexpr uint32_t SAVE_EVERY_S = 3600;

struct Record {
    uint64_t id = 0;          // compressed fabric ID: a reused fabric index is another controller
    uint32_t offline_s = 0;   // without contact, while the device was on the network
    uint32_t unsaved_s = 0;   // counted since the last save
    bool stale_noted = false; // logged once per start
};

struct Step {
    bool save = false;               // store the record
    bool note_stale = false;         // log that it is stale (once per start)
};

// One look at a controller, `elapsed_s` after the previous one. `live`: it
// holds a subscription now. `counting`: the device is on the network (while
// it is not, no controller could reach it, so that time does not count).
inline Step advance(Record& r, uint64_t id, bool live, bool counting, uint32_t elapsed_s) {
    Step step;
    if (r.id != id) {
        r = Record();
        r.id = id;
        step.save = true;
    }
    if (live) {
        if (r.offline_s != 0) step.save = true;
        r.offline_s = 0;
        r.unsaved_s = 0;
        r.stale_noted = false;
        return step;
    }
    if (counting) {
        r.offline_s = r.offline_s > UINT32_MAX - elapsed_s ? UINT32_MAX : r.offline_s + elapsed_s;
        r.unsaved_s += elapsed_s;
        if (r.unsaved_s >= SAVE_EVERY_S) {
            r.unsaved_s = 0;
            step.save = true;
        }
    }
    if (r.offline_s >= STALE_S && !r.stale_noted) {
        r.stale_noted = true;
        step.note_stale = true;
    }
    return step;
}

// A stored subscription whose node has no session with the device: dropped
// after ORPHAN_RETRIES failed attempts to reach it (counted by the Matter
// stack, also across restarts), or after ORPHAN_S without a session
inline bool drop_orphan(bool reachable, uint32_t orphan_s, uint32_t failed_attempts) {
    return !reachable && (orphan_s >= ORPHAN_S || failed_attempts >= ORPHAN_RETRIES);
}

}  // namespace fabric_health
