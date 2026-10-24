#include "models/CwRxModel.h"
#include "models/CwDecodeSettings.h"
#include "TestSettingsProfile.h"
#include <QCoreApplication>
#include <iostream>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <numbers>

using namespace AetherSDR;

namespace {
bool require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; }
    return condition;
}

void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

QVector<float> letterV(PcmFormat format)
{
    // Independent Morse fixture: V is three dits then one dah, 20 WPM.
    QVector<float> mono(format.sampleRateHz / 2, 0);
    auto silence = [&](int units) {
        mono.resize(mono.size() + qRound(units * .06 * format.sampleRateHz));
    };
    for (int letter = 0; letter < 12; ++letter) {
        if (letter) { silence(letter % 3 ? 3 : 7); }
        for (int mark = 0; mark < 4; ++mark) {
            if (mark) { silence(1); }
            const int count = qRound((mark == 3 ? 3 : 1) * .06 * format.sampleRateHz);
            for (int i = 0; i < count; ++i) {
                const double edge = std::min({1.0, i / (.005 * format.sampleRateHz),
                                              (count - 1 - i) / (.005 * format.sampleRateHz)});
                mono.append(.5f * edge * std::sin(2 * std::numbers::pi * 600 * i / format.sampleRateHz));
            }
        }
    }
    silence(35);
    QVector<float> samples;
    samples.reserve(mono.size() * format.channels());
    for (qsizetype i = 0; i < mono.size(); ++i) {
        const float other = .1f * std::sin(2 * std::numbers::pi * 1100 * i / format.sampleRateHz);
        samples.append(mono[i] + (format.channels() == 2 ? other : 0));
        if (format.channels() == 2) { samples.append(mono[i] - other); }
    }
    return samples;
}

bool establishEstimates(CwRxModel& decoder, PcmProducer& producer)
{
    QString decoded;
    const auto collect = QObject::connect(&decoder, &CwRxModel::textDecoded, &decoder,
                                          [&](const QString& text, float) { decoded += text; });
    const auto release = qScopeGuard([collect] { QObject::disconnect(collect); });
    decoder.setPitchRange(550, 650);
    decoder.setSpeedRange(18, 22);
    decoder.start();
    // Exercise fixed24 receive ingress with the original source lease.
    // Backend switches below use the inert stub; no model is downloaded.
    const PcmFormat format{24000, PcmLayout::Stereo};
    DecoderPcmAdapter adapter;
    adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 11);
    if (!require(producer.start(PcmPurpose::Slice, 11, format),
                 "Selected PCM producer did not start")) { return false; }
    const QVector<float> samples = letterV(format);
    constexpr qsizetype chunk = 2400 * 2;
    for (qsizetype offset = 0; offset < samples.size(); offset += chunk) {
        const auto frame = producer.produce(samples.mid(offset, chunk));
        if (!require(frame.has_value(), "Morse PCM frame rejected")) { return false; }
        const auto block = adapter.accept(*frame);
        if (!require(block.has_value(), "Morse PCM conversion rejected")) { return false; }
        decoder.feedFixed24(*block);
        pump(10);
    }
    pump(300);
    return require(decoded.count('V') >= 3, "Real fixed24 worker did not decode Morse V")
        && require(decoder.estimatedPitch() > 550 && decoder.estimatedPitch() < 650
                       && decoder.estimatedSpeed() > 0,
                   "Worker did not establish positive pitch and speed");
}

bool retainLockedEstimates()
{
    CwRxModel decoder;
    PcmProducer producer;  // the lease must stay current while locking
    if (!establishEstimates(decoder, producer)) { return false; }
    decoder.lockPitch(true);
    decoder.lockSpeed(true);
    const float pitch = decoder.estimatedPitch();
    const float speed = decoder.estimatedSpeed();
    // Switching away and back must not discard the operator's locks. Uses the
    // inert test backend rather than DeepFist so this runs in the default
    // build — the property belongs to CwRxModel, not to any one backend.
    if (!require(decoder.selectBackend("stub") && !decoder.isRunning(),
                 "Second-backend selection started an inactive worker")
        || !require(decoder.selectBackend("ggmorse") && !decoder.isRunning(),
                    "GGMorse selection started an inactive worker")
        || !require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                    "Backend switch discarded locked pitch or speed")) { return false; }
    decoder.start();
    decoder.reset();
    if (!require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                 "Reset discarded locked pitch or speed")) { return false; }
    decoder.stop();
    decoder.start();
    if (!require(decoder.estimatedPitch() == pitch && decoder.estimatedSpeed() == speed,
                 "Restart discarded locked pitch or speed")) { return false; }
    decoder.lockPitch(false);
    decoder.lockSpeed(false);
    decoder.stop();
    pump(20);
    return require(decoder.estimatedPitch() == 0 && decoder.estimatedSpeed() == 0,
                   "Unlocked stopped decoder kept old estimates");
}

bool sameRange(CwDecodeSettings::Range got, int low, int high)
{
    return got.low == low && got.high == high;
}

