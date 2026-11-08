#include "engine.h"

namespace phid {

bool HidEngine::add(HidDevice& device) {
    if (built_ || count_ == kMaxDevices) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (devices_[i] == &device) {
            return true;
        }
    }
    devices_[count_++] = &device;
    return true;
}

int HidEngine::bootDevice(BootRole role) const {
    for (size_t i = 0; i < count_; ++i) {
        if (devices_[i]->bootRole() == role) {
            return (int)i;
        }
    }
    return -1;
}

ProfileError HidEngine::buildProfile() {
    if (count_ == 0) {
        return ProfileError::NoDevices;
    }
    bool kbd = false, mouse = false;
    for (size_t i = 0; i < count_; ++i) {
        const BootRole r = devices_[i]->bootRole();
        bool& seen = (r == BootRole::BootKeyboard) ? kbd : mouse;
        if (r != BootRole::None) {
            if (seen) {
                return ProfileError::DuplicateBootRole;
            }
            seen = true;
        }
        if (devices_[i]->inputLength() == 0 || devices_[i]->inputLength() > kMaxReport) {
            return ProfileError::InvalidLayout;
        }
    }
    ReportMapBuilder b(map_, sizeof(map_));
    for (size_t i = 0; i < count_; ++i) {
        devices_[i]->reportId_ = (uint8_t)(i + 1);
        devices_[i]->ready_ = &ready_;
        devices_[i]->appendDescriptor(b, devices_[i]->reportId_);
    }
    if (b.overflowed()) {
        return ProfileError::ReportMapTooLarge;
    }
    mapLen_ = b.size();
    summary_ = parseDescriptor(map_, mapLen_);
    if (summary_.error != DescriptorError::None) {
        return ProfileError::DescriptorInvalid;
    }
    for (size_t i = 0; i < count_; ++i) {
        const HidDevice& d = *devices_[i];
        if (summary_.inputBytes(d.reportId()) != d.inputLength() || summary_.outputBytes(d.reportId()) != d.outputLength()) {
            return ProfileError::LengthMismatch;
        }
    }
    built_ = true;
    return ProfileError::None;
}

void HidEngine::setReady(bool ready) {
    if (ready && !ready_) {
        for (size_t i = 0; i < count_; ++i) {
            devices_[i]->markSync(); // restate everything to a freshly connected host
        }
    }
    ready_ = ready;
}

bool HidEngine::deliverable(size_t i) const {
    if (!ready_) {
        return false;
    }
    if (boot_) {
        // In boot protocol only the boot keyboard and mouse reports exist.
        return devices_[i]->bootRole() != BootRole::None && subscribed_[i];
    }
    return subscribed_[i];
}

bool HidEngine::anyPending() const {
    uint32_t seq;
    for (size_t i = 0; i < count_; ++i) {
        if (devices_[i]->pending(&seq)) {
            return true;
        }
    }
    return false;
}

void HidEngine::dropQueued(uint32_t now) {
    for (size_t i = 0; i < count_; ++i) {
        devices_[i]->expire(now, 0, true);
    }
}

void HidEngine::service(uint32_t now, Transport& transport) {
    for (size_t i = 0; i < count_; ++i) {
        if (!deliverable(i)) {
            devices_[i]->expire(now, readyDeadlineMs_, false);
        }
    }
    if (!ready_) {
        return;
    }
    for (;;) {
        // Oldest change first, across devices, so ctrl+click arrives as ctrl, then click.
        int best = -1;
        uint32_t bestSeq = 0;
        for (size_t i = 0; i < count_; ++i) {
            uint32_t seq;
            if (deliverable(i) && devices_[i]->pending(&seq) && (best < 0 || (int32_t)(seq - bestSeq) < 0)) {
                best = (int)i;
                bestSeq = seq;
            }
        }
        if (best < 0) {
            return;
        }
        HidDevice& d = *devices_[best];
        const bool boot = boot_ && d.bootRole() != BootRole::None;
        uint8_t buf[kMaxReport];
        const size_t len = d.buildReport(buf, boot);
        switch (transport.sendInput((size_t)best, boot, buf, len)) {
        case SendResult::Ok:
            d.commit();
            ++d.stats_.sent;
            ++stats_.sent;
            break;
        case SendResult::Busy:
            ++stats_.busy;
            return;
        case SendResult::NotSubscribed:
            subscribed_[best] = false;
            break;
        case SendResult::Error:
            ++stats_.sendErrors;
            return;
        }
    }
}

} // namespace phid
