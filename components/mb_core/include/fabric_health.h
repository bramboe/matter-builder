// Controllers that went away without unpairing: an app that was reset, a
// Matter server that was set up again, a test controller. The device keeps
// their fabric and the subscription it had, and after every restart it tries
// to reach that controller again ("Failed to establish CASE for
// subscription-resumption"), with growing pauses, for days.
//
// Per controller (Matter fabric) it is kept how long it has had no live
// connection (no subscription) while the device itself was on the network,
// in NVS, so restarts do not start the count over:
//
//   24 hours  its stored subscription is dropped: no more attempts to reach
//             it. A controller that is still there subscribes again by
//             itself when it comes back, as after any lost connection.
//   30 days   it counts as stale: the page and "matter fabrics" say so, and
//             it can be disconnected there. It is never removed by itself:
//             removing a fabric cannot be undone, and a Matter server that
//             was down for a while would have to pair again.

#pragma once

#include <cstdint>

namespace fabric_health {

constexpr uint32_t RESUMPTION_GIVE_UP_S = 24 * 3600;
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
