// Controllers that went away without unpairing: an app that was reset, a
// Matter server that was set up again, a test controller, or one hub of an
// Apple or Google home that is gone while the others still work. The device
// keeps the subscription such a node had, and after every restart it tries
// to reach that node again ("Failed to establish CASE for
// subscription-resumption"), with growing pauses, for days.
//
// Per stored subscription: when its node has no session with the device and
// it could not be reached ORPHAN_RETRIES times (the Matter stack counts
// that, also across restarts) or for ORPHAN_S, the subscription is dropped:
// no more attempts. A node that is still there subscribes again by itself
// when it comes back, as after any lost connection.
//
// Per controller (Matter fabric) it is kept how long it has had no live
// connection (no subscription) while the device itself was on the network,
// in NVS, so restarts do not start the count over. After 30 days it counts
// as stale: the page and "matter fabrics" say so, and it can be disconnected
// there. It is never removed by itself: removing a fabric cannot be undone,
// and a Matter server that was down for a while would have to pair again.

#pragma once

#include <cstdint>

namespace fabric_health {

constexpr uint32_t ORPHAN_S = 6 * 3600;
constexpr uint32_t ORPHAN_RETRIES = 5;
constexpr uint32_t STALE_S = 30 * 24 * 3600;

// From the status poll, in the Matter thread (every few seconds; it does its
// work once a minute)
void poll();

// A controller by its fabric index: seconds without contact (0 while
// connected, -1 when not known) and whether it is stale. Any thread.
struct Info {
    long offline_s;
    bool stale;
};
Info info(unsigned fabric_index);

}  // namespace fabric_health