// A saved decoder range reaches the slider and the decoder only as an ordered
// band inside the slider span; anything else falls back to the default.
bool savedRangesStayInsideTheSliders()
{
    auto& s = AppSettings::instance();
    auto store = [&](const char* json) { s.setValue("CwDecoder", QString::fromLatin1(json)); };
    // CONSTRUCTED blobs: they exercise the read-side validation only.
    store(R"({"rx":"True","tx":"False"})");
    if (!require(sameRange(CwDecodeSettings::pitchRange(), 500, 700)
                 && sameRange(CwDecodeSettings::speedRange(), 15, 40),
                 "Missing ranges did not read as the defaults")) { return false; }
    store(R"({"pitchLow":100,"pitchHigh":5000,"speedLow":0,"speedHigh":99})");
    if (!require(sameRange(CwDecodeSettings::pitchRange(), 300, 1200)
                 && sameRange(CwDecodeSettings::speedRange(), 5, 60),
                 "Out-of-span ranges were not clamped to the slider spans")) { return false; }
    store(R"({"pitchLow":900,"pitchHigh":600,"speedLow":"fast","speedHigh":30})");
    if (!require(sameRange(CwDecodeSettings::pitchRange(), 500, 700)
                 && sameRange(CwDecodeSettings::speedRange(), 15, 40),
                 "Inverted or non-numeric ranges did not fall back")) { return false; }

    store(R"({"rx":"False","tx":"True","liveContacts":"True"})");
    CwDecodeSettings::setPitchRange(651, 954);
    CwDecodeSettings::setSpeedRange(20, 30);
    const bool kept = require(sameRange(CwDecodeSettings::pitchRange(), 651, 954)
                              && sameRange(CwDecodeSettings::speedRange(), 20, 30)
                              && !CwDecodeSettings::rxEnabled() && CwDecodeSettings::txEnabled()
                              && CwDecodeSettings::liveContactsEnabled(),
                              "Saving a range lost it or another CW decoder setting");
    store(R"({"rx":"True","tx":"False"})");
    return kept;
}

bool unlockAfterStopClearsReadout()
{
    // The CW panel shows the last statsUpdated. A stopped decoder keeps its
    // locked values on screen; unlocking them must publish the cleared
    // estimate, or a dead reading stays up (#5745).
    float shownPitch = -1;  // declared before the decoder so they outlive it
    float shownSpeed = -1;
    CwRxModel decoder;
    PcmProducer producer;  // the lease must stay current while locking
    if (!establishEstimates(decoder, producer)) { return false; }
    decoder.lockPitch(true);
    decoder.lockSpeed(true);
    decoder.stop();
    QObject::connect(&decoder, &CwRxModel::statsUpdated, &decoder,
                     [&](float pitch, float speed) { shownPitch = pitch; shownSpeed = speed; });
    pump(20);
    if (!require(shownPitch > 550 && shownSpeed > 0, "Stopped decoder did not show its locked values")) {
        return false;
    }
    decoder.lockPitch(false);
    pump(20);
    if (!require(shownPitch == 0 && shownSpeed > 0, "Unlocking pitch on a stopped decoder left it on screen")) {
        return false;
    }
    decoder.lockSpeed(false);
    pump(20);
    return require(shownPitch == 0 && shownSpeed == 0,
                   "Unlocking speed on a stopped decoder left it on screen");
}

bool sameRange(std::optional<CwDecodeSettings::Range> got, int low, int high)
{
    return got && sameRange(*got, low, high);
}

