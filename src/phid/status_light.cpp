#include "status_light.h"

namespace phid {

namespace {

struct Step {
    uint16_t ms;
    uint32_t rgb;
};

constexpr uint32_t kDark = 0;

// Each event plays once; the LED goes back to normal afterwards.
constexpr Step kReady[] = {{100, StatusLight::kBlue}, {100, kDark}, {100, StatusLight::kBlue}, {100, kDark},
        {100, StatusLight::kBlue}};
constexpr Step kPaired[] = {{1000, StatusLight::kGreen}};
constexpr Step kDisconnected[] = {{250, StatusLight::kOrange}, {250, kDark}, {250, StatusLight::kOrange}};
constexpr Step kFailed[] = {{80, StatusLight::kRed}, {80, kDark}, {80, StatusLight::kRed}, {80, kDark},
        {80, StatusLight::kRed}};
// Pairing repeats this every kPairingPeriod while the pairing window is open.
constexpr Step kPairing[] = {{100, StatusLight::kBlue}, {150, kDark}, {100, StatusLight::kBlue}};
constexpr uint32_t kPairingPeriod = 2000;

template <size_t N>
bool play(const Step (&steps)[N], uint32_t elapsed, uint32_t* rgb) {
    for (const Step& s : steps) {
        if (elapsed < s.ms) {
            *rgb = s.rgb;
            return true;
        }
        elapsed -= s.ms;
    }
    return false;
}

} // namespace

void StatusLight::start(Event e, uint32_t now) {
    event_ = e;
    eventAt_ = now;
}

StatusLight::Output StatusLight::update(uint32_t now, const HidStatus& s, uint32_t newBonds, uint32_t failures) {
    if (!init_) {
        init_ = true;
        state_ = s.state;
        newBonds_ = newBonds;
        failures_ = failures;
    }
    const bool linked = state_ == HidState::Connected || state_ == HidState::Ready;
    if (newBonds != newBonds_) {
        bondedThisLink_ = true;
    }
    if (failures != failures_) {
        start(Event::Failed, now);
    } else if (s.state == HidState::Ready && state_ != HidState::Ready) {
        start(bondedThisLink_ ? Event::Paired : Event::Ready, now);
        bondedThisLink_ = false;
    } else if (linked && s.state == HidState::Advertising) {
        start(Event::Disconnected, now);
    }
    if (s.state == HidState::Off || s.state == HidState::Error) {
        event_ = Event::None;
        bondedThisLink_ = false;
    }
    state_ = s.state;
    newBonds_ = newBonds;
    failures_ = failures;

    Output out;
    const uint32_t elapsed = now - eventAt_;
    bool playing = false;
    switch (event_) {
    case Event::Ready:
        playing = play(kReady, elapsed, &out.rgb);
        break;
    case Event::Paired:
        playing = play(kPaired, elapsed, &out.rgb);
        break;
    case Event::Disconnected:
        playing = play(kDisconnected, elapsed, &out.rgb);
        break;
    case Event::Failed:
        playing = play(kFailed, elapsed, &out.rgb);
        break;
    case Event::None:
        break;
    }
    if (!playing) {
        event_ = Event::None;
    }

    // While connected, only a window someone opened on purpose (there are bonds)
    // counts; with no bonds the window stays open for good and would blink forever.
    const bool linkUp = s.state == HidState::Connected || s.state == HidState::Ready;
    const bool pairing = s.pairing && (s.state == HidState::Advertising || (linkUp && s.bondCount > 0));
    if (pairing && !pairing_) {
        pairingAt_ = now;
    }
    pairing_ = pairing;

    if (playing) {
        out.show = true;
    } else if (pairing) {
        out.show = play(kPairing, (now - pairingAt_) % kPairingPeriod, &out.rgb);
    } else if (steady_ && s.state == HidState::Ready) {
        out.show = true;
        out.rgb = kBlue;
    }
    if (!out.show) {
        out.rgb = 0;
    }
    return out;
}

} // namespace phid
