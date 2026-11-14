// Data2gSession (RFC #7159, Stage 2): the connected-mode adapter over the
// vendored Data2G ARQ engine, worked against a second station.
//
// The other station is the vendored core's own Engine as W1AW. The two are
// joined by a clean audio channel with a little noise: the adapter at
// AetherModem's 24 kHz in uneven chunks, the other station at the core's
// 8 kHz. No radio and no sound card are involved. A transmission the adapter
// hands back is played into the channel by this test and nowhere else.

#include "core/hfdata/Data2gSession.h"

#include "arq/engine.hpp"
#include "arq/session.hpp"
#include "audio/filters.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using AetherSDR::hfdata::Data2gBandwidth;
using AetherSDR::hfdata::Data2gLinkState;
using AetherSDR::hfdata::Data2gSession;
using AetherSDR::hfdata::Data2gSessionStep;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

bool has(const std::vector<std::string>& events, const std::string& prefix)
{
    return std::any_of(events.begin(), events.end(),
                       [&](const std::string& event) { return event.rfind(prefix, 0) == 0; });
}

int count(const std::vector<std::string>& events, const std::string& prefix)
{
    return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const std::string& event) {
        return event.rfind(prefix, 0) == 0;
    }));
}

std::vector<std::uint8_t> bytesOf(const std::string& text)
{
    return {text.begin(), text.end()};
}

// The adapter ("ours") and another station ("theirs") on one channel.
struct Link {
    static constexpr int kRate = 24000;
    static constexpr double kLevel = 0.1;   // what each station hears of the other
    static constexpr double kNoise = 0.002;
    // None a multiple of the 3:1 decimation except the first.
    static constexpr std::size_t kChunks[] = {2400, 479, 2401, 7, 1024, 4801, 257, 1};

    Data2gSession ours{"N0CALL", kRate, 17};
    data2g::arq::Engine theirs;
    data2g::audio::Decimator toTheirs{kRate};
    data2g::audio::Interpolator toOurs{kRate};
    std::mt19937 rng{11};
    std::normal_distribution<double> noise{0.0, kNoise};

    std::deque<float> onAirFromOurs;     // a handed-back transmission, playing out
    std::deque<double> onAirFromTheirs;  // at 24 kHz
    std::deque<double> onAirFromThird;   // at 24 kHz: a third station, heard by ours only
    std::size_t turn{0};
    std::int64_t samples{0};

    std::vector<std::string> ourEvents;
    std::vector<std::string> theirEvents;
    std::vector<std::uint8_t> ourReceived;
    std::vector<std::uint8_t> theirReceived;
    Data2gLinkState ourState{Data2gLinkState::Idle};
    int transmissions{0};
    double transmissionPeak{0.0};
    std::size_t shortestTransmission{SIZE_MAX};
    bool sawAbort{false};

    static data2g::arq::EngineConfig theirConfig()
    {
        data2g::arq::EngineConfig config;
        config.seed = 23;
        return config;
    }

    explicit Link(bool theyListen)
        : theirs("W1AW", theirConfig())
    {
        if (theyListen)
            theirs.listen(true);
    }

    double seconds() const { return static_cast<double>(samples) / kRate; }

    // A third station, K1ABC, sends one CONNECT addressed to ours.
    void thirdStationCalls()
    {
        data2g::arq::EngineConfig config;
        config.seed = 31;
        data2g::arq::Engine third("K1ABC", config);
        third.connect("N0CALL", 2);
        data2g::audio::Interpolator to24k(kRate);
        const std::vector<double> quiet(data2g::config::FS / 10, 0.0);
        for (int block = 0; block < 100; ++block) {
            const data2g::arq::Engine::Out out = third.step(quiet);
            if (!out.tx_burst)
                continue;
            const std::vector<double> at24k = to24k(*out.tx_burst);
            onAirFromThird.assign(at24k.begin(), at24k.end());
            return;
        }
    }

