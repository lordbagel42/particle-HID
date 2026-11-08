#include "harness.h"
#include "../../src/phid/status_light.h"

using namespace phid;

namespace {

HidStatus st(HidState state, bool pairing = false) {
    HidStatus s;
    s.state = state;
    s.pairing = pairing;
    return s;
}

constexpr uint32_t kBlue = StatusLight::kBlue;

} // namespace

TEST(status_light_counters_from_before_the_first_update_are_not_events) {
    StatusLight l;
    const auto o = l.update(1000, st(HidState::Advertising), 3, 7);
    CHECK(!o.show);
}

TEST(status_light_reconnect_flashes_blue_three_times_then_releases) {
    StatusLight l;
    l.update(0, st(HidState::Advertising), 1, 0);
    CHECK(!l.update(10, st(HidState::Connected), 1, 0).show); // connecting isn't an event
    auto o = l.update(100, st(HidState::Ready), 1, 0);
    CHECK(o.show);
    CHECK_EQ(o.rgb, kBlue);
    o = l.update(250, st(HidState::Ready), 1, 0);
    CHECK(o.show);
    CHECK_EQ(o.rgb, 0); // dark between flashes, not handed back
    CHECK_EQ(l.update(550, st(HidState::Ready), 1, 0).rgb, kBlue);
    CHECK(!l.update(600, st(HidState::Ready), 1, 0).show);
}

TEST(status_light_new_bond_shows_green_once) {
    StatusLight l;
    l.update(0, st(HidState::Advertising, true), 0, 0);
    l.update(10, st(HidState::Connected, true), 0, 0);
    l.update(20, st(HidState::Connected, true), 1, 0); // bonded, not subscribed yet
    auto o = l.update(30, st(HidState::Ready), 1, 0);
    CHECK_EQ(o.rgb, StatusLight::kGreen);
    CHECK_EQ(l.update(1029, st(HidState::Ready), 1, 0).rgb, StatusLight::kGreen);
    CHECK(!l.update(1030, st(HidState::Ready), 1, 0).show);
    // The next connection with the same bond is a plain reconnect.
    l.update(2000, st(HidState::Advertising), 1, 0);
    l.update(5000, st(HidState::Connected), 1, 0);
    CHECK_EQ(l.update(5010, st(HidState::Ready), 1, 0).rgb, kBlue);
}

TEST(status_light_new_bond_is_green_even_if_the_link_drops_before_ready) {
    StatusLight l;
    l.update(0, st(HidState::Connected, true), 0, 0);
    l.update(10, st(HidState::Connected, true), 1, 0);
    l.update(20, st(HidState::Advertising), 1, 0); // dropped before subscribing
    l.update(2000, st(HidState::Connected), 1, 0);
    CHECK_EQ(l.update(2010, st(HidState::Ready), 1, 0).rgb, StatusLight::kGreen);
}

TEST(status_light_error_cuts_an_event_short) {
    StatusLight l;
    l.update(0, st(HidState::Ready), 1, 0);
    CHECK(l.update(10, st(HidState::Advertising), 1, 0).show); // disconnect flashes start
    CHECK(!l.update(20, st(HidState::Error), 1, 0).show);
    CHECK(!l.update(30, st(HidState::Error), 1, 0).show);
}

TEST(status_light_disconnect_flashes_orange_only_after_a_link) {
    StatusLight l;
    l.update(0, st(HidState::Ready), 1, 0);
    auto o = l.update(100, st(HidState::Advertising), 1, 0);
    CHECK_EQ(o.rgb, StatusLight::kOrange);
    CHECK_EQ(l.update(400, st(HidState::Advertising), 1, 0).rgb, 0);
    CHECK_EQ(l.update(849, st(HidState::Advertising), 1, 0).rgb, StatusLight::kOrange);
    CHECK(!l.update(850, st(HidState::Advertising), 1, 0).show);
    // A connection that drops before it's ready is a disconnect too.
    l.update(1000, st(HidState::Connected), 1, 0);
    CHECK_EQ(l.update(1100, st(HidState::Advertising), 1, 0).rgb, StatusLight::kOrange);
}

TEST(status_light_pairing_double_blips_every_two_seconds) {
    StatusLight l;
    l.update(0, st(HidState::Off), 0, 0);
    CHECK_EQ(l.update(500, st(HidState::Advertising, true), 0, 0).rgb, kBlue);
    CHECK(l.update(650, st(HidState::Advertising, true), 0, 0).show); // dark gap
    CHECK_EQ(l.update(800, st(HidState::Advertising, true), 0, 0).rgb, kBlue);
    CHECK(!l.update(900, st(HidState::Advertising, true), 0, 0).show);
    CHECK(!l.update(2499, st(HidState::Advertising, true), 0, 0).show);
    CHECK_EQ(l.update(2550, st(HidState::Advertising, true), 0, 0).rgb, kBlue);
    // A connected host during the pairing window isn't "pairing mode".
    CHECK(!l.update(4500, st(HidState::Connected, true), 0, 0).show);
}

TEST(status_light_failure_interrupts_pairing_then_pairing_resumes) {
    StatusLight l;
    l.update(0, st(HidState::Advertising, true), 0, 0);
    auto o = l.update(1000, st(HidState::Advertising, true), 0, 1);
    CHECK_EQ(o.rgb, StatusLight::kRed);
    CHECK_EQ(l.update(1399, st(HidState::Advertising, true), 0, 1).rgb, StatusLight::kRed);
    CHECK(!l.update(1400, st(HidState::Advertising, true), 0, 1).show);
    CHECK_EQ(l.update(2050, st(HidState::Advertising, true), 0, 1).rgb, kBlue); // pairing phase kept
}

TEST(status_light_steady_mode_stays_on_while_ready) {
    StatusLight rgb, pin(true);
    rgb.update(0, st(HidState::Connected), 1, 0);
    pin.update(0, st(HidState::Connected), 1, 0);
    rgb.update(10, st(HidState::Ready), 1, 0);
    pin.update(10, st(HidState::Ready), 1, 0);
    CHECK(!rgb.update(5000, st(HidState::Ready), 1, 0).show);
    const auto o = pin.update(5000, st(HidState::Ready), 1, 0);
    CHECK(o.show);
    CHECK_EQ(o.rgb, kBlue);
    CHECK(!pin.update(9000, st(HidState::Error), 1, 0).show);
}
