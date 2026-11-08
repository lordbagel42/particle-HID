#include <mutex>

#include "harness.h"
#include "../../src/phid/platform.h"

namespace test {
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
int failures = 0;
uint32_t now = 1000;
} // namespace test

namespace phid {
namespace platform {
static std::recursive_mutex g_mutex;
void lock() {
    g_mutex.lock();
}
void unlock() {
    g_mutex.unlock();
}
uint32_t millis() {
    return test::now;
}
void wake() {
}
void sleepMs(uint32_t ms) {
    test::now += ms;
}
} // namespace platform
} // namespace phid

int main() {
    int failedCases = 0;
    for (const auto& c : test::cases()) {
        const int before = test::failures;
        c.fn();
        const bool ok = test::failures == before;
        failedCases += !ok;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    }
    std::printf("%zu cases, %d failed\n", test::cases().size(), failedCases);
    return failedCases ? 1 : 0;
}
