#include "descriptor_parser.h"

namespace phid {

namespace {

struct Globals {
    uint32_t reportSize = 0;
    uint32_t reportCount = 0;
    uint8_t reportId = 0;
};

uint32_t itemValue(const uint8_t* p, size_t size) {
    uint32_t v = 0;
    for (size_t i = 0; i < size; ++i) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

} // namespace

const ReportInfo* DescriptorSummary::find(uint8_t id) const {
    for (size_t i = 0; i < reportCount; ++i) {
        if (reports[i].id == id) {
            return &reports[i];
        }
    }
    return nullptr;
}

size_t DescriptorSummary::inputBytes(uint8_t id) const {
    auto r = find(id);
    return r ? (r->inputBits + 7) / 8 : 0;
}

size_t DescriptorSummary::outputBytes(uint8_t id) const {
    auto r = find(id);
    return r ? (r->outputBits + 7) / 8 : 0;
}

DescriptorSummary parseDescriptor(const uint8_t* data, size_t len) {
    DescriptorSummary out;
    Globals g;
    Globals stack[4];
    size_t stackDepth = 0;
    int collectionDepth = 0;
    bool sawWithId = false;
    bool sawWithoutId = false;
    size_t i = 0;

    auto fail = [&](DescriptorError e, size_t offset) {
        out.error = e;
        out.errorOffset = offset;
        return out;
    };

    while (i < len) {
        const size_t start = i;
        const uint8_t prefix = data[i++];
        if (prefix == 0xFE) { // long item: bDataSize, bLongItemTag, data
            if (i + 2 > len || i + 2 + data[i] > len) {
                return fail(DescriptorError::Truncated, start);
            }
            i += 2 + data[i];
            continue;
        }
        static const uint8_t kSizes[4] = {0, 1, 2, 4};
        const size_t size = kSizes[prefix & 0x03];
        if (i + size > len) {
            return fail(DescriptorError::Truncated, start);
        }
        const uint32_t value = itemValue(data + i, size);
        i += size;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = prefix >> 4;

        if (type == 0) { // main
            if (tag == 0x0A) { // Collection
                ++collectionDepth;
                continue;
            }
            if (tag == 0x0C) { // End Collection
                if (--collectionDepth < 0) {
                    return fail(DescriptorError::UnbalancedCollection, start);
                }
                continue;
            }
            if (tag != 0x08 && tag != 0x09 && tag != 0x0B) { // Input, Output, Feature
                continue;
            }
            if (collectionDepth == 0) {
                return fail(DescriptorError::MainOutsideCollection, start);
            }
            (g.reportId ? sawWithId : sawWithoutId) = true;
            if (sawWithId && sawWithoutId) {
                return fail(DescriptorError::MixedReportIds, start);
            }
            ReportInfo* r = nullptr;
            for (size_t k = 0; k < out.reportCount; ++k) {
                if (out.reports[k].id == g.reportId) {
                    r = &out.reports[k];
                }
            }
            if (!r) {
                if (out.reportCount == DescriptorSummary::kMaxReports) {
                    return fail(DescriptorError::TooManyReports, start);
                }
                r = &out.reports[out.reportCount++];
                *r = ReportInfo{g.reportId, 0, 0, 0};
            }
            const uint32_t bits = g.reportSize * g.reportCount;
            if (tag == 0x08) {
                r->inputBits += bits;
            } else if (tag == 0x09) {
                r->outputBits += bits;
            } else {
                r->featureBits += bits;
            }
        } else if (type == 1) { // global
            switch (tag) {
            case 0x07:
                g.reportSize = value;
                break;
            case 0x08:
                if (value == 0) {
                    return fail(DescriptorError::ReportIdZero, start);
                }
                g.reportId = value;
                break;
            case 0x09:
                g.reportCount = value;
                break;
            case 0x0A: // Push
                if (stackDepth == 4) {
                    return fail(DescriptorError::StackError, start);
                }
                stack[stackDepth++] = g;
                break;
            case 0x0B: // Pop
                if (stackDepth == 0) {
                    return fail(DescriptorError::StackError, start);
                }
                g = stack[--stackDepth];
                break;
            default:
                break;
            }
        }
    }
    if (collectionDepth != 0) {
        return fail(DescriptorError::UnbalancedCollection, len);
    }
    return out;
}

} // namespace phid