    void step()
    {
        const std::size_t n = kChunks[turn++ % std::size(kChunks)];

        std::vector<float> weHear(n);
        for (float& v : weHear) {
            double tx = 0.0;
            if (!onAirFromTheirs.empty()) {
                tx = onAirFromTheirs.front();
                onAirFromTheirs.pop_front();
            }
            if (!onAirFromThird.empty()) {
                tx += onAirFromThird.front();
                onAirFromThird.pop_front();
            }
            v = static_cast<float>(kLevel * tx + noise(rng));
        }
        Data2gSessionStep ourOut = ours.step(weHear);
        ourState = ourOut.state;
        std::move(ourOut.events.begin(), ourOut.events.end(), std::back_inserter(ourEvents));
        ourReceived.insert(ourReceived.end(), ourOut.received.begin(), ourOut.received.end());
        if (ourOut.abortTx) {
            sawAbort = true;
            onAirFromOurs.clear();   // what a transmitter does with abortTx
        }
        if (!ourOut.transmission.empty()) {
            ++transmissions;
            shortestTransmission = std::min(shortestTransmission, ourOut.transmission.size());
            for (float v : ourOut.transmission)
                transmissionPeak = std::max(transmissionPeak, static_cast<double>(std::fabs(v)));
            onAirFromOurs.insert(onAirFromOurs.end(), ourOut.transmission.begin(), ourOut.transmission.end());
        }

        std::vector<double> theyHear(n);
        for (double& v : theyHear) {
            double tx = 0.0;
            if (!onAirFromOurs.empty()) {
                tx = onAirFromOurs.front();
                onAirFromOurs.pop_front();
            }
            v = kLevel * tx + noise(rng);
        }
        const std::vector<double> atFs = toTheirs(theyHear);
        if (!atFs.empty()) {
            const data2g::arq::Engine::Out theirOut = theirs.step(atFs);
            for (std::string& event : theirs.events())
                theirEvents.push_back(std::move(event));
            const data2g::arq::Bytes got = theirs.session().read();
            theirReceived.insert(theirReceived.end(), got.begin(), got.end());
            const std::vector<double> at24k = toOurs(theirOut.audio);
            onAirFromTheirs.insert(onAirFromTheirs.end(), at24k.begin(), at24k.end());
        }
        samples += static_cast<std::int64_t>(n);
    }

    template <typename Done>
    bool runUntil(double limitSeconds, Done done)
    {
        const double end = seconds() + limitSeconds;
        while (seconds() < end) {
            step();
            if (done())
                return true;
        }
        return false;
    }

    void run(double forSeconds)
    {
        runUntil(forSeconds, [] { return false; });
    }
};

// Connect, data both ways, a graceful disconnect with its closing
// identification, a refusal dropped inside the closing window, and silence
// afterwards.
void session()
{
    Link link(true);
    check(link.ours.isValid(), "the session adapter is valid at 24 kHz");

    link.run(2.0);
    check(link.transmissions == 0, "nothing is handed back before the operator connects");

    link.ours.requestConnect("W1AW", Data2gBandwidth::Hz2400);
    const bool connected = link.runUntil(60.0, [&] {
        return has(link.ourEvents, "CONNECTED") && has(link.theirEvents, "CONNECTED");
    });
    check(connected, "both stations report CONNECTED");
    check(has(link.ourEvents, "CONNECTED W1AW"), "the adapter names its peer");
    check(link.ourState == Data2gLinkState::Connected, "the adapter's state is Connected");
    check(link.transmissions > 0, "the adapter handed back transmissions for its own session");
    check(link.transmissionPeak > 0.9 && link.transmissionPeak < 1.1,
          "a transmission's audio peaks at full scale");
    check(link.shortestTransmission > static_cast<std::size_t>(Link::kRate / 5),
          "a transmission is a whole burst, not a chunk");

    const std::string toThem = "Hello W1AW, this is the AetherSDR session test.\r";
    const std::string toUs = "Hello N0CALL, copied.\r";
    link.ours.send(bytesOf(toThem));
    link.theirs.session().write(bytesOf(toUs));
    const bool exchanged = link.runUntil(120.0, [&] {
        return link.theirReceived.size() >= toThem.size() && link.ourReceived.size() >= toUs.size();
    });
    check(exchanged, "data arrives in both directions");
    check(std::string(link.theirReceived.begin(), link.theirReceived.end()) == toThem,
          "the other station received exactly what was sent");
    check(std::string(link.ourReceived.begin(), link.ourReceived.end()) == toUs,
          "the adapter received exactly what the other station sent");

    // A third station calls ours in the middle of the session. The engine
    // answers that with a BUSY refusal; it is not the session's, so it is
    // dropped, and the session carries on.
    link.run(8.0);   // let the exchange's last acknowledgements go
    link.thirdStationCalls();
    check(!link.onAirFromThird.empty(), "the third station put a CONNECT on the air");
    const bool refusalDropped = link.runUntil(30.0, [&] { return link.ours.suppressedTransmissions() > 0; });
    check(refusalDropped, "the refusal to a third station mid-session was dropped");
    check(link.ourState == Data2gLinkState::Connected, "the session survives a third station's call");
    const std::string more = "Still here.\r";
    link.ours.send(bytesOf(more));
    const bool stillFlows = link.runUntil(60.0, [&] { return link.theirReceived.size() >= toThem.size() + more.size(); });
    check(stillFlows, "data still flows after the dropped refusal");
    const std::uint64_t droppedInSession = link.ours.suppressedTransmissions();

    const int idsBefore = count(link.theirEvents, "ID N0CALL");
    link.ours.requestDisconnect();
    const bool closed = link.runUntil(120.0, [&] {
        return has(link.ourEvents, "DISCONNECTED") && has(link.theirEvents, "DISCONNECTED");
    });
    check(closed, "both stations report DISCONNECTED");
    const bool identified = link.runUntil(Data2gSession::kClosingIdSeconds + 10.0, [&] {
        return count(link.theirEvents, "ID N0CALL") > idsBefore;
    });
    check(identified, "the other station heard the closing identification");
    check(link.ours.suppressedTransmissions() == droppedInSession,
          "nothing of the session's own was dropped");

    // The other station calls back once the session is over.
    const int sentBeforeCall = link.transmissions;
    link.theirs.abort();
    link.theirs.connect("N0CALL", 2);
    link.run(60.0);
    check(link.transmissions == sentBeforeCall, "a call after the session is not answered");
    check(link.ours.suppressedTransmissions() > droppedInSession, "that refusal was dropped too");
    check(link.ourState == Data2gLinkState::Idle, "no session starts from the other side");
    check(link.ours.faultCount() == 0, "no core faults");
}

