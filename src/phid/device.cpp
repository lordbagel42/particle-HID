#include "device.h"

#include "platform.h"

namespace phid {

namespace {
uint32_t g_seq = 0;
} // namespace

HidDevice::Stamp HidDevice::stamp() {
    // Called with the library lock held.
    return Stamp{++g_seq, platform::millis()};
}

void HidDevice::markSync() {
    sync_ = true;
    syncSeq_ = stamp().seq;
}

bool HidDevice::hostReady() const {
    return ready_ && *ready_;
}

void HidDevice::wake() const {
    platform::wake();
}

} // namespace phid
