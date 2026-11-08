#include "usage_keys.h"

#include "platform.h"

namespace phid {

// Invariant: queue_.size() + (logical_ != 0) <= kQueueSize, so the release of
// a held key always fits.
void UsageKeys::push(uint16_t usage) {
    const Stamp st = stamp();
    queue_.push(Event{usage, st.seq, st.time});
    logical_ = usage;
    wake();
}

bool UsageKeys::pressUsage(uint16_t usage) {
    platform::Guard g;
    if (usage == 0) {
        ++stats_.rejectedInvalid;
        return false;
    }
    if (logical_ == usage) {
        return true;
    }
    if (queue_.size() + 2 > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    push(usage);
    return true;
}

bool UsageKeys::releaseUsage(uint16_t usage) {
    platform::Guard g;
    if (logical_ != usage || usage == 0) {
        return true;
    }
    push(0);
    return true;
}

bool UsageKeys::tapUsage(uint16_t usage) {
    platform::Guard g;
    if (usage == 0 || logical_ == usage) {
        ++stats_.rejectedInvalid;
        return false;
    }
    if (queue_.size() + 2 > kQueueSize) {
        ++stats_.rejectedFull;
        return false;
    }
    push(usage);
    push(0);
    return true;
}

bool UsageKeys::pending(uint32_t* seq) const {
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

size_t UsageKeys::buildReport(uint8_t* out, bool boot) {
    (void)boot;
    encode(queue_.empty() ? base_ : queue_.front().usage, out);
    return inputLength();
}

void UsageKeys::commit() {
    if (!queue_.empty()) {
        base_ = queue_.front().usage;
        queue_.pop();
    }
    sync_ = false;
}

void UsageKeys::expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) {
    while (!queue_.empty() && (dropAll || now - queue_.front().time > maxAgeMs)) {
        base_ = queue_.front().usage;
        queue_.pop();
        ++stats_.droppedNotReady;
    }
}

void BTMediaKeys::appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const {
    b.usagePage(0x0C).usage(0x01).collection(Collection::Application);
    b.reportId(reportId);
    b.logicalMin(0).logicalMax(0x3FF).usageMin(0).usageMax(0x3FF);
    b.reportSize(16).reportCount(1).input(Io::Data | Io::Array | Io::Absolute);
    b.endCollection();
}

void BTSystemControl::appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const {
    b.usagePage(0x01).usage(0x80).collection(Collection::Application);
    b.reportId(reportId);
    b.usageMin(0x81).usageMax(0x83).logicalMin(1).logicalMax(3);
    b.reportSize(2).reportCount(1).input(Io::Data | Io::Array | Io::Absolute);
    b.inputPadding(6);
    b.endCollection();
}

} // namespace phid
