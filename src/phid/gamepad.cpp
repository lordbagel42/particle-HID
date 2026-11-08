#include "gamepad.h"

#include "platform.h"

namespace phid {

namespace {

int16_t toAxis(float v) {
    if (!(v >= -1.0f)) { // also catches NaN
        v = -1.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    return (int16_t)(v * 32767.0f);
}

int16_t clampAxis(int16_t v) {
    return v < -32767 ? -32767 : v; // keep the range symmetric
}

} // namespace

void BTGamepad::dirty() {
    if (!dirty_) {
        dirty_ = true;
        seq_ = stamp().seq;
    }
    wake();
}

bool BTGamepad::setButton(uint8_t button, bool down) {
    platform::Guard g;
    if (button < 1 || button > layout_.buttonCount) {
        ++stats_.rejectedInvalid;
        return false;
    }
    const uint32_t mask = 1u << (button - 1);
    if (((buttons_ & mask) != 0) == down) {
        return true;
    }
    buttons_ ^= mask;
    if (((buttons_ ^ sent_) & mask) == 0) {
        intermediate_ |= mask; // back to what the host has; the change in between must still be sent
    } else {
        intermediate_ &= ~mask;
    }
    dirty();
    return true;
}

void BTGamepad::releaseAll() {
    for (uint8_t b = 1; b <= layout_.buttonCount; ++b) {
        setButton(b, false);
    }
}

void BTGamepad::hat(Hat h) {
    platform::Guard g;
    if (h != hat_) {
        hat_ = h;
        dirty();
    }
}

void BTGamepad::stick(Stick s, int16_t x, int16_t y) {
    platform::Guard g;
    const size_t i = (size_t)s * 2;
    x = clampAxis(x);
    y = clampAxis(y);
    if (i + 1 < 4 && (axes_[i] != x || axes_[i + 1] != y)) {
        axes_[i] = x;
        axes_[i + 1] = y;
        dirty();
    }
}

void BTGamepad::stick(Stick s, float x, float y) {
    stick(s, toAxis(x), toAxis(y));
}

void BTGamepad::trigger(Trigger t, uint8_t value) {
    platform::Guard g;
    const size_t i = (size_t)t;
    if (i < 2 && triggers_[i] != value) {
        triggers_[i] = value;
        dirty();
    }
}

void BTGamepad::trigger(Trigger t, float value) {
    if (!(value >= 0.0f)) {
        value = 0.0f;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    trigger(t, (uint8_t)(value * 255.0f + 0.5f));
}

uint8_t BTGamepad::inputLength() const {
    return (layout_.buttonCount + 7) / 8 + (layout_.hasHat ? 1 : 0) + layout_.stickCount * 4 + (layout_.hasTriggers ? 2 : 0);
}

bool BTGamepad::pending(uint32_t* seq) const {
    if (dirty_) {
        *seq = seq_;
        return true;
    }
    if (sync_) {
        *seq = syncSeq_;
        return true;
    }
    return false;
}

size_t BTGamepad::buildReport(uint8_t* out, bool boot) {
    (void)boot;
    built_ = buttons_ ^ intermediate_;
    size_t n = 0;
    for (size_t i = 0; i < ((size_t)layout_.buttonCount + 7) / 8; ++i) {
        out[n++] = (uint8_t)(built_ >> (8 * i));
    }
    if (layout_.hasHat) {
        out[n++] = (uint8_t)hat_;
    }
    for (size_t i = 0; i < (size_t)layout_.stickCount * 2; ++i) {
        out[n++] = (uint8_t)axes_[i];
        out[n++] = (uint8_t)(axes_[i] >> 8);
    }
    if (layout_.hasTriggers) {
        out[n++] = triggers_[0];
        out[n++] = triggers_[1];
    }
    return n;
}

void BTGamepad::commit() {
    sync_ = false;
    sent_ = built_;
    if (intermediate_) {
        intermediate_ = 0; // the current state still has to go out
        dirty_ = true;
    } else {
        dirty_ = false;
    }
}

void BTGamepad::appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const {
    b.usagePage(0x01).usage(0x05).collection(Collection::Application);
    b.reportId(reportId);
    if (layout_.buttonCount) {
        b.usagePage(0x09).usageMin(1).usageMax(layout_.buttonCount).logicalMin(0).logicalMax(1);
        b.reportSize(1).reportCount(layout_.buttonCount).input(Io::Data | Io::Variable | Io::Absolute);
        b.inputPadding((8 - layout_.buttonCount % 8) % 8);
    }
    if (layout_.hasHat) {
        b.usagePage(0x01).usage(0x39).logicalMin(0).logicalMax(7).physicalMin(0).physicalMax(315).unit(0x14);
        b.reportSize(4).reportCount(1).input(Io::Data | Io::Variable | Io::Absolute | Io::NullState);
        b.unit(0).physicalMin(0).physicalMax(0);
        b.inputPadding(4);
    }
    if (layout_.stickCount) {
        b.usagePage(0x01).logicalMin(-32767).logicalMax(32767);
        b.usage(0x30).usage(0x31); // left X, Y
        if (layout_.stickCount > 1) {
            b.usage(0x32).usage(0x35); // right Z, Rz
        }
        b.reportSize(16).reportCount(layout_.stickCount * 2).input(Io::Data | Io::Variable | Io::Absolute);
    }
    if (layout_.hasTriggers) {
        b.usagePage(0x02).usage(0xC5).usage(0xC4).logicalMin(0).logicalMax(255); // Brake, Accelerator
        b.reportSize(8).reportCount(2).input(Io::Data | Io::Variable | Io::Absolute);
    }
    b.endCollection();
}

} // namespace phid
