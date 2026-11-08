#pragma once

#include <cstddef>
#include <cstdint>

namespace phid {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

// SoftDevice address types.
namespace AddrType {
constexpr uint8_t Public = 0;
constexpr uint8_t RandomStatic = 1;
constexpr uint8_t RandomResolvable = 2;
constexpr uint8_t RandomNonResolvable = 3;
} // namespace AddrType

// Everything needed to recognize a host and resume an encrypted link with it.
struct Bond {
    static constexpr size_t kSysAttrMax = 32;  // system services (Service Changed CCCD)
    static constexpr size_t kUsrAttrMax = 96;  // our CCCDs

    bool used = false;
    uint8_t addrType = 0;  // identity address
    uint8_t addr[6] = {};
    bool hasIrk = false;
    uint8_t irk[16] = {};
    // Our LTK, which the host uses to re-encrypt (as with nRF5 Peer Manager's own_ltk).
    uint8_t ltk[16] = {};
    uint8_t ltkLen = 0;
    bool lesc = false;
    bool authenticated = false;
    uint16_t ediv = 0;
    uint8_t rand[8] = {};
    uint32_t profileHash = 0;
    uint32_t lastUsed = 0; // LRU counter
    uint8_t sysLen = 0;
    uint8_t sys[kSysAttrMax] = {};
    uint8_t usrLen = 0;
    uint8_t usr[kUsrAttrMax] = {};
};

// On-flash record: magic, format, payload, CRC-32. Little-endian throughout.
constexpr size_t kBondRecordSize = 4 + 2 + 2 + 192 + 4;
size_t encodeBond(const Bond& bond, uint8_t* out);                  // returns kBondRecordSize
bool decodeBond(const uint8_t* in, size_t len, Bond* out);          // false on bad magic, version, length, or CRC

// AES-128 ECB with big-endian key and block, as Bluetooth's e() function.
using AesEcbFn = bool (*)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// True when addr (SoftDevice LSB-first) is a resolvable private address generated from irk (LSB-first).
bool rpaMatches(const uint8_t irk[16], const uint8_t addr[6], AesEcbFn aes);

class BondTable {
public:
    static constexpr size_t kMaxBonds = 8;

    void setCapacity(size_t n) {
        cap_ = n == 0 ? 1 : n > kMaxBonds ? kMaxBonds : n;
    }
    size_t capacity() const {
        return cap_;
    }
    Bond& slot(size_t i) {
        return slots_[i];
    }
    const Bond& slot(size_t i) const {
        return slots_[i];
    }
    size_t count() const;

    // Index of the bond for a peer address, resolving private addresses with IRKs; -1 if none.
    int findByAddress(uint8_t addrType, const uint8_t addr[6], AesEcbFn aes) const;
    // Index of the legacy bond whose EDIV/Rand match; -1 if none or if they are all zero.
    int findByMasterId(uint16_t ediv, const uint8_t rand[8]) const;
    // Slot to store a new bond for this identity: the existing one, a free one,
    // or the least recently used one when evict is true. -1 when full and !evict.
    int allocate(uint8_t addrType, const uint8_t addr[6], bool evict) const;
    void touch(size_t i);
    void clear(size_t i) {
        slots_[i] = Bond();
    }

private:
    Bond slots_[kMaxBonds];
    size_t cap_ = 4;
    uint32_t clock_ = 0;
};

} // namespace phid
