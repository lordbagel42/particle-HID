#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/phid/engine.h"

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases();
extern int failures;
extern uint32_t now;

struct Register {
    Register(const char* n, void (*f)()) {
        cases().push_back({n, f});
    }
};

#define TEST(name)                                    \
    static void name();                               \
    static test::Register reg_##name(#name, &name);   \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++test::failures;                                                    \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const long long va_ = (long long)(a), vb_ = (long long)(b);                                 \
        if (va_ != vb_) {                                                                           \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++test::failures;                                                                       \
        }                                                                                           \
    } while (0)

// Records every report; can pretend the radio is busy or a report is unsubscribed.
struct FakeTransport : phid::Transport {
    struct Sent {
        size_t device;
        bool boot;
        std::vector<uint8_t> data;
    };
    std::vector<Sent> sent;
    int busyEvery = 0; // every Nth send returns Busy
    int calls = 0;

    phid::SendResult sendInput(size_t device, bool boot, const uint8_t* d, size_t n) override {
        ++calls;
        if (busyEvery && calls % busyEvery == 0) {
            return phid::SendResult::Busy;
        }
        sent.push_back({device, boot, std::vector<uint8_t>(d, d + n)});
        return phid::SendResult::Ok;
    }
};

// Pumps until nothing is pending (Busy just retries, like a TX-complete wake).
inline void drain(phid::HidEngine& e, FakeTransport& t) {
    for (int i = 0; i < 100000 && e.anyPending(); ++i) {
        e.service(now, t);
    }
}

inline void readyAll(phid::HidEngine& e) {
    for (size_t i = 0; i < e.deviceCount(); ++i) {
        e.setSubscribed(i, true);
    }
    e.setReady(true);
}

} // namespace test