// A stranger calls a station whose operator has not connected to anyone: the
// refusal the engine would send is dropped, and no session starts.
void strangerCalls()
{
    Link link(false);
    link.run(1.0);
    link.theirs.connect("N0CALL", 2);
    link.run(40.0);
    check(link.transmissions == 0, "an unarmed station hands back nothing");
    check(!has(link.ourEvents, "CONNECTED"), "an incoming CONNECT starts no session");
    check(link.ourState == Data2gLinkState::Idle, "the adapter stays idle");
    check(link.ours.suppressedTransmissions() > 0, "the engine's refusal was dropped, and counted");
}

// Abort mid-session: transmit stops at once and nothing follows, the closing
// identification included.
void abortMidSession()
{
    Link link(true);
    link.ours.requestConnect("W1AW", Data2gBandwidth::Hz500);
    const bool connected = link.runUntil(90.0, [&] { return has(link.ourEvents, "CONNECTED"); });
    check(connected, "connected at 500 Hz before the abort");
    link.ours.send(std::vector<std::uint8_t>(2000, 0x55));
    link.run(3.0);

    link.ours.requestAbort();
    link.step();
    check(link.sawAbort, "the step after an abort says to stop transmitting");
    check(link.ourState == Data2gLinkState::Idle, "the adapter is idle at once");
    const int sentAtAbort = link.transmissions;
    link.run(30.0);
    check(link.transmissions == sentAtAbort, "nothing is handed back after an abort");
}

// Requests the engine cannot honour are reported, not thrown.
void refusals()
{
    Link link(true);
    link.ours.send(bytesOf("nobody is listening"));
    link.ours.requestConnect("NOT A CALL SIGN!", Data2gBandwidth::Hz2400);
    link.run(0.5);
    check(has(link.ourEvents, "REFUSED not connected"), "data with no session is refused");
    check(count(link.ourEvents, "REFUSED") == 2, "a call sign that will not pack is refused");
    check(link.transmissions == 0, "a refused connect hands back nothing");

    link.ours.requestConnect("W1AW", Data2gBandwidth::Hz2400);
    link.ours.requestConnect("W1AW", Data2gBandwidth::Hz2400);
    link.run(0.5);
    check(count(link.ourEvents, "REFUSED") == 3, "a second connect during a session is refused");

    Data2gSession badRate("N0CALL", 44100);
    check(!badRate.isValid(), "an unsupported rate leaves the adapter invalid");
    badRate.requestConnect("W1AW", Data2gBandwidth::Hz2400);
    const std::vector<float> silence(2400, 0.0f);
    const Data2gSessionStep out = badRate.step(silence);
    check(out.transmission.empty() && has(out.events, "REFUSED"),
          "an invalid adapter refuses and stays silent");

    Data2gSession badCall("NOT A CALL SIGN!", 24000);
    check(!badCall.isValid(), "a station call that will not pack leaves the adapter invalid");
}

// The key lead is silence at the head of each transmission.
void keyLead()
{
    Link link(true);
    link.ours.setKeyLeadSeconds(0.3);
    std::vector<float> first;
    link.ours.requestConnect("W1AW", Data2gBandwidth::Hz2400);
    const std::vector<float> quiet(2400, 0.0f);
    for (int i = 0; i < 100 && first.empty(); ++i)
        first = link.ours.step(quiet).transmission;
    check(!first.empty(), "a connect produces a transmission");
    const auto firstLoud = std::find_if(first.begin(), first.end(),
                                        [](float v) { return std::fabs(v) > 0.01f; });
    const double leadSeconds = static_cast<double>(std::distance(first.begin(), firstLoud)) / Link::kRate;
    check(leadSeconds > 0.28 && leadSeconds < 0.32, "the transmission opens with the key lead's silence");
}

} // namespace

int main()
{
    session();
    strangerCalls();
    abortMidSession();
    refusals();
    keyLead();
    if (g_failures) {
        std::fprintf(stderr, "data2g_session_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("ok: data2g_session_test");
    return 0;
}
