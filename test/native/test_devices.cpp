#include <random>
#include <set>

#include "harness.h"
#include "../../src/phid/gamepad.h"
#include "../../src/phid/keyboard.h"
#include "../../src/phid/mouse.h"
#include "../../src/phid/usage_keys.h"

using namespace phid;
using test::FakeTransport;

namespace {

std::vector<uint8_t> kbReport(uint8_t mods, std::initializer_list<uint8_t> keys) {
    std::vector<uint8_t> r(8, 0);
    r[0] = mods;
    size_t i = 2;
    for (uint8_t k : keys) {
        r[i++] = k;
    }
    return r;
}

std::set<uint8_t> keysOf(const std::vector<uint8_t>& r) {
    std::set<uint8_t> s;
    for (int bit = 0; bit < 8; ++bit) {
        if (r[0] & (1 << bit)) {
            s.insert(0xE0 + bit);
        }
    }
    for (size_t i = 2; i < 8; ++i) {
        if (r[i]) {
            s.insert(r[i]);
        }
    }
    return s;
}

int16_t le16(const std::vector<uint8_t>& r, size_t i) {
    return (int16_t)(r[i] | (r[i + 1] << 8));
}

} // namespace

TEST(keyboard_tap_with_shift_is_four_ordered_reports) {
    BTKeyboard kb;
    HidEngine e;
    e.add(kb);
    CHECK(e.buildProfile() == ProfileError::None);
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t); // sync report
    t.sent.clear();
    CHECK(kb.tap(Key::A, Mod::LeftShift));
    test::drain(e, t);
    CHECK_EQ(t.sent.size(), 4);
    if (t.sent.size() == 4) {
        CHECK(t.sent[0].data == kbReport(0x02, {}));
        CHECK(t.sent[1].data == kbReport(0x02, {0x04}));
        CHECK(t.sent[2].data == kbReport(0x02, {}));
        CHECK(t.sent[3].data == kbReport(0x00, {}));
    }
}

TEST(keyboard_busy_radio_loses_nothing) {
    BTKeyboard kb;
    HidEngine e;
    e.add(kb);
    e.buildProfile();
    test::readyAll(e);
    FakeTransport t;
    t.busyEvery = 2;
    CHECK_EQ(kb.type("hello"), 5);
    test::drain(e, t);
    // 5 taps * 2 reports; the Ready sync rides on the first one
    CHECK_EQ(t.sent.size(), 10);
    std::string typed;
    for (size_t i = 0; i + 1 < t.sent.size(); i += 2) {
        const uint8_t k = t.sent[i].data[2];
        typed += (char)('a' + k - 0x04);
    }
    CHECK(typed == "hello");
}

TEST(keyboard_releases_always_fit_when_queue_is_full) {
    BTKeyboard kb;
    HidEngine e;
    e.add(kb);
    e.buildProfile();
    // Not ready: nothing drains. Hold 4 modifiers and 6 keys, then fill with taps.
    for (uint8_t m = 0xE0; m < 0xE4; ++m) {
        CHECK(kb.press(m));
    }
    for (uint8_t k = 0x04; k < 0x0A; ++k) {
        CHECK(kb.press(k));
    }
    CHECK(!kb.press((uint8_t)0x0A)); // 7th key: rollover
    CHECK(kb.release((uint8_t)0x09)); // make room for one more key
    // Fill the queue with press/release pairs; the last accepted press must still be releasable.
    int pairs = 0;
    while (kb.press(Key::F1)) {
        CHECK(kb.release(Key::F1));
        ++pairs;
    }
    CHECK(pairs > 0);
    CHECK(kb.stats().rejectedFull > 0);
    CHECK(!kb.tap(Key::F2));
    kb.releaseAll(); // must all fit
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t);
    CHECK(!t.sent.empty());
    CHECK(t.sent.back().data == kbReport(0, {}));
}

