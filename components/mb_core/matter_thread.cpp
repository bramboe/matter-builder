#include "matter_thread.h"

#include <atomic>
#include <memory>

#include <platform/CHIPDeviceLayer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace matter_thread {

namespace {

// Shared with the Matter thread: it may still be at work, or get to it
// later, when the caller has given up waiting
enum : int { PENDING, RUNNING, CANCELLED };

struct Request {
    std::function<void()> work;
    std::atomic<int> state{PENDING};
    SemaphoreHandle_t done = nullptr;
    ~Request() {
        if (done != nullptr) vSemaphoreDelete(done);
    }
};

}  // namespace

bool run(std::function<void()> work, int timeout_ms) {
    // The Matter thread exists once Matter has started; until then nothing
    // else runs Matter's code, and scheduled work would wait for the start
    const TaskHandle_t matter = xTaskGetHandle(CHIP_DEVICE_CONFIG_CHIP_TASK_NAME);
    if (matter == nullptr || matter == xTaskGetCurrentTaskHandle()) {
        work();
        return true;
    }
    auto* shared = new std::shared_ptr<Request>(std::make_shared<Request>());
    std::shared_ptr<Request> request = *shared;
    request->work = std::move(work);
    request->done = xSemaphoreCreateBinary();
    if (request->done == nullptr) {
        delete shared;
        return false;
    }
    const CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            auto* held = reinterpret_cast<std::shared_ptr<Request>*>(arg);
            // Not when the caller gave up: what the work refers to may be gone
            int expected = PENDING;
            if ((*held)->state.compare_exchange_strong(expected, RUNNING)) {
                (*held)->work();
                xSemaphoreGive((*held)->done);
            }
            delete held;
        },
        reinterpret_cast<intptr_t>(shared));
    if (err != CHIP_NO_ERROR) {
        delete shared;
        return false;
    }
    if (xSemaphoreTake(request->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return true;
    }
    // Too late. Either it never starts now, or it has started: then it may
    // use what the caller holds, so the caller waits until it is done
    int expected = PENDING;
    if (request->state.compare_exchange_strong(expected, CANCELLED)) {
        return false;
    }
    xSemaphoreTake(request->done, portMAX_DELAY);
    return true;
}

}  // namespace matter_thread
