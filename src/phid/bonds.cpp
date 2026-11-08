#include "bonds.h"

#include <cstring>

namespace phid {

namespace {

constexpr uint32_t kMagic = 0x44494850; // "PHID"
constexpr uint16_t kFormat = 1;
constexpr size_t kPayload = 192;

struct Writer {
    uint8_t* p;
    void u8(uint8_t v) {
        *p++ = v;
    }
    void u16(uint16_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(uint32_t v) {
        u16(v);
        u16(v >> 16);
    }
    void bytes(const uint8_t* d, size_t n) {
        memcpy(p, d, n);
        p += n;
    }
};

struct Reader {
    const uint8_t* p;
    uint8_t u8() {
        return *p++;
    }
    uint16_t u16() {
        uint16_t v = u8();
        return v | (uint16_t)u8() << 8;
    }
    uint32_t u32() {
        uint32_t v = u16();
        return v | (uint32_t)u16() << 16;
    }
    void bytes(uint8_t* d, size_t n) {
        memcpy(d, p, n);
        p += n;
    }
};

bool sameAddr(const Bond& b, uint8_t type, const uint8_t addr[6]) {
    return b.addrType == type && memcmp(b.addr, addr, 6) == 0;
}

bool isIdentityType(uint8_t type) {
    return type == AddrType::Public || type == AddrType::RandomStatic;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

size_t encodeBond(const Bond& b, uint8_t* out) {
    memset(out, 0, kBondRecordSize);
    Writer w{out};
    w.u32(kMagic);
    w.u16(kFormat);
    w.u16(kPayload);
    uint8_t* payload = w.p;
    w.u8(b.addrType);
    w.bytes(b.addr, 6);
    w.u8((b.hasIrk ? 1 : 0) | (b.lesc ? 2 : 0) | (b.authenticated ? 4 : 0));
    w.bytes(b.irk, 16);
    w.bytes(b.ltk, 16);
    w.u8(b.ltkLen);
    w.u16(b.ediv);
    w.bytes(b.rand, 8);
    w.u32(b.profileHash);
    w.u32(b.lastUsed);
    w.u8(b.sysLen);
    w.bytes(b.sys, Bond::kSysAttrMax);
    w.u8(b.usrLen);
    w.bytes(b.usr, Bond::kUsrAttrMax);
    // 189 bytes used; the rest of the payload is reserved (zero).
    w.p = payload + kPayload;
    w.u32(crc32(out, 8 + kPayload));
    return kBondRecordSize;
}

bool decodeBond(const uint8_t* in, size_t len, Bond* out) {
    if (len != kBondRecordSize) {
        return false;
    }
    Reader r{in};
    if (r.u32() != kMagic || r.u16() != kFormat || r.u16() != kPayload) {
        return false;
    }
    Reader c{in + 8 + kPayload};
    if (c.u32() != crc32(in, 8 + kPayload)) {
        return false;
    }
    Bond b;
    b.used = true;
    b.addrType = r.u8();
    r.bytes(b.addr, 6);
    const uint8_t flags = r.u8();
    b.hasIrk = flags & 1;
    b.lesc = flags & 2;
    b.authenticated = flags & 4;
    r.bytes(b.irk, 16);
    r.bytes(b.ltk, 16);
    b.ltkLen = r.u8();
    b.ediv = r.u16();
    r.bytes(b.rand, 8);
    b.profileHash = r.u32();
    b.lastUsed = r.u32();
    b.sysLen = r.u8();
    r.bytes(b.sys, Bond::kSysAttrMax);
    b.usrLen = r.u8();
    r.bytes(b.usr, Bond::kUsrAttrMax);
    if (b.ltkLen == 0 || b.ltkLen > 16 || b.sysLen > Bond::kSysAttrMax || b.usrLen > Bond::kUsrAttrMax) {
        return false;
    }
    *out = b;
    return true;
}

// Bluetooth Core Vol 3 Part H 2.2.2: ah(k, r) = e(k, r') mod 2^24. SoftDevice keys and
// addresses are LSB-first while e() is MSB-first, hence the reversals (same as nRF5 Peer Manager).
bool rpaMatches(const uint8_t irk[16], const uint8_t addr[6], AesEcbFn aes) {
    if ((addr[5] & 0xC0) != 0x40) {
        return false; // not a resolvable private address
    }
    uint8_t key[16];
    uint8_t block[16] = {};
    uint8_t enc[16];
    for (size_t i = 0; i < 16; ++i) {
        key[i] = irk[15 - i];
    }
    for (size_t i = 0; i < 3; ++i) {
        block[15 - i] = addr[3 + i]; // prand
    }
    if (!aes(key, block, enc)) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (enc[15 - i] != addr[i]) { // hash
            return false;
        }
    }
    return true;
}

size_t BondTable::count() const {
    size_t n = 0;
    for (size_t i = 0; i < cap_; ++i) {
        n += slots_[i].used;
    }
    return n;
}

int BondTable::findByAddress(uint8_t addrType, const uint8_t addr[6], AesEcbFn aes) const {
    for (size_t i = 0; i < cap_; ++i) {
        const Bond& b = slots_[i];
        if (!b.used) {
            continue;
        }
        if (isIdentityType(addrType) && sameAddr(b, addrType, addr)) {
            return (int)i;
        }
        if (addrType == AddrType::RandomResolvable && b.hasIrk && aes && rpaMatches(b.irk, addr, aes)) {
            return (int)i;
        }
    }
    return -1;
}

int BondTable::findByMasterId(uint16_t ediv, const uint8_t rand[8]) const {
    static const uint8_t zero[8] = {};
    if (ediv == 0 && memcmp(rand, zero, 8) == 0) {
        return -1; // LESC links have no master ID
    }
    for (size_t i = 0; i < cap_; ++i) {
        const Bond& b = slots_[i];
        if (b.used && !b.lesc && b.ediv == ediv && memcmp(b.rand, rand, 8) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int BondTable::allocate(uint8_t addrType, const uint8_t addr[6], bool evict) const {
    int freeSlot = -1;
    int oldest = -1;
    for (size_t i = 0; i < cap_; ++i) {
        const Bond& b = slots_[i];
        if (b.used && isIdentityType(addrType) && sameAddr(b, addrType, addr)) {
            return (int)i; // re-pairing host replaces its old bond
        }
        if (!b.used && freeSlot < 0) {
            freeSlot = (int)i;
        }
        if (b.used && (oldest < 0 || (int32_t)(b.lastUsed - slots_[oldest].lastUsed) < 0)) {
            oldest = (int)i;
        }
    }
    if (freeSlot >= 0) {
        return freeSlot;
    }
    return evict ? oldest : -1;
}

void BondTable::touch(size_t i) {
    uint32_t newest = clock_;
    for (size_t k = 0; k < cap_; ++k) {
        if (slots_[k].used && (int32_t)(slots_[k].lastUsed - newest) > 0) {
            newest = slots_[k].lastUsed;
        }
    }
    clock_ = newest + 1;
    slots_[i].lastUsed = clock_;
}

} // namespace phid
