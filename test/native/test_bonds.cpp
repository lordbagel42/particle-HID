#include <openssl/evp.h>

#include "harness.h"
#include "../../src/phid/bonds.h"

using namespace phid;

namespace {

bool opensslAes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1;
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    ok = ok && EVP_EncryptUpdate(ctx, out, &len, in, 16) == 1 && len == 16;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

Bond sampleBond() {
    Bond b;
    b.used = true;
    b.addrType = AddrType::Public;
    for (int i = 0; i < 6; ++i) b.addr[i] = 0x10 + i;
    b.hasIrk = true;
    for (int i = 0; i < 16; ++i) b.irk[i] = 0xA0 + i;
    for (int i = 0; i < 16; ++i) b.ltk[i] = 0x30 + i;
    b.ltkLen = 16;
    b.lesc = true;
    b.profileHash = 0xDEADBEEF;
    b.lastUsed = 42;
    b.sysLen = 6;
    for (int i = 0; i < 6; ++i) b.sys[i] = i + 1;
    b.usrLen = 22;
    for (int i = 0; i < 22; ++i) b.usr[i] = 0x80 + i;
    return b;
}

} // namespace

// Bluetooth Core v5.x Vol 3 Part H, Appendix D.7 sample data for ah().
TEST(rpa_resolves_bluetooth_spec_sample) {
    // IRK ec0234a357c8ad05341010a60a397d9b (MSB first) stored LSB first, as the SoftDevice does.
    const uint8_t irkMsb[16] = {0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
                                0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b};
    uint8_t irk[16];
    for (int i = 0; i < 16; ++i) irk[i] = irkMsb[15 - i];
    // prand 708194, hash 0dfbaa -> address 70:81:94:0d:fb:aa, LSB first.
    uint8_t addr[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};
    CHECK(rpaMatches(irk, addr, opensslAes));
    addr[0] ^= 1;
    CHECK(!rpaMatches(irk, addr, opensslAes));
    addr[0] ^= 1;
    irk[3] ^= 0x40;
    CHECK(!rpaMatches(irk, addr, opensslAes));
}

TEST(bond_record_round_trips) {
    const Bond b = sampleBond();
    uint8_t rec[kBondRecordSize];
    CHECK_EQ(encodeBond(b, rec), kBondRecordSize);
    Bond d;
    CHECK(decodeBond(rec, sizeof(rec), &d));
    CHECK(d.used && d.addrType == b.addrType && memcmp(d.addr, b.addr, 6) == 0);
    CHECK(d.hasIrk && memcmp(d.irk, b.irk, 16) == 0 && memcmp(d.ltk, b.ltk, 16) == 0);
    CHECK(d.lesc && !d.authenticated && d.ltkLen == 16);
    CHECK(d.profileHash == 0xDEADBEEF && d.lastUsed == 42);
    CHECK(d.sysLen == 6 && memcmp(d.sys, b.sys, 6) == 0);
    CHECK(d.usrLen == 22 && memcmp(d.usr, b.usr, 22) == 0);
}

TEST(bond_record_rejects_any_single_byte_corruption) {
    const Bond b = sampleBond();
    uint8_t rec[kBondRecordSize];
    encodeBond(b, rec);
    int accepted = 0;
    for (size_t i = 0; i < sizeof(rec); ++i) {
        rec[i] ^= 0x5A;
        Bond d;
        accepted += decodeBond(rec, sizeof(rec), &d);
        rec[i] ^= 0x5A;
    }
    CHECK_EQ(accepted, 0);
    Bond d;
    CHECK(!decodeBond(rec, sizeof(rec) - 1, &d));
}

TEST(crc32_matches_zlib) {
    const uint8_t s[] = "123456789";
    CHECK_EQ(crc32(s, 9), 0xCBF43926u);
}

TEST(bond_table_lookup_and_lru_eviction) {
    BondTable t;
    t.setCapacity(2);
    uint8_t a1[6] = {1, 2, 3, 4, 5, 6}, a2[6] = {6, 5, 4, 3, 2, 1}, a3[6] = {9, 9, 9, 9, 9, 9};
    int s = t.allocate(AddrType::Public, a1, false);
    CHECK_EQ(s, 0);
    t.slot(s) = sampleBond();
    memcpy(t.slot(s).addr, a1, 6);
    t.slot(s).lesc = false;
    t.slot(s).ediv = 0x1234;
    t.touch(s);
    s = t.allocate(AddrType::Public, a2, false);
    CHECK_EQ(s, 1);
    t.slot(s) = sampleBond();
    memcpy(t.slot(s).addr, a2, 6);
    t.touch(s);
    CHECK_EQ(t.findByAddress(AddrType::Public, a2, nullptr), 1);
    CHECK_EQ(t.findByAddress(AddrType::RandomStatic, a2, nullptr), -1);
    const uint8_t zero[8] = {};
    CHECK_EQ(t.findByMasterId(0x1234, t.slot(0).rand), 0);
    CHECK_EQ(t.findByMasterId(0, zero), -1);
    CHECK_EQ(t.allocate(AddrType::Public, a1, false), 0); // same host re-pairs into its own slot
    CHECK_EQ(t.allocate(AddrType::Public, a3, false), -1); // full
    t.touch(0);                                            // slot 1 is now least recently used
    CHECK_EQ(t.allocate(AddrType::Public, a3, true), 1);
}
