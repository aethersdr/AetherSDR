// The remote-engine backend's socket-free pieces (docs/aetherd-remote-backend.md
// §2): the client bucket and its size from welcome.limits, latest-wins per
// resource and method with one request in flight per resource, the resource ->
// seam mapping (Hz/MHz, panId, audio, AGC, receive-only capabilities), and the
// WireGuard admission check for ws://, with an injected interface lookup.
#include "core/backends/aetherd/RemoteEngineMapping.h"
#include "core/backends/aetherd/RemoteEngineScheduler.h"
#ifdef HAVE_WEBSOCKETS
#include "core/backends/aetherd/RemoteEngineBackend.h"
#endif

#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::aetherd;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

void bucketSize()
{
    const auto limits = RemoteEngineScheduler::clientLimits(
        {{"requestsPerSecond", 100}, {"requestBurst", 200}});
    check(limits.requestsPerSecond == 50.0 && limits.requestBurst == 50.0,
          "the client bucket is half the advertised rate, burst 50 at 100/200");
    check(RemoteEngineScheduler::clientLimits({{"requestsPerSecond", 100}, {"requestBurst", 40}})
              == RemoteEngineScheduler::Limits{50.0, 20.0},
          "and never more than half the advertised burst");
    check(RemoteEngineScheduler::clientLimits({}) == RemoteEngineScheduler::Limits{50.0, 50.0},
          "missing limits fall back to 50/s, burst 50");
    check(RemoteEngineScheduler::clientLimits({{"requestsPerSecond", -1}, {"requestBurst", 200}})
              == RemoteEngineScheduler::Limits{50.0, 50.0},
          "nonsense limits fall back too");
}

void bucketCharges()
{
    qint64 now = 0;
    RemoteEngineScheduler scheduler([&now] { return now; });
    scheduler.setLimits({50.0, 50.0});
    for (int i = 0; i < 60; ++i) { scheduler.enqueuePlain("resource.subscribe", {}); }
    int sent = 0;
    while (scheduler.takeNext()) { ++sent; }
    check(sent == 50, "plain frames (subscribes, re-reads) draw on the bucket: a burst of 50");
    check(scheduler.msUntilReady() == 20, "the next token is 20 ms away at 50/s");
    now += 20;
    check(scheduler.takeNext().has_value() && !scheduler.takeNext().has_value(),
          "one token per 20 ms after the burst");
    now += 10'000;
    scheduler.clear();
    check(!scheduler.msUntilReady().has_value(), "nothing waiting, nothing scheduled");
}

void latestWins()
{
    qint64 now = 0;
    RemoteEngineScheduler scheduler([&now] { return now; });
    const QString slice = resourceKey("slice", "radio-1", "0");
    const QString pan = resourceKey("panadapter", "radio-1", "p0");
    for (int hz = 1; hz <= 100; ++hz) {
        scheduler.submitIntent(slice, "slice.setFrequency", {{"hz", 14'000'000 + hz}});
    }
    scheduler.submitIntent(slice, "slice.setMode", {{"mode", "CW"}});
    scheduler.submitIntent(pan, "panadapter.setCenter", {{"hz", 14'100'000}});
    check(scheduler.queuedCount() == 3, "100 frequency intents keep only the newest, beside the mode");
    const auto first = scheduler.takeNext();
    check(first && first->method == "slice.setFrequency" && first->params.value("hz").toInt() == 14'000'100,
          "the oldest pending method goes first, carrying the newest value");
    const auto second = scheduler.takeNext();
    check(second && second->resourceKey == pan, "the slice is in flight, so the pan goes next");
    check(!scheduler.takeNext().has_value(), "the mode waits: one request in flight per resource");
    check(scheduler.intentOutstanding(slice, "slice.setFrequency")
              && scheduler.intentOutstanding(slice, "slice.setMode"),
          "both slice methods count as outstanding (echo held back)");
    scheduler.completed(slice);
    check(!scheduler.intentOutstanding(slice, "slice.setFrequency"), "a reply settles the frequency");
    const auto third = scheduler.takeNext();
    check(third && third->method == "slice.setMode", "the mode is not lost to later frequency intents");
}

