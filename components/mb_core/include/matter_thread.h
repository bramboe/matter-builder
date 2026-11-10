// The one way into Matter from another task (the HTTP API, the console, a
// timer): the work runs on the Matter thread, and the caller waits for it.
//
// Matter's own work runs on its thread, with a stack sized for it. Taking
// the stack lock from the HTTP or console task instead runs Matter's code on
// that task's small stack: opening the pairing window that way crashed the
// bed bridge, and so did removing a fabric. So everything that reads or
// changes Matter state goes through here, however small it looks.
//
// On the Matter thread itself, and before Matter runs, the work is done at
// once. False when the Matter thread did not get to it in time (it is busy
// or stuck): the work then never runs. Work that has started is waited for,
// so it may refer to what the caller holds.

#pragma once

#include <functional>

namespace matter_thread {

bool run(std::function<void()> work, int timeout_ms = 15000);

}  // namespace matter_thread
