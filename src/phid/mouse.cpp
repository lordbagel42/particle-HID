#include "mouse.h"

#include "platform.h"

namespace phid {

namespace {

constexpr uint8_t kButtonMask = 0x1F;

size_t popcount8(uint8_t v) {
    size_t n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

int32_t addSat(int32_t a, int32_t b) {
    const int64_t s = (int64_t)a + b;
    return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
}

int32_t clamp(int32_t v, int32_t lim) {
    return v > lim ? lim : v < -lim ? -lim : v;
}

} // namespace

// Invariant: queue_.size() + popcount(logical_) <= kQueueSize, so releasing
// every held button always fits.
bool BTMouse::move(int32_t dx, int32_t dy, int32_t wheel, int32_t pan) {
    if (!dx && !dy && !wheel && !pan) {
        return true;
    }
    platform::Guard g;
    if (!queue_.empty() && queue_.back().kind == Move) {
        Event& e = queue_.back();
        e.x = addSat(e.x, dx);
        e.y = addSat(e.y, dy);
        e.wheel = addSat(e.wheel, wheel);
        e.pan = addSat(e.pan, pan);
        wake();
        return true;
    }
    if (queue_.size() + 1 + popcount8(logical_) > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    const Stamp st = stamp();
    queue_.push(Event{Move, 0, dx, dy, wheel, pan, st.seq, st.time});
    wake();
    return true;
}

bool BTMouse::setButtons(uint8_t next) {
    if (next == logical_) {
        return true;
    }
    if (queue_.size() + 1 + popcount8(next) > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    const Stamp st = stamp();
    queue_.push(Event{Buttons, next, 0, 0, 0, 0, st.seq, st.time});
    logical_ = next;
    wake();
    return true;
}

bool BTMouse::press(uint8_t buttons) {
    platform::Guard g;
    if (buttons & ~kButtonMask) {
        ++stats_.rejectedInvalid;
        return false;
    }
    return setButtons(logical_ | buttons);
}

bool BTMouse::release(uint8_t buttons) {
    platform::Guard g;
    return setButtons(logical_ & ~buttons);
}

bool BTMouse::click(uint8_t buttons) {
    platform::Guard g;
    if (buttons & ~kButtonMask) {
        ++stats_.rejectedInvalid;
        return false;
    }
    const uint8_t down = logical_ | buttons;
    if (down == logical_) {
        ++stats_.rejectedInvalid; // already held; a click would be a release
        return false;
    }
    if (queue_.size() + 2 + popcount8(logical_) > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    const uint8_t before = logical_;
    setButtons(down);
    setButtons(before);
    return true;
}

bool BTMouse::pending(uint32_t* seq) const {
    if (!queue_.empty()) {
        *seq = queue_.front().seq;
        return true;
    }
    if (sync_) {
        *seq = syncSeq_;
        return true;
    }
    return false;
}

size_t BTMouse::buildReport(uint8_t* out, bool boot) {
    uint8_t buttons = base_;
    last_ = {};
    lastBoot_ = boot;
    if (!queue_.empty()) {
        const Event& e = queue_.front();
        if (e.kind == Buttons) {
            buttons = e.buttons;
        } else {
            const int32_t lim = boot ? 127 : 32767;
            last_ = Chunk{clamp(e.x, lim), clamp(e.y, lim), boot ? 0 : clamp(e.wheel, 127), boot ? 0 : clamp(e.pan, 127)};
        }
    }
    out[0] = buttons;
    if (boot) {
        out[1] = (uint8_t)(int8_t)last_.x;
        out[2] = (uint8_t)(int8_t)last_.y;
        return 3;
    }
    out[1] = (uint8_t)last_.x;
    out[2] = (uint8_t)(last_.x >> 8);
    out[3] = (uint8_t)last_.y;
    out[4] = (uint8_t)(last_.y >> 8);
    out[5] = (uint8_t)(int8_t)last_.wheel;
    out[6] = (uint8_t)(int8_t)last_.pan;
    return 7;
}

void BTMouse::commit() {
    sync_ = false;
    if (queue_.empty()) {
        return;
    }
    Event& e = queue_.front();
    if (e.kind == Buttons) {
        base_ = e.buttons;
        queue_.pop();
        return;
    }
    e.x -= last_.x;
    e.y -= last_.y;
    e.wheel -= last_.wheel;
    e.pan -= last_.pan;
    if (lastBoot_) {
        e.wheel = 0; // boot reports have no wheel or pan
        e.pan = 0;
    }
    if (!e.x && !e.y && !e.wheel && !e.pan) {
        queue_.pop();
    }
    last_ = {};
}

void BTMouse::expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) {
    while (!queue_.empty() && (dropAll || now - queue_.front().time > maxAgeMs)) {
        const Event& e = queue_.front();
        if (e.kind == Buttons) {
            base_ = e.buttons;
        }
        queue_.pop(); // stale motion is meaningless once the host can see the pointer again
        ++stats_.droppedNotReady;
    }
}

void BTMouse::appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const {
    b.usagePage(0x01).usage(0x02).collection(Collection::Application);
    b.reportId(reportId);
    b.usage(0x01).collection(Collection::Physical);
    // Buttons
    b.usagePage(0x09).usageMin(1).usageMax(5).logicalMin(0).logicalMax(1);
    b.reportSize(1).reportCount(5).input(Io::Data | Io::Variable | Io::Absolute);
    b.inputPadding(3);
    // X, Y
    b.usagePage(0x01).usage(0x30).usage(0x31).logicalMin(-32767).logicalMax(32767);
    b.reportSize(16).reportCount(2).input(Io::Data | Io::Variable | Io::Relative);
    // Wheel
    b.usage(0x38).logicalMin(-127).logicalMax(127);
    b.reportSize(8).reportCount(1).input(Io::Data | Io::Variable | Io::Relative);
    // AC Pan
    b.usagePage(0x0C).usage(0x238).reportCount(1).input(Io::Data | Io::Variable | Io::Relative);
    b.endCollection();
    b.endCollection();
}

} // namespace phid