void sliceMapping()
{
    const QJsonObject slice{
        {"id", "0"}, {"letter", "A"}, {"panadapterId", "0x40000000"},
        {"frequencyHz", 14'074'000}, {"mode", "USB"},
        {"filter", QJsonObject{{"lowHz", 100}, {"highHz", 2900}}},
        {"active", true}, {"txSlice", true}, {"locked", false},
        {"audio", QJsonObject{{"gain", 63}, {"pan", 50}, {"muted", false}}},
        {"receive", QJsonObject{{"antenna", "ANT1"}, {"rfGain", 0},
            {"agc", QJsonObject{{"mode", "med"}, {"threshold", 72}, {"offLevel", 10}}},
            {"squelch", QJsonObject{{"enabled", false}, {"level", 20}}}}},
        {"receiveObservation", QJsonObject{
            {"noiseBlanker", QJsonObject{{"known", true}, {"value", true}, {"authority", "engine"}}},
            {"noiseReduction", QJsonObject{{"known", false}, {"value", QJsonValue::Null}}},
            {"rit", QJsonObject{{"known", true}, {"enabled", true}, {"offsetHz", -120}}}}}};
    const SliceDelta d = sliceDeltaFrom(slice, {"USB", "LSB", "CW"});
    check(d.frequency && std::abs(*d.frequency - 14.074) < 1e-12, "14 074 000 Hz becomes 14.074 MHz");
    check(d.panId == QString("0x40000000"), "the slice carries its panId (the model's tune gate needs it)");
    check(d.audioGain == 63.0 && d.audioMute == false, "audio gain and mute are carried");
    check(d.agcMode == QString("med") && d.agcThreshold == 72 && d.agcOffLevel == 10, "AGC is carried");
    check(d.filterLow == 100 && d.filterHigh == 2900 && d.mode == QString("USB"), "mode and filter");
    check(d.nb == true && !d.nr.has_value(), "an unknown observation stays absent");
    check(d.ritOn == true && d.ritFreq == -120, "RIT is carried");
    check(d.txSlice == false, "a remote slice is never the TX slice");
    check(d.modeList == QStringList({"USB", "LSB", "CW"}), "the mode list is the engine's");
    check(!canonicalSliceId("07") && !canonicalSliceId("-1") && canonicalSliceId("7") == 7,
          "slice ids must be canonical");
}

