#pragma once

#include <cstddef>
#include <cstdint>

namespace phid {

// Main item flags.
namespace Io {
constexpr uint8_t Data = 0x00;
constexpr uint8_t Constant = 0x01;
constexpr uint8_t Array = 0x00;
constexpr uint8_t Variable = 0x02;
constexpr uint8_t Absolute = 0x00;
constexpr uint8_t Relative = 0x04;
constexpr uint8_t NullState = 0x40;
} // namespace Io

// Writes HID short items with the smallest encoding. Logical and physical
// extents are signed in HID, so 255 is written as two bytes, not as 0xFF (-1).
class ReportMapBuilder {
public:
    ReportMapBuilder(uint8_t* buf, size_t capacity)
            : buf_(buf),
              cap_(capacity) {
    }

    ReportMapBuilder& usagePage(uint16_t v) {
        return unsignedItem(0x04, v);
    }
    ReportMapBuilder& usage(uint16_t v) {
        return unsignedItem(0x08, v);
    }
    ReportMapBuilder& usageMin(uint16_t v) {
        return unsignedItem(0x18, v);
    }
    ReportMapBuilder& usageMax(uint16_t v) {
        return unsignedItem(0x28, v);
    }
    ReportMapBuilder& logicalMin(int32_t v) {
        return signedItem(0x14, v);
    }
    ReportMapBuilder& logicalMax(int32_t v) {
        return signedItem(0x24, v);
    }
    ReportMapBuilder& physicalMin(int32_t v) {
        return signedItem(0x34, v);
    }
    ReportMapBuilder& physicalMax(int32_t v) {
        return signedItem(0x44, v);
    }
    ReportMapBuilder& unit(uint32_t v) {
        return unsignedItem(0x64, v);
    }
    ReportMapBuilder& reportSize(uint8_t v) {
        return unsignedItem(0x74, v);
    }
    ReportMapBuilder& reportId(uint8_t v) {
        return unsignedItem(0x84, v);
    }
    ReportMapBuilder& reportCount(uint8_t v) {
        return unsignedItem(0x94, v);
    }
    ReportMapBuilder& input(uint8_t flags) {
        return unsignedItem(0x80, flags);
    }
    ReportMapBuilder& output(uint8_t flags) {
        return unsignedItem(0x90, flags);
    }
    ReportMapBuilder& collection(uint8_t kind) {
        return unsignedItem(0xA0, kind);
    }
    ReportMapBuilder& endCollection();

    // Report Size/Count followed by a constant input item, for alignment.
    ReportMapBuilder& inputPadding(uint8_t bits);

    const uint8_t* data() const {
        return buf_;
    }
    size_t size() const {
        return len_;
    }
    bool overflowed() const {
        return overflow_;
    }

private:
    ReportMapBuilder& unsignedItem(uint8_t tagType, uint32_t v);
    ReportMapBuilder& signedItem(uint8_t tagType, int32_t v);
    void put(uint8_t b);

    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflow_ = false;
};

namespace Collection {
constexpr uint8_t Physical = 0x00;
constexpr uint8_t Application = 0x01;
} // namespace Collection

} // namespace phid
