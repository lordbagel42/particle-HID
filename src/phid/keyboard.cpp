#include "keyboard.h"

#include "platform.h"

namespace phid {

namespace {

size_t popcount8(uint8_t v) {
    size_t n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

} // namespace

bool BTKeyboard::State::has(uint8_t usage) const {
    if (isModifier(usage)) {
        return mods & (1 << (usage - 0xE0));
    }
    for (uint8_t k : keys) {
        if (k == usage) {
            return true;
        }
    }
    return false;
}

bool BTKeyboard::State::add(uint8_t usage) {
    if (isModifier(usage)) {
        mods |= 1 << (usage - 0xE0);
        return true;
    }
    if (has(usage)) {
        return true;
    }
    for (uint8_t& k : keys) {
        if (k == 0) {
            k = usage;
            return true;
        }
    }
    return false;
}

void BTKeyboard::State::remove(uint8_t usage) {
    if (isModifier(usage)) {
        mods &= ~(1 << (usage - 0xE0));
        return;
    }
    // Keep the remaining keys in press order.
    size_t w = 0;
    for (size_t r = 0; r < 6; ++r) {
        if (keys[r] != usage) {
            keys[w++] = keys[r];
        }
    }
    while (w < 6) {
        keys[w++] = 0;
    }
}

size_t BTKeyboard::State::keyCount() const {
    size_t n = 0;
    for (uint8_t k : keys) {
        n += (k != 0);
    }
    return n;
}

size_t BTKeyboard::State::down() const {
    return popcount8(mods) + keyCount();
}

void BTKeyboard::apply(State& s, const Event& e) {
    if (e.down) {
        s.add(e.usage);
    } else {
        s.remove(e.usage);
    }
}

void BTKeyboard::push(uint8_t usage, bool down) {
    const Stamp st = stamp();
    queue_.push(Event{usage, (uint8_t)down, st.seq, st.time});
    if (down) {
        logical_.add(usage);
    } else {
        logical_.remove(usage);
    }
}

// Invariant: queue_.size() + logical_.down() <= kQueueSize, so the release of
// every key that is down always fits.
bool BTKeyboard::press(uint8_t usage) {
    platform::Guard g;
    if (!valid(usage)) {
        ++stats_.rejectedInvalid;
        return false;
    }
    if (logical_.has(usage)) {
        return true;
    }
    if (!isModifier(usage) && logical_.keyCount() == 6) {
        ++stats_.rejectedInvalid; // 6-key rollover
        return false;
    }
    if (queue_.size() + 1 + logical_.down() + 1 > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    push(usage, true);
    wake();
    return true;
}

bool BTKeyboard::release(uint8_t usage) {
    platform::Guard g;
    if (!logical_.has(usage)) {
        return true;
    }
    push(usage, false);
    wake();
    return true;
}

void BTKeyboard::releaseAll() {
    platform::Guard g;
    State s = logical_;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (s.mods & (1 << bit)) {
            push(0xE0 + bit, false);
        }
    }
    for (uint8_t k : s.keys) {
        if (k) {
            push(k, false);
        }
    }
    wake();
}

BTKeyboard::TapResult BTKeyboard::tapLocked(uint8_t usage, uint8_t mods) {
    if (!valid(usage) || logical_.has(usage)) {
        ++stats_.rejectedInvalid;
        return TapResult::Invalid;
    }
    if (!isModifier(usage) && logical_.keyCount() == 6) {
        ++stats_.rejectedInvalid;
        return TapResult::Invalid;
    }
    uint8_t newMods = mods & ~logical_.mods;
    if (isModifier(usage)) {
        newMods &= ~(1 << (usage - 0xE0));
    }
    const size_t events = 2 * (1 + popcount8(newMods));
    if (queue_.size() + events + logical_.down() > kQueueSize) {
        return TapResult::Full; // callers count the refusal; write() may retry
    }
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (newMods & (1 << bit)) {
            push(0xE0 + bit, true);
        }
    }
    push(usage, true);
    push(usage, false);
    for (int bit = 7; bit >= 0; --bit) {
        if (newMods & (1 << bit)) {
            push(0xE0 + bit, false);
        }
    }
    return TapResult::Ok;
}

bool BTKeyboard::tap(Key key, uint8_t mods) {
    platform::Guard g;
    const TapResult r = tapLocked((uint8_t)key, mods);
    if (r == TapResult::Full) {
        ++stats_.rejectedFull;
    } else if (r == TapResult::Ok) {
        wake();
    }
    return r == TapResult::Ok;
}

size_t BTKeyboard::write(uint8_t c) {
    const KeyStroke ks = layout_->map((char)c);
    if (ks.key == 0) {
        return c == '\r' ? 1 : 0;
    }
    const uint32_t start = platform::millis();
    for (;;) {
        {
            platform::Guard g;
            const TapResult r = tapLocked(ks.key, ks.mods);
            if (r == TapResult::Ok) {
                wake();
                return 1;
            }
            if (r == TapResult::Invalid) {
                return 0;
            }
            if (!hostReady() || platform::millis() - start >= writeTimeoutMs_) {
                ++stats_.rejectedFull;
                return 0;
            }
        }
        wake();
        platform::sleepMs(5);
    }
}

size_t BTKeyboard::type(const char* text) {
    size_t n = 0;
    for (; text && *text; ++text) {
        if (!write((uint8_t)*text)) {
            break;
        }
        ++n;
    }
    return n;
}

bool BTKeyboard::pending(uint32_t* seq) const {
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

size_t BTKeyboard::buildReport(uint8_t* out, bool boot) {
    (void)boot; // the boot keyboard report has the same layout
    State s = base_;
    if (!queue_.empty()) {
        apply(s, queue_.front());
    }
    out[0] = s.mods;
    out[1] = 0;
    for (size_t i = 0; i < 6; ++i) {
        out[2 + i] = s.keys[i];
    }
    return 8;
}

void BTKeyboard::commit() {
    if (!queue_.empty()) {
        apply(base_, queue_.front());
        queue_.pop();
    }
    sync_ = false;
}

void BTKeyboard::expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) {
    while (!queue_.empty() && (dropAll || now - queue_.front().time > maxAgeMs)) {
        apply(base_, queue_.front());
        queue_.pop();
        ++stats_.droppedNotReady;
    }
}

void BTKeyboard::onOutput(const uint8_t* data, size_t len) {
    if (len >= 1 && data[0] != leds_) {
        leds_ = data[0];
        ledsChanged_ = true;
    }
}

void BTKeyboard::runCallbacks() {
    if (ledsChanged_) {
        ledsChanged_ = false;
        if (ledCallback_) {
            ledCallback_(leds_);
        }
    }
}

void BTKeyboard::appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const {
    b.usagePage(0x01).usage(0x06).collection(Collection::Application);
    b.reportId(reportId);
    // Modifiers
    b.usagePage(0x07).usageMin(0xE0).usageMax(0xE7).logicalMin(0).logicalMax(1);
    b.reportSize(1).reportCount(8).input(Io::Data | Io::Variable | Io::Absolute);
    // Reserved byte
    b.reportSize(8).reportCount(1).input(Io::Constant);
    // LEDs
    b.usagePage(0x08).usageMin(1).usageMax(5).reportSize(1).reportCount(5).output(Io::Data | Io::Variable | Io::Absolute);
    b.reportSize(3).reportCount(1).output(Io::Constant);
    // Keys
    b.usagePage(0x07).usageMin(0).usageMax(kMaxKeyUsage).logicalMin(0).logicalMax(kMaxKeyUsage);
    b.reportSize(8).reportCount(6).input(Io::Data | Io::Array | Io::Absolute);
    b.endCollection();
}

} // namespace phid