// Random presses, releases, taps, busy radio and disconnects. Invariants:
// each report changes at most one key, and after releaseAll the host ends with nothing held.
TEST(keyboard_random_storm_never_sticks_keys) {
    std::mt19937 rng(1234);
    for (int round = 0; round < 50; ++round) {
        BTKeyboard kb;
        HidEngine e;
        e.add(kb);
        e.buildProfile();
        test::readyAll(e);
        FakeTransport t;
        t.busyEvery = 3;
        std::set<uint8_t> host;
        auto absorb = [&]() {
            for (auto& s : t.sent) {
                std::set<uint8_t> next = keysOf(s.data);
                size_t diff = 0;
                for (uint8_t k : next) {
                    diff += !host.count(k);
                }
                for (uint8_t k : host) {
                    diff += !next.count(k);
                }
                if (!e.ready()) {
                    continue;
                }
                CHECK(diff <= 1 || &s == &t.sent.front());
                host = next;
            }
            t.sent.clear();
        };
        for (int step = 0; step < 400; ++step) {
            const int op = rng() % 10;
            const uint8_t key = (rng() % 2) ? 0x04 + rng() % 8 : 0xE0 + rng() % 3;
            if (op < 4) {
                kb.press(key);
            } else if (op < 8) {
                kb.release(key);
            } else if (op == 8) {
                kb.tap((Key)(0x04 + rng() % 8), rng() % 2 ? Mod::LeftShift : 0);
            } else if (rng() % 8 == 0) {
                // Disconnect then reconnect: queued events drop, held keys are restated.
                e.setReady(false);
                e.dropQueued(test::now);
                test::readyAll(e);
            }
            if (rng() % 3 == 0) {
                e.service(test::now, t);
                absorb();
            }
        }
        kb.releaseAll();
        test::drain(e, t);
        absorb();
        CHECK(host.empty());
    }
}

TEST(keyboard_events_wait_for_host_then_expire_but_held_keys_survive) {
    BTKeyboard kb;
    HidEngine e;
    e.add(kb);
    e.buildProfile();
    e.readyDeadline(2000);
    FakeTransport t;
    kb.press(Key::LeftShift);
    kb.tap(Key::B);
    test::now += 1000;
    e.service(test::now, t); // still within the deadline
    CHECK_EQ(kb.stats().droppedNotReady, 0);
    test::now += 1500;
    e.service(test::now, t);
    CHECK_EQ(kb.stats().droppedNotReady, 3);
    CHECK(t.sent.empty());
    test::readyAll(e);
    test::drain(e, t);
    CHECK_EQ(t.sent.size(), 1); // sync: shift is still held
    CHECK(t.sent[0].data == kbReport(0x02, {}));
}

TEST(mouse_motion_is_conserved_and_chunked) {
    BTMouse m;
    HidEngine e;
    e.add(m);
    e.buildProfile();
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t);
    t.sent.clear();
    t.busyEvery = 4;
    std::mt19937 rng(7);
    long long wantX = 0, wantY = 0;
    for (int i = 0; i < 2000; ++i) {
        const int32_t dx = (int32_t)(rng() % 200001) - 100000;
        const int32_t dy = (int32_t)(rng() % 21) - 10;
        CHECK(m.move(dx, dy));
        wantX += dx;
        wantY += dy;
        if (rng() % 5 == 0) {
            e.service(test::now, t);
        }
    }
    test::drain(e, t);
    long long gotX = 0, gotY = 0;
    for (auto& s : t.sent) {
        CHECK(le16(s.data, 1) >= -32767 && le16(s.data, 1) <= 32767);
        gotX += le16(s.data, 1);
        gotY += le16(s.data, 3);
    }
    CHECK_EQ(gotX, wantX);
    CHECK_EQ(gotY, wantY);
}

