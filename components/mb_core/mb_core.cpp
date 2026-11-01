#include "mb_core.h"

namespace mb_core {

namespace {

App current = {"Matter Builder", "Matter Builder", "matter-builder", "Matter-Builder", nullptr, nullptr, nullptr, nullptr, "", "",
               nullptr, nullptr, nullptr};

}  // namespace

void init(const App& app) { current = app; }

const App& app() { return current; }

void stop() {
    if (current.stop != nullptr) {
        current.stop();
    }
}

}  // namespace mb_core