void panMapping()
{
    const auto state = panStateFrom({{"centerHz", 7'100'000}, {"bandwidthHz", 192'000},
        {"bandwidthLimitsHz", QJsonObject{{"minimum", 48'000}, {"maximum", 1'536'000}}},
        {"receive", QJsonObject{{"rfGain", -12}}}});
    check(state && std::abs(state->centerMhz - 7.1) < 1e-12 && std::abs(state->bandwidthMhz - 0.192) < 1e-12,
          "pan center and bandwidth in MHz");
    check(state && state->rfGain == -12, "RF gain keeps its sign (ANAN attenuation is negative)");
    check(state && state->minimumBandwidthMhz == 0.048 && state->maximumBandwidthMhz == 1.536,
          "bandwidth limits in MHz");
    check(!panStateFrom({{"centerHz", 7'100'000}, {"bandwidthHz", 0}}), "a zero bandwidth is not a pan");
}

void capabilities()
{
    const QJsonObject session{
        {"identity", QJsonObject{{"model", "ANAN-G2"}, {"manufacturer", "Apache Labs"}}},
        {"capabilities", QJsonObject{
            {"maxSlices", 1}, {"maxPanadapters", 1}, {"canTransmit", true},
            {"sliceFrequencyControl", QJsonObject{{"authority", "engine"}, {"minimumHz", 10'000}, {"maximumHz", 61'440'000}}},
            {"receiveModeControl", QJsonObject{{"authority", "engine"}, {"modes", QJsonArray{"USB", "LSB"}}}},
            {"receiveFilterControl", QJsonValue::Null},
            {"receiveDspControl", QJsonObject{{"authority", "engine"}, {"maximumRitOffsetHz", 9999},
                {"features", QJsonObject{{"agcThreshold", QJsonArray{"USB", "LSB"}}, {"rit", QJsonArray{"USB"}}}}}},
            {"receivePanRfGainControl", QJsonObject{{"authority", "engine"}, {"minimumDb", -31}, {"maximumDb", 0}, {"stepDb", 1}}},
            {"radioSpeakerControl", QJsonObject{{"authority", "engine"}}}}}};
    const RadioCapabilities c = capabilitiesFrom(session, "med");
    check(c.family == "aetherd" && c.model == "ANAN-G2", "the family is aetherd; the model is the engine's radio");
    check(!c.canTransmit && c.txPowerMaxWatts == 0.0, "never transmit, whatever the engine says");
    check(c.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "no client settings domains: nothing is restored to the engine (rule 1)");
    check(c.sliceFrequencyControl.authority == SliceFrequencyControl::Authority::Engine
              && c.sliceFrequencyControl.maximumHz == 61'440'000,
          "frequency control mirrors the engine's record");
    check(c.receiveModeControl && c.receiveModeControl->modes == QStringList({"USB", "LSB"}), "modes mirror");
    check(!c.receiveFilterControl && !c.receiveAudioControl && !c.receivePanCenterControl,
          "a record the engine does not send stays absent");
    check(c.receiveDspControl && c.receiveDspControl->supports(ReceiveDspControl::Feature::Agc, "LSB")
              && c.receiveDspControl->supports(ReceiveDspControl::Feature::Rit, "USB")
              && !c.receiveDspControl->supports(ReceiveDspControl::Feature::Rit, "LSB")
              && !c.receiveDspControl->supports(ReceiveDspControl::Feature::Nb, "USB"),
          "DSP features keep their per-mode restriction");
    check(c.hasAgcThreshold && c.agcModes == QStringList({"med"}),
          "AGC: the threshold, and only the running mode is offered");
    check(c.receivePanRfGainControl && c.receivePanRfGainControl->minimumDb == -31, "RF gain range mirrors");
    check(c.radioSpeakerControl.has_value(), "the radio speaker mirrors");
    const RadioCapabilities empty = capabilitiesFrom({}, {});
    check(empty.family == "aetherd" && !empty.canTransmit && !empty.receiveModeControl && empty.agcModes.isEmpty(),
          "before the first snapshot nothing is offered");
}

void tailnetAdmission()
{
#ifdef HAVE_WEBSOCKETS
    const auto names = [](const QHostAddress& address) -> QString {
        if (address == QHostAddress("100.101.102.103")) { return "tailscale0"; }
        if (address == QHostAddress("10.8.0.2")) { return "wg0"; }
        if (address == QHostAddress("192.168.50.10")) { return "eth0"; }
        return {};
    };
    check(RemoteEngineBackend::tailnetPathAdmitted(QHostAddress("100.101.102.103"), names),
          "ws:// is admitted from a tailscale interface");
    check(RemoteEngineBackend::tailnetPathAdmitted(QHostAddress("10.8.0.2"), names),
          "and from a wg interface");
    check(!RemoteEngineBackend::tailnetPathAdmitted(QHostAddress("192.168.50.10"), names),
          "never from a LAN interface");
    check(!RemoteEngineBackend::tailnetPathAdmitted(QHostAddress("100.64.9.9"), names),
          "an address on no known interface is refused");
    check(!RemoteEngineBackend::tailnetPathAdmitted(QHostAddress(), names), "nor a null address");
#else
    std::printf("[SKIP] no Qt WebSockets: the backend is not built\n");
#endif
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bucketSize();
    bucketCharges();
    latestWins();
    sliceMapping();
    panMapping();
    capabilities();
    tailnetAdmission();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
