#include "harness.h"
#include "../../src/phid/gamepad.h"
#include "../../src/phid/keyboard.h"
#include "../../src/phid/keyboard_layout.h"
#include "../../src/phid/mouse.h"
#include "../../src/phid/usage_keys.h"

using namespace phid;

// HID 1.11 Appendix B.1 boot keyboard descriptor.
static const uint8_t kBootKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

// HID 1.11 Appendix B.2 boot mouse descriptor.
static const uint8_t kBootMouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0};

TEST(parser_reads_hid_spec_boot_descriptors) {
    DescriptorSummary k = parseDescriptor(kBootKeyboard, sizeof(kBootKeyboard));
    CHECK(k.error == DescriptorError::None);
    CHECK_EQ(k.reportCount, 1);
    CHECK_EQ(k.reports[0].id, 0);
    CHECK_EQ(k.reports[0].inputBits, 64);
    CHECK_EQ(k.reports[0].outputBits, 8);
    DescriptorSummary m = parseDescriptor(kBootMouse, sizeof(kBootMouse));
    CHECK(m.error == DescriptorError::None);
    CHECK_EQ(m.reports[0].inputBits, 24);
}

TEST(parser_rejects_broken_descriptors) {
    CHECK(parseDescriptor(kBootKeyboard, sizeof(kBootKeyboard) - 1).error == DescriptorError::UnbalancedCollection);
    const uint8_t truncated[] = {0x05, 0x01, 0x26, 0xFF};
    CHECK(parseDescriptor(truncated, sizeof(truncated)).error == DescriptorError::Truncated);
    const uint8_t idZero[] = {0xA1, 0x01, 0x85, 0x00, 0xC0};
    CHECK(parseDescriptor(idZero, sizeof(idZero)).error == DescriptorError::ReportIdZero);
    // Report without ID, then a report with ID 2.
    const uint8_t mixed[] = {0xA1, 0x01, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x85, 0x02, 0x81, 0x02, 0xC0};
    CHECK(parseDescriptor(mixed, sizeof(mixed)).error == DescriptorError::MixedReportIds);
    const uint8_t extraEnd[] = {0xA1, 0x01, 0xC0, 0xC0};
    CHECK(parseDescriptor(extraEnd, sizeof(extraEnd)).error == DescriptorError::UnbalancedCollection);
}

TEST(builder_encodes_signed_extents_and_wide_usages) {
    uint8_t buf[32];
    ReportMapBuilder b(buf, sizeof(buf));
    b.logicalMax(255).logicalMin(-127).usage(0x238).logicalMax(127).logicalMin(-32767);
    const uint8_t want[] = {0x26, 0xFF, 0x00, 0x15, 0x81, 0x0A, 0x38, 0x02, 0x25, 0x7F, 0x16, 0x01, 0x80};
    CHECK_EQ(b.size(), sizeof(want));
    CHECK(memcmp(buf, want, sizeof(want)) == 0);
    uint8_t tiny[2];
    ReportMapBuilder t(tiny, sizeof(tiny));
    t.logicalMax(255);
    CHECK(t.overflowed());
}

TEST(composite_profile_report_ids_and_lengths) {
    BTKeyboard kb;
    BTMouse mouse;
    BTMediaKeys media;
    BTSystemControl sys;
    BTGamepad pad;
    HidEngine e;
    CHECK(e.add(kb) && e.add(mouse) && e.add(media) && e.add(sys) && e.add(pad));
    CHECK(e.buildProfile() == ProfileError::None);
    const DescriptorSummary& s = e.summary();
    CHECK_EQ(s.reportCount, 5);
    CHECK_EQ(s.inputBytes(1), 8);
    CHECK_EQ(s.outputBytes(1), 1);
    CHECK_EQ(s.inputBytes(2), 7);
    CHECK_EQ(s.inputBytes(3), 2);
    CHECK_EQ(s.inputBytes(4), 1);
    CHECK_EQ(s.inputBytes(5), 2 + 1 + 8 + 2);
    CHECK(e.reportMapLength() <= HidEngine::kMaxReportMap);
}

TEST(every_gamepad_layout_matches_its_descriptor) {
    int checked = 0;
    for (uint8_t buttons = 0; buttons <= 32; ++buttons) {
        for (uint8_t sticks = 0; sticks <= 2; ++sticks) {
            for (int hat = 0; hat < 2; ++hat) {
                for (int trig = 0; trig < 2; ++trig) {
                    GamepadLayout l = GamepadLayout().buttons(buttons).sticks(sticks).hat(hat).triggers(trig);
                    if (l.buttonCount == 0 && !l.hasHat && !l.stickCount && !l.hasTriggers) {
                        continue;
                    }
                    BTGamepad pad(l);
                    HidEngine e;
                    e.add(pad);
                    const ProfileError err = e.buildProfile();
                    if (err != ProfileError::None) {
                        std::printf("  layout b=%d s=%d h=%d t=%d -> %d\n", buttons, sticks, hat, trig, (int)err);
                    }
                    CHECK(err == ProfileError::None);
                    uint8_t out[32];
                    CHECK_EQ(pad.buildReport(out, false), pad.inputLength());
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 100);
}

TEST(profile_rejects_two_keyboards) {
    BTKeyboard a, b;
    HidEngine e;
    e.add(a);
    e.add(b);
    CHECK(e.buildProfile() == ProfileError::DuplicateBootRole);
    HidEngine empty;
    CHECK(empty.buildProfile() == ProfileError::NoDevices);
}

TEST(us_layout_spot_checks) {
    struct {
        char c;
        uint8_t key, mods;
    } cases[] = {
        {'a', 0x04, 0}, {'A', 0x04, Mod::LeftShift}, {'z', 0x1D, 0}, {'1', 0x1E, 0}, {'0', 0x27, 0},
        {'!', 0x1E, Mod::LeftShift}, {'@', 0x1F, Mod::LeftShift}, {')', 0x27, Mod::LeftShift},
        {' ', 0x2C, 0}, {'~', 0x35, Mod::LeftShift}, {'`', 0x35, 0}, {'"', 0x34, Mod::LeftShift},
        {'?', 0x38, Mod::LeftShift}, {'\n', 0x28, 0}, {'\t', 0x2B, 0}, {'\r', 0, 0}, {(char)0x80, 0, 0},
    };
    for (auto& c : cases) {
        KeyStroke k = layoutUs.map(c.c);
        if (k.key != c.key || k.mods != c.mods) {
            std::printf("  char 0x%02x -> %02x/%02x\n", (unsigned char)c.c, k.key, k.mods);
        }
        CHECK(k.key == c.key && k.mods == c.mods);
    }
}
