#pragma once

#include <cstdint>

namespace phid {
namespace platform {

// Implemented in platform_particle.cpp on devices and by the native test harness.
void lock();                   // recursive library lock
void unlock();
uint32_t millis();
void wake();                   // wake the HID worker
void sleepMs(uint32_t ms);

struct Guard {
    Guard() {
        lock();
    }
    ~Guard() {
        unlock();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

} // namespace platform
} // namespace phid
