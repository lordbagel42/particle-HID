#if defined(PARTICLE)

#include "platform.h"

#include "concurrent_hal.h"
#include "delay_hal.h"
#include "timer_hal.h"

namespace phid {

namespace detail {
volatile os_semaphore_t wakeSemaphore = nullptr; // set by BleHid.begin()
} // namespace detail

namespace platform {

namespace {

// Created on first use so static constructors in user code can call into the library.
os_mutex_recursive_t mutex() {
    static os_mutex_recursive_t m = [] {
        os_mutex_recursive_t h = nullptr;
        os_mutex_recursive_create(&h);
        return h;
    }();
    return m;
}

} // namespace

void lock() {
    os_mutex_recursive_lock(mutex());
}

void unlock() {
    os_mutex_recursive_unlock(mutex());
}

uint32_t millis() {
    return (uint32_t)hal_timer_millis(nullptr);
}

void wake() {
    const os_semaphore_t s = detail::wakeSemaphore;
    if (s) {
        os_semaphore_give(s, false);
    }
}

void sleepMs(uint32_t ms) {
    HAL_Delay_Milliseconds(ms);
}

} // namespace platform
} // namespace phid

#endif
