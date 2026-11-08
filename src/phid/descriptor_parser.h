#pragma once

#include <cstddef>
#include <cstdint>

namespace phid {

struct ReportInfo {
    uint8_t id;
    uint16_t inputBits;
    uint16_t outputBits;
    uint16_t featureBits;
};

enum class DescriptorError : uint8_t {
    None = 0,
    Truncated,            // an item runs past the end of the buffer
    UnbalancedCollection, // End Collection without Collection, or missing End Collection
    MainOutsideCollection,
    ReportIdZero,         // Report ID 0 is reserved
    MixedReportIds,       // some main items have a Report ID and some don't
    TooManyReports,
    StackError,           // Push/Pop mismatch
};

struct DescriptorSummary {
    static constexpr size_t kMaxReports = 16;
    DescriptorError error = DescriptorError::None;
    size_t errorOffset = 0;
    ReportInfo reports[kMaxReports] = {};
    size_t reportCount = 0;

    const ReportInfo* find(uint8_t id) const;
    // Byte lengths without the Report ID prefix, rounded up.
    size_t inputBytes(uint8_t id) const;
    size_t outputBytes(uint8_t id) const;
};

// Parses a HID report descriptor and totals the bits of every input, output,
// and feature report. This is an independent check that the GATT report
// characteristics match the Report Map the host will read.
DescriptorSummary parseDescriptor(const uint8_t* data, size_t len);

} // namespace phid
