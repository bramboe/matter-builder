#include "mb_core.h"

#include "log_buffer.h"

namespace mb_core {

namespace {

App current = {"Matter Builder", "Matter Builder", "matter-builder", "Matter-Builder", nullptr, nullptr, nullptr, nullptr, "", "",
               nullptr, nullptr, nullptr, nullptr};

}  // namespace

void init(const App& app) {
    current = app;
    log_buffer::start();  // from here on the log is also kept for the add-on and the page
}

const App& app() { return current; }

void stop() {
    if (current.stop != nullptr) {
        current.stop();
    }
}

}  // namespace mb_core