TEST(mouse_click_lands_between_the_moves_around_it) {
    BTMouse m;
    HidEngine e;
    e.add(m);
    e.buildProfile();
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t);
    t.sent.clear();
    m.move(10, 0);
    m.move(5, 1);
    m.click(MouseButton::Left);
    m.move(-3, 0);
    test::drain(e, t);
    CHECK_EQ(t.sent.size(), 4);
    if (t.sent.size() == 4) {
        CHECK(t.sent[0].data[0] == 0 && le16(t.sent[0].data, 1) == 15 && le16(t.sent[0].data, 3) == 1);
        CHECK(t.sent[1].data[0] == MouseButton::Left && le16(t.sent[1].data, 1) == 0);
        CHECK(t.sent[2].data[0] == 0 && le16(t.sent[2].data, 1) == 0);
        CHECK(t.sent[3].data[0] == 0 && le16(t.sent[3].data, 1) == -3);
    }
}

TEST(mouse_boot_protocol_drops_wheel_without_stalling) {
    BTMouse m;
    HidEngine e;
    e.add(m);
    e.buildProfile();
    test::readyAll(e);
    e.setBootProtocol(true);
    FakeTransport t;
    test::drain(e, t);
    t.sent.clear();
    m.move(300, 0, 5);
    test::drain(e, t);
    CHECK(!e.anyPending());
    long long x = 0;
    for (auto& s : t.sent) {
        CHECK(s.boot && s.data.size() == 3);
        x += (int8_t)s.data[1];
    }
    CHECK_EQ(x, 300);
}

TEST(gamepad_quick_tap_between_reports_still_registers) {
    BTGamepad pad;
    HidEngine e;
    e.add(pad);
    e.buildProfile();
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t);
    t.sent.clear();
    pad.press(Button::A);
    pad.release(Button::A); // both before the radio gets a chance
    test::drain(e, t);
    CHECK_EQ(t.sent.size(), 2);
    if (t.sent.size() == 2) {
        CHECK(t.sent[0].data[0] & 0x01);
        CHECK(!(t.sent[1].data[0] & 0x01));
    }
    t.sent.clear();
    pad.stick(Stick::Left, 1.0f, -1.0f);
    pad.stick(Stick::Left, 0.5f, 0.0f); // latest state wins, no extra report
    pad.trigger(Trigger::Right, 1.0f);
    test::drain(e, t);
    CHECK_EQ(t.sent.size(), 1);
    if (t.sent.size() == 1) {
        CHECK_EQ(le16(t.sent[0].data, 3), 16383);
        CHECK_EQ(t.sent[0].data[12], 255);
        CHECK_EQ(t.sent[0].data[2], (uint8_t)Hat::Centered);
    }
    CHECK(!pad.press(17)); // only 16 buttons in the default layout
}

TEST(reports_follow_the_order_of_calls_across_devices) {
    BTKeyboard kb;
    BTMouse m;
    BTMediaKeys media;
    HidEngine e;
    e.add(kb);
    e.add(m);
    e.add(media);
    e.buildProfile();
    test::readyAll(e);
    FakeTransport t;
    test::drain(e, t);
    t.sent.clear();
    kb.press(Key::LeftCtrl);
    m.click();
    media.tap(Media::VolumeUp);
    kb.release(Key::LeftCtrl);
    test::drain(e, t);
    std::vector<size_t> order;
    for (auto& s : t.sent) {
        order.push_back(s.device);
    }
    CHECK((order == std::vector<size_t>{0, 1, 1, 2, 2, 0}));
}

TEST(unsubscribed_report_does_not_block_others) {
    BTKeyboard kb;
    BTMediaKeys media;
    HidEngine e;
    e.add(kb);
    e.add(media);
    e.buildProfile();
    e.setSubscribed(0, true);
    e.setSubscribed(1, false);
    e.setReady(true);
    FakeTransport t;
    media.tap(Media::Mute);
    kb.tap(Key::C);
    for (int i = 0; i < 10; ++i) {
        e.service(test::now, t);
    }
    size_t mediaReports = 0;
    for (auto& s : t.sent) {
        mediaReports += s.device == 1;
    }
    CHECK_EQ(mediaReports, 0);
    CHECK_EQ(t.sent.size(), 2); // the keyboard tap
    test::now += 5000;
    e.service(test::now, t);
    CHECK_EQ(media.stats().droppedNotReady, 2);
}
