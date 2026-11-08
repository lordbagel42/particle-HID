#include "report_map.h"

namespace phid {

void ReportMapBuilder::put(uint8_t b) {
    if (len_ < cap_) {
        buf_[len_++] = b;
    } else {
        overflow_ = true;
    }
}

ReportMapBuilder& ReportMapBuilder::unsignedItem(uint8_t tagType, uint32_t v) {
    if (v <= 0xFF) {
        put(tagType | 1);
        put(v);
    } else if (v <= 0xFFFF) {
        put(tagType | 2);
        put(v);
        put(v >> 8);
    } else {
        put(tagType | 3);
        put(v);
        put(v >> 8);
        put(v >> 16);
        put(v >> 24);
    }
    return *this;
}

ReportMapBuilder& ReportMapBuilder::signedItem(uint8_t tagType, int32_t v) {
    if (v >= -128 && v <= 127) {
        put(tagType | 1);
        put(static_cast<uint8_t>(v));
    } else if (v >= -32768 && v <= 32767) {
        put(tagType | 2);
        put(static_cast<uint8_t>(v));
        put(static_cast<uint8_t>(v >> 8));
    } else {
        put(tagType | 3);
        put(static_cast<uint8_t>(v));
        put(static_cast<uint8_t>(v >> 8));
        put(static_cast<uint8_t>(v >> 16));
        put(static_cast<uint8_t>(v >> 24));
    }
    return *this;
}

ReportMapBuilder& ReportMapBuilder::endCollection() {
    put(0xC0);
    return *this;
}

ReportMapBuilder& ReportMapBuilder::inputPadding(uint8_t bits) {
    if (bits) {
        reportSize(bits).reportCount(1).input(Io::Constant);
    }
    return *this;
}

} // namespace phid