// The pitch range follows the radio's CW pitch: kept when it contains the
// pitch, re-centred on the pitch when it misses it.
bool pitchRangeFollowsTheRadioPitch()
{
    using S = CwDecodeSettings;
    // CONSTRUCTED pitches: they exercise the centring and the span edges only.
    if (!require(sameRange(S::pitchRangeAround(800), 650, 950)
                 && sameRange(S::pitchRangeAround(250), 300, 600)
                 && sameRange(S::pitchRangeAround(1150), 900, 1200)
                 && sameRange(S::pitchRangeAround(1500), 900, 1200),
                 "The range around a pitch is not 300 Hz inside the slider span")) { return false; }

    const S::Range fixedDefault = S::kPitchDefault;
    // MEASURED (#6158, reporter's support bundle 2026-10-04: `transmit … pitch=710`,
    // no saved range) — the radio's pitch sits outside the 500-700 default.
    if (!require(sameRange(S::pitchRangeFor(710, fixedDefault), 560, 860),
                 "A pitch outside the range did not re-centre on it")) { return false; }
    // MEASURED (#6650 review, NF0T, FLEX-8400 fw 4.3.16, 2026-10-23): range set by
    // hand to 545-700, then CW pitch 600 -> 610 — the hand-set range must stay.
    if (!require(!S::pitchRangeFor(610, S::Range{545, 700}),
                 "A pitch change inside a hand-set range replaced it")) { return false; }
    // CONSTRUCTED values from here: the range edges only.
    if (!require(!S::pitchRangeFor(500, fixedDefault) && !S::pitchRangeFor(700, fixedDefault),
                 "A pitch on the range edge replaced it")) { return false; }
    return require(sameRange(S::pitchRangeFor(499, fixedDefault), 349, 649)
                   && sameRange(S::pitchRangeFor(701, fixedDefault), 551, 851),
                   "A pitch just outside the range did not re-centre on it");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("cw-rx-model"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    if (!savedRangesStayInsideTheSliders()) { return 1; }
    if (!pitchRangeFollowsTheRadioPitch()) { return 1; }
    CwDecodeSettings::setBackend(QStringLiteral("unknown"));
    if (!require(!CwDecodeSettings::neuralSelected(),
                 "Unknown saved backend disables ggmorse controls")) { return 1; }
    for (const QString& key : CwRxModel::availableBackends()) {
        CwDecodeSettings::setBackend(key);
        if (!require(CwDecodeSettings::neuralSelected() == (key != QLatin1String("ggmorse")),
                     "Saved backend gating disagrees with the decoder catalog")) { return 1; }
    }
    CwDecodeSettings::setBackend(QStringLiteral("ggmorse"));
    QTemporaryDir cache;
    qputenv("AETHER_DEEPFIST_MODEL_DIR", cache.path().toUtf8());
    CwRxModel decoder;
    if (!require(decoder.backendKey() == "ggmorse" && decoder.supportsTuning()
                 && !decoder.isRunning(), "Default RX backend changed")) { return 1; }
    decoder.start();
    if (!require(decoder.isRunning(), "ggmorse did not start")) { return 1; }
    if (!require(!decoder.selectBackend("unknown") && decoder.isRunning()
                 && decoder.backendKey() == "ggmorse", "Invalid selection interrupted RX")) { return 1; }
    decoder.reset();
    if (!require(decoder.isRunning(), "Reset disabled RX")) { return 1; }
    decoder.stop();
    decoder.reset();
    if (!require(!decoder.isRunning(), "Reset enabled stopped RX")) { return 1; }
#ifdef HAVE_DEEPFIST
    if (!require(decoder.selectBackend("deepfist") && !decoder.supportsTuning()
                 && !decoder.isRunning(), "DeepFist selection failed")) { return 1; }
    if (!require(decoder.selectBackend("ggmorse") && decoder.supportsTuning(),
                 "Cannot return to ggmorse")) { return 1; }
#else
    if (!require(!decoder.selectBackend("deepfist")
                 && CwRxModel::availableBackends() == QStringList{"ggmorse"},
                 "Unavailable backend advertised")) { return 1; }
#endif
    int neutral = 0;
    QObject::connect(&decoder, &CwRxModel::statsUpdated, &app,
                     [&](float pitch, float speed) { if (pitch == 0 && speed == 0) { ++neutral; } });
    decoder.start();
    decoder.stop();
    QCoreApplication::processEvents();
    if (!require(neutral > 0, "Stop did not publish neutral RX readings")) { return 1; }

    // A state observer can replace the operation or destroy its facade. The
    // superseded callback must not continue publishing a second notification.
    auto disposable = std::make_unique<CwRxModel>();
    int statuses = 0;
    QObject::connect(disposable.get(), &CwRxModel::statusChanged, &app, [&] { ++statuses; });
    QObject::connect(disposable.get(), &CwRxModel::statsUpdated, &app,
                     [&](float, float) { disposable.reset(); });
    disposable->reset();
    QCoreApplication::processEvents();
    if (!require(!disposable && statuses == 0, "State notification survived facade deletion")) { return 1; }
    // A state observer replacing the backend mid-notification must not let the
    // superseded backend keep publishing. Also on the test backend so it runs
    // without the optional experiment.
    CwRxModel nested;
    bool replaced = false;
    QObject::connect(&nested, &CwRxModel::statsUpdated, &app, [&](float, float) {
        if (!replaced) { replaced = true; nested.selectBackend("ggmorse"); }
    });
    QObject::connect(&nested, &CwRxModel::statusChanged, &app, [&] {
        if (nested.backendKey() != "ggmorse") { ++statuses; }
    });
    nested.selectBackend("stub");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    if (!require(replaced && nested.backendKey() == "ggmorse" && statuses == 0,
                 "Superseded backend state continued publishing")) { return 1; }

    // The statusChanged relay is a QueuedConnection, and disconnecting a
    // backend does not retract an event already posted for it. So: make the
    // backend post one, supersede it before the loop runs, and require that the
    // stale post is dropped. Only the generation check can do that.
    CwRxModel superseded;
    int posts = 0;
    QObject::connect(&superseded, &CwRxModel::statusChanged, &app, [&] { ++posts; });
    superseded.selectBackend("stub");
    QCoreApplication::processEvents();
    const int settled = posts;
    superseded.start();                 // stub emits statusChanged -> event queued
    superseded.selectBackend("ggmorse");  // bumps the generation first
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    if (!require(posts == settled + 1,
                 "Stale queued statusChanged survived the backend switch")) { return 1; }
    return require(!decoder.isRunning(), "Stopped decoder restarted") && retainLockedEstimates()
        && unlockAfterStopClearsReadout() ? 0 : 1;
}
