// IcomCIV — the IRadioBackend seam, against the fake IC-705.
//
// The load-bearing assertion here is the TCI/WSJT-X AUDIO CONTRACT. Everything
// downstream of this backend — the speaker, the TCI receiver channel, the
// decoders WSJT-X runs — consumes interleaved stereo float32 at 24 kHz. The
// radio delivers 48 kHz MONO. Neither half of that conversion is optional:
//
//   * skip the rate conversion and playback runs an octave low, so WSJT-X sees
//     every tone at twice its true frequency and decodes nothing;
//   * skip the channel duplication and TciServer, which divides the buffer by
//     2*sizeof(float), sees half the frames it thinks it has.
//
// Both failures are silent — audio flows, meters move, the session is healthy.
// So they get a test rather than a comment.

#include "IcomFakeRadio.h"

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMap>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <cstdio>
#include <cstring>
#include <optional>
#include <memory>

using namespace AetherSDR;
using namespace AetherSDR::icom;
using namespace AetherSDR::icom::test;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepareGeneration(IcomCivBackend& backend, std::uint64_t generation)
    {
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
    }

    static void deliver(IcomCivBackend& backend, const CivFrame& frame,
                        std::uint64_t generation)
    {
        backend.onCivFrame(frame, generation);
    }

    static void expectPttConfirmation(IcomCivBackend& backend, bool keyed)
    {
        backend.m_keyed = !keyed;
        backend.m_pendingPttIntent = keyed;
        backend.m_pendingPttUntilMs = backend.nowMs() + 1000;
    }

    static int linkPollIntervalMs(const IcomCivBackend& backend)
    {
        return backend.m_linkTimer ? backend.m_linkTimer->interval() : -1;
    }

    static bool pumpUntilIdle(IcomCivBackend& backend)
    {
        backend.pumpCiv(backend.nowMs());
        return backend.m_civScheduler.idle();
    }

    static bool recoveryActive(const IcomCivBackend& backend)
    {
        return backend.m_civRecoveryStartedAtMs > 0;
    }

    static void stopPollers(IcomCivBackend& backend)
    {
        if (backend.m_meterTimer) {
            backend.m_meterTimer->stop();
        }
        if (backend.m_linkTimer) {
            backend.m_linkTimer->stop();
        }
    }

    static void runControlPhase(IcomCivBackend& backend, int previousPhase)
    {
        backend.m_controlPollPhase = previousPhase;
        backend.onLinkTick();
    }

    static std::vector<std::vector<std::uint8_t>> queuedFrames(
        const IcomCivBackend& backend)
    {
        std::vector<std::vector<std::uint8_t>> frames;
        frames.reserve(backend.m_civScheduler.m_queue.size());
        for (const IcomCivScheduler::Queued& queued :
             backend.m_civScheduler.m_queue) {
            frames.push_back(queued.request.frame);
        }
        return frames;
    }

    static std::pair<quint64, quint64> terminalRequestCounts(
        const IcomCivBackend& backend)
    {
        return {backend.m_schedulerCancelledRequests,
                backend.m_schedulerFailedRequests};
    }

    static void queuePendingRead(IcomCivBackend& backend)
    {
        const std::vector<std::uint8_t> frame = cmdReadFrequency(kIc705Addr);
        backend.queueRead(frame, backend.semanticKey(frame),
                          IcomCivScheduler::Priority::Maintenance);
    }

    static void cancelScheduler(IcomCivBackend& backend)
    {
        backend.terminateScheduler(IcomCivScheduler::TerminalOutcome::Cancelled,
                                   IcomCivBackend::SchedulerWaiterOutcome::Cancelled);
    }

    static int recoveryIntervalMs(const IcomCivBackend& backend)
    {
        const auto recovery = profileFor(*backend.m_model).civRecovery;
        return recovery ? recovery->retryIntervalMs : -1;
    }

    static int maxRecoveryAttempts(const IcomCivBackend& backend)
    {
        const auto recovery = profileFor(*backend.m_model).civRecovery;
        return recovery ? recovery->maxAttempts : 0;
    }

    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }

    static int preampStep(const IcomCivBackend& backend)
    {
        return backend.m_preampStep;
    }

    static bool tuneActive(const IcomCivBackend& backend)
    {
        return backend.m_tuning;
    }

    static bool tuneTimerActive(const IcomCivBackend& backend)
    {
        return backend.m_tuneTimer && backend.m_tuneTimer->isActive();
    }

    static void reassertPreamp(IcomCivBackend& backend)
    {
        backend.reassertPanPreampWireStep(backend.m_preampStep);
    }

    static QVariantMap repeaterState(const IcomCivBackend& backend)
    {
        return backend.repeaterStateMap();
    }

    static QVariantList controls(const IcomCivBackend& backend)
    {
        return backend.controlMap();
    }
};

}  // namespace AetherSDR::icom

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// A frame that can MOVE the transmitter or the antenna tuner, as opposed to one
// that merely asks about them.
//
// 1C 00 and 1C 01 each have two forms and only one of them does anything. The
// payload-bearing form is the command — cmdSetPtt writes one byte (00 receive,
// 01 transmit) and cmdSetTuner writes one byte (00 off, 01 on, 02 start a
// matching cycle). The empty form is a READ, which the register answers and
// never acts on. The backend polls exactly that read on a 250 ms cadence from
// onMeterTick, deliberately: m_keyed was set only by our own setKeying() and by
// an unsolicited status frame, so a radio keyed from its own front-panel PTT
// left every transmit meter suppressed and reading "never fed".
//
// Matching on cmd+sub alone cannot tell those two forms apart, and a check that
// cannot tell them apart is not a TX-safety check. It fires on the poll — which
// keys nothing — and it would go on firing at whoever silenced it, until it got
// silenced the easy way.
static bool movesPttOrTuner(const CivFrame& f)
{
    return f.cmd == cmd::kControl && f.hasSub
        && (f.sub == control::kPtt || f.sub == control::kTuner)
        && !f.data.empty();
}

// ---------------------------------------------------------------------------
// CI-V trace capture
//
// traceCiv writes the decoded `cmd=`/`sub=` tag ONLY to qCDebug — the in-memory
// ring stores raw hex. So the tag can only be asserted by capturing log output,
// which is why this needs a message handler rather than a getter.
// ---------------------------------------------------------------------------
static QStringList g_civLines;
static bool g_capturing = false;
static QtMessageHandler g_prevHandler = nullptr;

static void civCapture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (g_capturing && ctx.category && std::strcmp(ctx.category, "aether.icom.civ") == 0)
        g_civLines << msg;
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

// The most recent captured line beginning with `prefix`, or a null string.
static QString lastLineStartingWith(const QString& prefix)
{
    for (auto it = g_civLines.crbegin(); it != g_civLines.crend(); ++it)
        if (it->startsWith(prefix))
            return *it;
    return {};
}

static void testStaleSessionFrameIsDropped()
{
    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepareGeneration(backend, 2);

    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    QSignalSpy transmitSpy(&backend, &IRadioBackend::transmitChanged);
    QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
    QSignalSpy spectrumSpy(&backend, &IRadioBackend::spectrumFrameReady);

    CivFrame frequency;
    frequency.to = kControllerAddress;
    frequency.from = kIc705Addr;
    frequency.cmd = cmd::kReadFreq;
    frequency.data = {0x00, 0x00, 0x20, 0x44, 0x01};  // 144.200 MHz BCD

    CivFrame ptt;
    ptt.to = kControllerAddress;
    ptt.from = kIc705Addr;
    ptt.cmd = cmd::kControl;
    ptt.hasSub = true;
    ptt.sub = control::kPtt;
    ptt.data = {0x01};

    CivFrame meterFrame;
    meterFrame.to = kControllerAddress;
    meterFrame.from = kIc705Addr;
    meterFrame.cmd = cmd::kMeter;
    meterFrame.hasSub = true;
    meterFrame.sub = meter::kSMeter;
    meterFrame.data = {0x01, 0x20};

    std::vector<std::uint8_t> scopeBody{
        0x00, encodeBcdByte(1), encodeBcdByte(1), 0x00,
    };
    const std::vector<std::uint8_t> centre = encodeFreq(14'100'000);
    const std::vector<std::uint8_t> span = encodeFreq(100'000);
    scopeBody.insert(scopeBody.end(), centre.begin(), centre.end());
    scopeBody.insert(scopeBody.end(), span.begin(), span.end());
    scopeBody.push_back(0x00);
    scopeBody.insert(scopeBody.end(), kScopePointsIc705, 80);
    const std::optional<CivFrame> scopeFrame = parseFrame(
        buildFrameSub(kIc705Addr, cmd::kScope, scope::kWaveData, scopeBody));
    check(scopeFrame.has_value(), "stale-generation scope fixture is valid");

    // Generation 1 represents a queued civFrameReady delivery from the session
    // that was disconnected before generation 2 became current.
    IcomCivBackendTestAccess::deliver(backend, frequency, 1);
    IcomCivBackendTestAccess::deliver(backend, ptt, 1);
    IcomCivBackendTestAccess::deliver(backend, meterFrame, 1);
    if (scopeFrame) {
        IcomCivBackendTestAccess::deliver(backend, *scopeFrame, 1);
    }
    QCoreApplication::processEvents();
    check(sliceSpy.isEmpty() && transmitSpy.isEmpty() && meterSpy.isEmpty()
              && spectrumSpy.isEmpty(),
          "a queued CI-V frame from the previous session publishes no model, meter, or scope state");

    // Controls: each frame is well-formed and publishes on its own surface when
    // tagged with the active generation. This proves every stale assertion
    // exercises the generation gate rather than a decoder rejection.
    IcomCivBackendTestAccess::deliver(backend, frequency, 2);
    // Frequency deliberately updates both SliceModel and TransmitModel. Keep
    // the PTT control assertion independent of that companion TX-frequency
    // publication.
    transmitSpy.clear();
    IcomCivBackendTestAccess::expectPttConfirmation(backend, true);
    IcomCivBackendTestAccess::deliver(backend, ptt, 2);
    IcomCivBackendTestAccess::deliver(backend, meterFrame, 2);
    if (scopeFrame) {
        IcomCivBackendTestAccess::deliver(backend, *scopeFrame, 2);
    }
    QCoreApplication::processEvents();
    check(sliceSpy.count() == 1, "the active generation publishes slice state");
    check(transmitSpy.count() == 1, "the active generation publishes transmit state");
    check(meterSpy.count() == 1, "the active generation publishes meter state");
    check(spectrumSpy.count() == 1, "the active generation publishes scope state");

}

static void testTxMeterMinimumHoldAtBackendSeam()
{
    const IcomModel* ic705 = modelForName("IC-705");
    check(ic705 != nullptr, "TX meter hold test resolves the IC-705 profile");
    if (!ic705) {
        return;
    }

    IcomCivBackend backend;
    IcomCivBackendTestAccess::selectModel(backend, *ic705);
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);

    CivFrame ptt;
    ptt.to = kControllerAddress;
    ptt.from = kIc705Addr;
    ptt.cmd = cmd::kControl;
    ptt.hasSub = true;
    ptt.sub = control::kPtt;
    ptt.data = {0x01};
    IcomCivBackendTestAccess::expectPttConfirmation(backend, true);
    IcomCivBackendTestAccess::deliver(backend, ptt, 1);

    CivFrame swr;
    swr.to = kControllerAddress;
    swr.from = kIc705Addr;
    swr.cmd = cmd::kMeter;
    swr.hasSub = true;
    swr.sub = meter::kSwr;
    swr.data = {0x00, 0x80};

    QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
    IcomCivBackendTestAccess::deliver(backend, swr, 1);
    check(meterSpy.count() == 1,
          "a real keyed IC-705 SWR reply crosses the backend seam");

    swr.data = {0x00, 0x00};
    IcomCivBackendTestAccess::deliver(backend, swr, 1);
    check(meterSpy.count() == 1,
          "an isolated keyed IC-705 SWR minimum is held below the shared meter seam");

    swr.data = {0x00, 0x82};
    IcomCivBackendTestAccess::deliver(backend, swr, 1);
    check(meterSpy.count() == 2,
          "the next real IC-705 SWR reply publishes without UI-side smoothing changes");
}

static void testPreampWireStateRemainsAuthoritative()
{
    IcomCivBackend backend;
    const IcomModel* ic9700 = modelForName("IC-9700");
    check(ic9700 != nullptr, "IC-9700 preamp test resolves its model");
    if (!ic9700) {
        return;
    }
    IcomCivBackendTestAccess::selectModel(backend, *ic9700);
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);

    CivFrame reported;
    reported.to = kControllerAddress;
    reported.from = 0xA2;
    reported.cmd = cmd::kFunction;
    reported.hasSub = true;
    reported.sub = func::kPreamp;
    reported.data = {0x02};
    IcomCivBackendTestAccess::deliver(backend, reported, 1);
    check(IcomCivBackendTestAccess::preampStep(backend) == 2,
          "IC-9700 wire state 02 is mirrored without being renamed P.AMP INT");

    IcomCivBackendTestAccess::reassertPreamp(backend);
    check(IcomCivBackendTestAccess::preampStep(backend) == 2,
          "diagnostic reassert preserves the radio-adopted wire state");

    backend.setPanPreamp(QStringLiteral("0"), 2);
    check(IcomCivBackendTestAccess::preampStep(backend) == 1,
          "operator intent remains bounded to OFF/P.AMP INT on the IC-9700");
}

static void testDestructorCancelsWaiter(QCoreApplication& app)
{
    auto backend = std::make_unique<IcomCivBackend>();
    IcomCivBackendTestAccess::prepareGeneration(*backend, 1);
    IcomCivBackendTestAccess::queuePendingRead(*backend);

    QVariantMap terminalResult;
    QObject::connect(backend.get(), &IRadioBackend::extensionResult, &app,
                     [&](quint64 requestId, const QVariant& result) {
                         if (requestId == 0xD357) {
                             terminalResult = result.toMap();
                         }
                     });
    backend->invokeExtension(QStringLiteral("icom"),
                             QStringLiteral("civ.scheduler.wait-idle"),
                             0xD357,
                             QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
    backend.reset();
    check(terminalResult.value(QStringLiteral("outcome")).toString()
              == QLatin1String("cancelled")
              && terminalResult.value(QStringLiteral("cancelled")).toBool(),
          "backend destruction explicitly cancels a pending scheduler waiter");
}

static void testTerminalWaiterReentrySurvivesReset(QCoreApplication& app)
{
    IcomCivBackend backend;
    IcomCivBackendTestAccess::prepareGeneration(backend, 1);
    IcomCivBackendTestAccess::queuePendingRead(backend);

    QVariantMap firstResult;
    QVariantMap reentrantResult;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &app,
                     [&](quint64 requestId, const QVariant& result) {
                         if (requestId == 0x5119) {
                             firstResult = result.toMap();
                             backend.invokeExtension(
                                 QStringLiteral("icom"),
                                 QStringLiteral("civ.scheduler.wait-idle"),
                                 0x5120,
                                 QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
                         } else if (requestId == 0x5120) {
                             reentrantResult = result.toMap();
                         }
                     });
    backend.invokeExtension(QStringLiteral("icom"),
                            QStringLiteral("civ.scheduler.wait-idle"),
                            0x5119,
                            QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
    IcomCivBackendTestAccess::cancelScheduler(backend);

    check(firstResult.value(QStringLiteral("outcome")).toString()
              == QLatin1String("cancelled"),
          "scheduler termination reports the original waiter as cancelled");
    check(reentrantResult.value(QStringLiteral("outcome")).toString()
              == QLatin1String("completed"),
          "a waiter registered re-entrantly after reset is not erased");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AetherSDR::SliceDelta>("SliceDelta");
    qRegisterMetaType<AetherSDR::MeterDef>("MeterDef");

    testStaleSessionFrameIsDropped();
    testTxMeterMinimumHoldAtBackendSeam();
    testPreampWireStateRemainsAuthoritative();
    testDestructorCancelsWaiter(app);
    testTerminalWaiterReentrySurvivesReset(app);

    FakeIc705 radio;
    // Same model under a harmless presentation variant. modelForName()
    // canonicalises it, while capability code that compares the display text
    // literally loses CWK.
    radio.setDeviceName("IC 705");
    radio.setSetting(setting::kNtpEnabled, 0x01);
    radio.setSettingText(setting::kNtpServer, "time.nist.gov");
    radio.setSetting(setting::kGpsTimeCorrect, 0x01);
    radio.setGpsSource(0x01);
    radio.setGpsPosition({
        0x34, 0x13, 0x46, 0x40, 0x01,
        0x01, 0x18, 0x03, 0x53, 0x40, 0x00,
        0x01, 0x74, 0x25, 0x00,
        0x02, 0x75,
        0x00, 0x04, 0x27,
        0x20, 0x26, 0x08, 0x21, 0x12, 0x34, 0x56,
    });
    IcomCivBackend backend;

    int sliceAudioBuffers = 0;
    qint64 sliceAudioBytes = 0;
    int speakerBuffers = 0;
    QObject::connect(&backend, &IRadioBackend::sliceAudioFrameReady, &app,
                     [&](int, const QByteArray& pcm) {
                         ++sliceAudioBuffers;
                         sliceAudioBytes += pcm.size();
                     });
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &app,
                     [&](const QByteArray&) { ++speakerBuffers; });

    // ACCUMULATED, not last-wins. Each delta carries only the fields that
    // moved, so a plain "keep the newest" would show one setting and forget the
    // nine that arrived in their own frames a millisecond earlier.
    SliceDelta lastSliceState;
    TransmitDelta lastTransmitState;
    std::vector<QString> publishedModes;
    // Every mode LIST the backend published, in order — the vocabulary the mode
    // combo is built from (#5040). Accumulated rather than last-wins because the
    // list is republished when the identity changes, and an empty one is a real
    // answer (an identity withdrawn), not an absent field.
    std::vector<QStringList> publishedModeLists;
    // Every mox edge in order. The PTT confirmation window is defined by which
    // transitions it lets through, so the SEQUENCE is the assertion, not the
    // final value — a suppressed-then-corrected state and a never-suppressed
    // one both end up in the same place.
    std::vector<bool> moxPublications;
    std::vector<bool> xfcPublications;
    GpsDelta gpsState;
    QObject::connect(&backend, &IRadioBackend::gpsChanged, &app,
                     [&](const GpsDelta& d) {
                         if (d.status) { gpsState.status = d.status; }
                         if (d.positionValid) { gpsState.positionValid = d.positionValid; }
                         if (d.source) { gpsState.source = d.source; }
                         if (d.grid) { gpsState.grid = d.grid; }
                         if (d.altitude) { gpsState.altitude = d.altitude; }
                         if (d.lat) { gpsState.lat = d.lat; }
                         if (d.lon) { gpsState.lon = d.lon; }
                         if (d.time) { gpsState.time = d.time; }
                         if (d.date) { gpsState.date = d.date; }
                         if (d.speed) { gpsState.speed = d.speed; }
                         if (d.track) { gpsState.track = d.track; }
                         if (d.ntpEnabled) { gpsState.ntpEnabled = d.ntpEnabled; }
                         if (d.ntpServer) { gpsState.ntpServer = d.ntpServer; }
                         if (d.gpsTimeCorrectionEnabled) {
                             gpsState.gpsTimeCorrectionEnabled = d.gpsTimeCorrectionEnabled;
                         }
                         if (d.ntpSyncStatus) { gpsState.ntpSyncStatus = d.ntpSyncStatus; }
                     });
    const auto mergeSlice = [&lastSliceState](const SliceDelta& d) {
        if (d.nr) lastSliceState.nr = d.nr;
        if (d.nb) lastSliceState.nb = d.nb;
        if (d.anf) lastSliceState.anf = d.anf;
        if (d.mn) lastSliceState.mn = d.mn;
        if (d.agcMode) lastSliceState.agcMode = d.agcMode;
        if (d.nrLevel) lastSliceState.nrLevel = d.nrLevel;
        if (d.nbLevel) lastSliceState.nbLevel = d.nbLevel;
        if (d.audioGain) lastSliceState.audioGain = d.audioGain;
        if (d.ritOn) lastSliceState.ritOn = d.ritOn;
        if (d.xitOn) lastSliceState.xitOn = d.xitOn;
        if (d.ritFreq) lastSliceState.ritFreq = d.ritFreq;
        if (d.mode) lastSliceState.mode = d.mode;
        if (d.filterLow) lastSliceState.filterLow = d.filterLow;
        if (d.filterHigh) lastSliceState.filterHigh = d.filterHigh;
        if (d.fmToneMode) lastSliceState.fmToneMode = d.fmToneMode;
        if (d.fmToneValue) lastSliceState.fmToneValue = d.fmToneValue;
        if (d.repeaterOffsetDir) lastSliceState.repeaterOffsetDir = d.repeaterOffsetDir;
        if (d.fmRepeaterOffsetFreq) {
            lastSliceState.fmRepeaterOffsetFreq = d.fmRepeaterOffsetFreq;
        }
        if (d.txOffsetFreq) lastSliceState.txOffsetFreq = d.txOffsetFreq;
    };
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                     [&](int, const SliceDelta& d) {
                         mergeSlice(d);
                         if (d.mode)
                             publishedModes.push_back(*d.mode);
                         if (d.modeList)
                             publishedModeLists.push_back(*d.modeList);
                     });
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &app,
                     [&](const TransmitDelta& d) {
        if (d.speechProcEnable) lastTransmitState.speechProcEnable = d.speechProcEnable;
        if (d.speechProcLevel) lastTransmitState.speechProcLevel = d.speechProcLevel;
        if (d.rfPower) lastTransmitState.rfPower = d.rfPower;
        if (d.micLevel) lastTransmitState.micLevel = d.micLevel;
        if (d.sbMonitor) lastTransmitState.sbMonitor = d.sbMonitor;
        if (d.voxEnable) lastTransmitState.voxEnable = d.voxEnable;
        if (d.voxLevel) lastTransmitState.voxLevel = d.voxLevel;
        if (d.cwPitch) lastTransmitState.cwPitch = d.cwPitch;
        if (d.cwSpeed) lastTransmitState.cwSpeed = d.cwSpeed;
        if (d.cwBreakIn) lastTransmitState.cwBreakIn = d.cwBreakIn;
        if (d.transmitFreq) lastTransmitState.transmitFreq = d.transmitFreq;
        if (d.txFilterLow) lastTransmitState.txFilterLow = d.txFilterLow;
        if (d.txFilterHigh) lastTransmitState.txFilterHigh = d.txFilterHigh;
        if (d.atuEnabled) lastTransmitState.atuEnabled = d.atuEnabled;
        if (d.atuStatusRaw) lastTransmitState.atuStatusRaw = d.atuStatusRaw;
        if (d.mox) { lastTransmitState.mox = d.mox; moxPublications.push_back(*d.mox); }
    });

    // WHAT THE RADIO SAYS IT IS, and which bands follow from that. Both ride
    // the same RadioDelta, and the band half is what puts a 2 m / 70 cm button
    // in front of the operator (#5041) — so capture it off the seam rather than
    // trusting the table it was read from.
    QString publishedModel;
    QString publishedBandsRaw;
    // Counted, not just captured. The identity is published TWICE per connect -
    // once from the handshake name and again when the directed 0x19 0x00 answers
    // - and the whole point of the first one is that it beats the second. A test
    // that only looks at the final value cannot tell the two apart.
    int identityPublications = 0;
    QObject::connect(&backend, &IRadioBackend::radioChanged, &app,
                     [&](const RadioDelta& d) {
                         if (d.model) publishedModel = *d.model;
                         if (d.bandsRaw) publishedBandsRaw = *d.bandsRaw;
                         if (d.model || d.bandsRaw) ++identityPublications;
                     });
    QObject::connect(&backend, &IRadioBackend::transmitFrequencyCheckChanged,
                     &app, [&xfcPublications](bool on) {
        xfcPublications.push_back(on);
    });

    // THE SNAPSHOT THAT MAKES THE ORDERING ASSERTABLE.
    //
    // Taken synchronously inside `emit connected()` - same thread, direct
    // connection - so it freezes what the seam had been told at the instant the
    // connect edge fired, before any waitFor() spins an event loop and lets the
    // 0x19 reply land. Without this the assertions below pass whether the
    // declaration arrived at the handshake or half a second later off the wire,
    // which is precisely the distinction the band menu depends on.
    int identityPublicationsAtConnect = -1;
    QString modelAtConnect;
    QString bandsAtConnect;
    QObject::connect(&backend, &IRadioBackend::connected, &app, [&] {
        identityPublicationsAtConnect = identityPublications;
        modelAtConnect = publishedModel;
        bandsAtConnect = publishedBandsRaw;
    });

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    QSignalSpy meterDefSpy(&backend, &IRadioBackend::meterDefined);
    QSignalSpy rfGainInfoSpy(&backend, &IRadioBackend::panRfGainInfoChanged);
    QSignalSpy spectrumSpy(&backend, &IRadioBackend::spectrumFrameReady);

    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radio.controlPort();
    req.params.insert(QStringLiteral("icom.serialPort"), radio.serialPort());
    req.params.insert(QStringLiteral("icom.audioPort"), radio.audioPort());
    req.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
    req.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
    req.params.insert(QStringLiteral("icom.civAddress"), 0xA4);

    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }), "the backend connects");
    check(connectedSpy.count() == 1, "and emits connected() exactly once");

    // The identity, published ON THE CONNECT EDGE — not later, when the
    // 0x19 0x00 address query answers. The band menu is built there, so a
    // declaration that arrived after it would leave the operator looking at a
    // band panel with no 2 m or 70 cm button until something else forced a
    // rebuild. The name is the only identity that exists this early, and it is
    // enough (#5041).
    //
    // ASSERTED OFF THE SNAPSHOT, not off the live variables. `connected()` is
    // emitted after publishIdentity() in onSessionConnected() and before any
    // event loop runs again, while the directed 0x19 0x00 reply cannot arrive
    // until a socket read is serviced — so anything the snapshot holds was
    // published from the handshake name, and anything it does not hold was not.
    // Remove the publishIdentity() call from onSessionConnected() and the five
    // checks below fail (measured, along with the agreement check further down),
    // which is the gate the timing claim needs and did not have: the live
    // variables are re-filled identically by the 0x19 republish a moment later,
    // so every one of these passed against code that published only late.
    check(identityPublicationsAtConnect == 1,
          "the identity is published exactly once BEFORE connected() is emitted "
          "- from the handshake name, not from the 0x19 0x00 reply that cannot "
          "have been serviced yet");
    check(modelAtConnect == QStringLiteral("IC-705"),
          "and publishes the canonical model name at connect");
    check(bandsAtConnect.contains(QStringLiteral("2m")),
          "declaring 2m, which no FlexLib model table would have given it");
    check(bandsAtConnect.contains(QStringLiteral("440")),
          "and 440 - the band the reported bug refused to tune at all");
    check(bandsAtConnect.contains(QStringLiteral("20m")),
          "and HF, because the declaration REPLACES the built-in band grid "
          "rather than adding a VHF row to it");

    // THE SECOND PUBLICATION, which is what makes the first one an ordering
    // claim rather than a tautology. The directed 0x19 0x00 in the connect burst
    // re-publishes the identity from the authoritative address, so the count
    // must GROW past the snapshot — if it never did, "published at the connect
    // edge" would be true only because nothing else ever published at all.
    check(waitFor([&] { return identityPublications > identityPublicationsAtConnect; }),
          "and the 0x19 0x00 reply re-publishes it afterwards - two distinct "
          "publications, the early one first");
    check(publishedModel == QStringLiteral("IC-705"),
          "with the address-resolved identity agreeing with the name-resolved one");
    check(publishedBandsRaw == bandsAtConnect,
          "and the same declaration, so the band menu is not rebuilt from a "
          "different answer a moment later");

    quint64 schedulerRequestId = 9000;
    const auto waitSchedulerIdle = [&](int timeoutMs = 5000) {
        QSignalSpy replySpy(&backend, &IRadioBackend::extensionResult);
        const quint64 requestId = ++schedulerRequestId;
        QVariantMap arg;
        arg.insert(QStringLiteral("timeoutMs"), timeoutMs);
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("civ.scheduler.wait-idle"),
                                requestId, arg);
        QVariantMap result;
        const bool replied = waitFor([&] {
            for (const QList<QVariant>& reply : replySpy) {
                if (reply.at(0).toULongLong() == requestId) {
                    result = reply.at(1).toMap();
                    return true;
                }
            }
            return false;
        }, timeoutMs + 500);
        return replied && result.value(QStringLiteral("idle")).toBool()
            && !result.value(QStringLiteral("timedOut")).toBool();
    };

    // ---- capability -------------------------------------------------------
    const RadioCapabilities caps = backend.capabilities();
    check(caps.hasTransmitFrequencyCheck,
          "IC-705 advertises the verified momentary XFC capability");
    check(caps.family == QStringLiteral("icom"), "family is icom");
    check(caps.maxSlices == 1, "one slice");
    // The three that are easy to get backwards, each with a real consequence.
    check(!caps.hostModulates,
          "the RADIO modulates — true here would open the host mic on a radio that never uses it");
    check(!caps.hasDaxStreams, "no IQ on any networked Icom — absent, not deferred");
    check(caps.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "the radio remembers its own state, so the client restores NOTHING");
    check(caps.hasRadioSideDsp, "NR/NB/notch run in the radio's firmware");
    check(caps.hasRadioSideCwKeyer && caps.cwTextKeyerName == QLatin1String("CWK"),
          "CWK capability follows the resolved CI-V model, not its display string");

    // RFC #4984's profile diagnostic is the hand-off contract for subsequent
    // model bring-ups. Pin the serialized form here, not only the C++ table, so
    // PRs #5140/#5149 can consume it without reintroducing address branches.
    {
        QSignalSpy profileSpy(&backend, &IRadioBackend::extensionResult);
        backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("profile.show"),
                                9001, {});
        check(profileSpy.count() == 1, "profile.show answers synchronously");
        if (profileSpy.count() == 1) {
            const QVariantMap profile = profileSpy.at(0).at(1).toMap();
            check(profile.value(QStringLiteral("model")).toString()
                      == QLatin1String("IC-705"),
                  "profile.show names the active model");
            check(profile.value(QStringLiteral("supportedBringup")).toBool(),
                  "the active IC-705 profile is an intentional bring-up target");
            const QVariantMap fm = profile.value(QStringLiteral("fmRepeater")).toMap();
            check(fm.value(QStringLiteral("dialect")).toString()
                      == QLatin1String("extended")
                      && fm.value(QStringLiteral("dtcs")).toBool()
                      && fm.value(QStringLiteral("xfc")).toBool(),
                  "profile.show carries the IC-705 repeater dialect, DTCS and XFC");
            const QVariantMap gps = profile.value(QStringLiteral("gps")).toMap();
            check(gps.value(QStringLiteral("ntpEnabledSetItem")).toInt() == 167
                      && gps.value(QStringLiteral("ntpServerSetItem")).toInt() == 168
                      && gps.value(QStringLiteral("timeCorrectSetItem")).toInt() == 169
                      && gps.value(QStringLiteral("ntpAccess")).toBool(),
                  "profile.show carries the IC-705 GPS/NTP command shape");
        }
    }
    check(caps.cwTextMinWpm == 6 && caps.cwTextMaxWpm == 48
              && caps.cwTextMaxMessageChars == 30
              && !caps.cwTextHasProgress && !caps.cwTextHasStoredMacros,
          "CWK publishes its honest range, message, progress and macro limits");
    check(!caps.canReboot, "power-off over WiFi is a one-way trip, so no reboot is offered");

    // ---- a slice exists, which TCI routing depends on ---------------------
    check(sliceSpy.count() >= 1, "a slice is published at connect");
    check(!meterDefSpy.isEmpty(), "meter definitions are published");
    check(rfGainInfoSpy.count() == 1, "the RF-gain range is advertised");
    if (rfGainInfoSpy.count() == 1) {
        const auto args = rfGainInfoSpy.first();
        // THE REAL RF-GAIN REGISTER (14 02), 0000..0255, published as a
        // percentage. This slider used to drive the three-position PREAMP
        // (16 02) and label its positions "0 dB / 1 dB / 2 dB" — the radio
        // calls them OFF, P.AMP1 and P.AMP2 and publishes no gain figures for
        // them, so none of those numbers was a decibel of anything.
        check(args.at(1).toInt() == 0 && args.at(2).toInt() == 100 && args.at(3).toInt() == 1,
              "as the real 0..100 continuous gain");
        // 14 02 has no published dB mapping, so a dB label would be the same
        // invention in a new place.
        check(args.at(4).toString() == QStringLiteral("%"),
              "in percent, because the register has no published dB scale");
    }

    // ---- model discovery (Phase 5) ----------------------------------------
    check(waitFor([&] { return backend.model().civAddress == 0xA4; }),
          "the backend ASKS the radio what it is (0x19 0x00) rather than assuming");
    check(backend.model().name == "IC-705", "and resolves the IC-705");
    check(backend.model().verified, "whose capability numbers are tier-1 verified");
    check(waitSchedulerIdle(),
          "connect-time radio-authoritative state converges through the scheduler");
    const RadioCapabilities connectedCaps = backend.capabilities();
    check(connectedCaps.hasGpsLocation && connectedCaps.hasGpsTimeConfiguration,
          "the resolved IC-705 advertises GPS position and clock configuration");
    check(!connectedCaps.hasGpsSatelliteTelemetry
              && !connectedCaps.hasGpsFrequencyReference,
          "but does not overclaim satellite counts, lock, or a GPS frequency reference");
    check(gpsState.positionValid.value_or(false)
              && gpsState.status.value_or(QString{}) == QStringLiteral("Position reported"),
          "the startup read publishes a usable position without inventing a lock");
    check(!gpsState.grid.value_or(QString{}).isEmpty()
              && gpsState.date.value_or(QString{}) == QStringLiteral("2026-08-21")
              && gpsState.time.value_or(QString{}) == QStringLiteral("12:34:56Z"),
          "coordinates are derived to grid square and the full radio UTC is preserved");
    check(gpsState.ntpEnabled.value_or(false)
              && gpsState.ntpServer.value_or(QString{}) == QStringLiteral("time.nist.gov")
              && gpsState.gpsTimeCorrectionEnabled.value_or(false),
          "the dashboard state is adopted from the radio's NTP/GPS settings");

    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("gps.ntp.enabled"),
                            0, false);
    check(waitSchedulerIdle() && radio.setting(setting::kNtpEnabled) == 0x00
              && gpsState.ntpEnabled.has_value() && !*gpsState.ntpEnabled,
          "an explicit NTP toggle is written, read back, and only then published");
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("gps.ntp.server"),
                            0, QStringLiteral("pool.ntp.org"));
    check(waitSchedulerIdle()
              && radio.settingText(setting::kNtpServer) == "pool.ntp.org"
              && gpsState.ntpServer.value_or(QString{}) == QStringLiteral("pool.ntp.org"),
          "an explicit NTP hostname write preserves all bytes and publishes read-back");
    backend.invokeExtension(QStringLiteral("icom"),
                            QStringLiteral("gps.time-correction"), 0, false);
    check(waitSchedulerIdle() && radio.setting(setting::kGpsTimeCorrect) == 0x00
              && gpsState.gpsTimeCorrectionEnabled.has_value()
              && !*gpsState.gpsTimeCorrectionEnabled,
          "GPS Time Correct also follows explicit-write then read-back authority");
    backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("gps.ntp.sync"),
                            0, {});
    check(waitSchedulerIdle()
              && gpsState.ntpSyncStatus.value_or(QString{}) == QStringLiteral("Succeeded"),
          "Sync now follows 1A 07 with the radio's 1A 08 access result");
    check(lastTransmitState.cwSpeed.value_or(-1) == 28,
          "connect reads and adopts the radio's 28 WPM key speed");
    check(lastTransmitState.cwPitch.value_or(-1) == 601,
          "connect reads and adopts the radio's CW pitch");
    check(lastTransmitState.cwBreakIn.value_or(false),
          "connect reads and adopts the radio's full break-in as active");
    const auto sawConnectRead = [&](std::uint8_t command, std::uint8_t sub) {
        return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                           [=](const CivFrame& frame) {
            return frame.cmd == command && frame.hasSub && frame.sub == sub
                && frame.data.empty();
        });
    };
    check(sawConnectRead(cmd::kLevel, level::kCwPitch)
              && sawConnectRead(cmd::kLevel, level::kKeySpeed)
              && sawConnectRead(cmd::kFunction, func::kBreakIn),
          "the connect burst explicitly requests pitch, speed and break-in");
    const auto sawBareConnectRead = [&](std::uint8_t command) {
        return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                           [=](const CivFrame& frame) {
            return frame.cmd == command && !frame.hasSub && frame.data.empty();
        });
    };
    check(lastSliceState.fmToneMode == QStringLiteral("ctcss_tx")
              && std::abs(lastSliceState.fmToneValue.value_or(0.0) - 88.5) < 0.001,
          "connect adopts the IC-705 repeater-tone enable and frequency");
    check(lastSliceState.repeaterOffsetDir == QStringLiteral("down")
              && std::abs(lastSliceState.fmRepeaterOffsetFreq.value_or(0.0) - 0.6) < 0.000001
              && std::abs(lastSliceState.txOffsetFreq.value_or(0.0) + 0.6) < 0.000001,
          "connect adopts DUP- and the 600 kHz repeater offset into the slice UX state");
    check(sawConnectRead(cmd::kFunction, func::kRepeaterTone)
              && sawConnectRead(cmd::kTone, 0x00)
              && sawBareConnectRead(cmd::kDuplex)
              && sawBareConnectRead(cmd::kReadRepeaterOffset),
          "the connect burst explicitly requests every FM repeater field");
    check(sawConnectRead(cmd::kControl, control::kXfc),
          "the connect burst explicitly adopts the radio's XFC state");

    // XFC is momentary, radio-authoritative selected-VFO state. Both write
    // edges must land, and a silent front-panel hold must still converge via
    // the fast poll used when CI-V Transceive is quiet.
    radio.clearCivLog();
    backend.setTransmitFrequencyCheck(true);
    check(waitSchedulerIdle() && radio.m_transmitFrequencyCheck,
          "IC-705 XFC press reaches the radio");
    check(!xfcPublications.empty() && xfcPublications.back(),
          "IC-705 XFC press readback updates the UX state");
    backend.setTransmitFrequencyCheck(false);
    check(waitSchedulerIdle() && !radio.m_transmitFrequencyCheck,
          "IC-705 XFC release always reaches the radio");
    radio.frontPanelTransmitFrequencyCheck(true);
    check(waitFor([&] {
              return !xfcPublications.empty() && xfcPublications.back();
          }, 1500),
          "silent front-panel IC-705 XFC press converges into the UX");
    radio.frontPanelTransmitFrequencyCheck(false);
    check(waitFor([&] {
              return !xfcPublications.empty() && !xfcPublications.back();
          }, 1500),
          "silent front-panel IC-705 XFC release converges into the UX");

    // ---- FM repeater: two-way mapping and memory-safe ordering ------------
    // The fake reproduces the IC-705 quirk where a frequency change clears the
    // repeater-tone enable. A complete memory application therefore has to tune
    // first and enable tone last; inspecting only final model state would let a
    // wrong ordering pass while the radio itself was left with tone off.
    radio.setClearRepeaterToneOnFrequencyWrite(true);
    radio.clearCivLog();
    backend.setSliceFrequency(0, 145'250'000.0);
    backend.setSliceFmRepeater(0, QStringLiteral("up"), 600'000.0,
                               QStringLiteral("ctcss_tx"), 100.0);
    check(waitSchedulerIdle(), "FM repeater memory application drains through the scheduler");
    check(radio.m_repeaterOffsetDirection == RepeaterOffsetDirection::Up
              && radio.m_repeaterOffsetHz == 600'000
              && std::abs(radio.m_repeaterToneHz - 100.0) < 0.001
              && radio.m_functions[func::kRepeaterTone] == 1,
          "frequency, DUP+, offset, CTCSS value and enable all land on the radio");
    const auto writeIndex = [&](const std::function<bool(const CivFrame&)>& match) {
        const auto it = std::find_if(radio.civCommands().begin(), radio.civCommands().end(), match);
        return it == radio.civCommands().end()
            ? static_cast<std::ptrdiff_t>(-1)
            : std::distance(radio.civCommands().begin(), it);
    };
    const std::ptrdiff_t frequencyWrite = writeIndex([](const CivFrame& frame) {
        return frame.cmd == cmd::kSetFreq && !frame.data.empty();
    });
    const std::ptrdiff_t toneEnableWrite = writeIndex([](const CivFrame& frame) {
        return frame.cmd == cmd::kFunction && frame.hasSub
            && frame.sub == func::kRepeaterTone && !frame.data.empty();
    });
    const std::ptrdiff_t toneValueWrite = writeIndex([](const CivFrame& frame) {
        return frame.cmd == cmd::kTone && frame.hasSub && frame.sub == 0x00
            && !frame.data.empty();
    });
    check(frequencyWrite >= 0 && toneValueWrite > frequencyWrite
              && toneEnableWrite > toneValueWrite,
          "memory application tunes first, writes the tone parameter, and enables CTCSS last");

    // Change all four values at the fake radio's front panel WITHOUT an
    // unsolicited frame. Two bounded link-tick groups must reconcile them,
    // which is the fallback that keeps the UX honest when Transceive is quiet.
    radio.frontPanelRepeater(RepeaterOffsetDirection::Down, 700'000, false, 123.0);
    QMetaObject::invokeMethod(&backend, "onLinkTick", Qt::DirectConnection);
    QMetaObject::invokeMethod(&backend, "onLinkTick", Qt::DirectConnection);
    check(waitSchedulerIdle(), "front-panel FM repeater reconciliation drains");
    check(lastSliceState.fmToneMode == QStringLiteral("off")
              && std::abs(lastSliceState.fmToneValue.value_or(0.0) - 123.0) < 0.001
              && lastSliceState.repeaterOffsetDir == QStringLiteral("down")
              && std::abs(lastSliceState.fmRepeaterOffsetFreq.value_or(0.0) - 0.7) < 0.000001
              && std::abs(lastSliceState.txOffsetFreq.value_or(0.0) + 0.7) < 0.000001,
          "silent front-panel tone, tone value, DUP- and offset changes converge into the UX");
    radio.setClearRepeaterToneOnFrequencyWrite(false);
    backend.setSliceFrequency(0, static_cast<double>(kRadioFrequencyHz));
    check(waitSchedulerIdle(), "the integration fixture returns to its HF test frequency");

    // One CI-V command 17 message is one bounded, exact transaction. Text the
    // radio cannot preserve is rejected before a frame enters the scheduler.
    radio.clearCivLog();
    check(!backend.sendCwText(QStringLiteral("CQ \u2665 TEST")).isEmpty(),
          "unsupported CW text is rejected instead of rewritten");
    check(!backend.sendCwText(QString(31, QLatin1Char('A'))).isEmpty(),
          "messages beyond the documented 30-character frame are rejected");
    check(radio.civCommands().empty(),
          "a rejected CW message emits no CI-V frame");
    const QString exactLimit(30, QLatin1Char('A'));
    check(backend.sendCwText(exactLimit).isEmpty(),
          "a valid 30-character CW message is accepted");
    check(waitSchedulerIdle(), "the accepted CW message drains through the scheduler");
    check(std::count_if(radio.civCommands().begin(), radio.civCommands().end(),
                        [](const CivFrame& frame) {
              return frame.cmd == cmd::kCwMessage && frame.data.size() == 30;
          }) == 1,
          "the accepted message is exactly one bounded command 17 frame");

    // The UI is boolean but the register is Off/Semi/Full. After adopting
    // Full, toggling off and back on must restore 02 rather than demote to 01.
    radio.clearCivLog();
    backend.setCwBreakIn(false);
    check(waitSchedulerIdle(), "full break-in can be turned off");
    backend.setCwBreakIn(true);
    check(waitSchedulerIdle(), "break-in can be restored");
    std::vector<int> breakInWrites;
    for (const CivFrame& frame : radio.civCommands()) {
        if (frame.cmd == cmd::kFunction && frame.hasSub
            && frame.sub == func::kBreakIn && !frame.data.empty()) {
            breakInWrites.push_back(frame.data.front());
        }
    }
    check(breakInWrites == std::vector<int>({0, 2}),
          "Off -> On restores the radio's full-break-in mode without loss");
    const SliceDelta connectedSliceState = lastSliceState;
    const TransmitDelta connectedTransmitState = lastTransmitState;

    // ---- THE AUDIO CONTRACT (TCI / WSJT-X) --------------------------------
    //
    // Feed exactly 4800 mono samples at 48 kHz — 100 ms. The contract says the
    // seam must see 100 ms of INTERLEAVED STEREO at 24 kHz, which is
    // 2400 frames x 2 channels x 4 bytes = 19200 bytes.
    constexpr int kMonoSamplesIn = 4800;
    constexpr qint64 kExpectedBytes = 2400 * 2 * static_cast<qint64>(sizeof(float));
    {
        std::vector<float> mono(kMonoSamplesIn);
        for (int i = 0; i < kMonoSamplesIn; ++i)
            mono[static_cast<std::size_t>(i)] =
                0.25f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        // Push it the way the radio does: in the protocol's own unequal pair,
        // 682 + 278 samples per 20 ms frame, rather than as one big block.
        std::size_t at = 0;
        while (at < mono.size()) {
            const std::size_t n = std::min<std::size_t>(682, mono.size() - at);
            radio.pushAudio(encodeAudio(AudioCodec::Lpcm1ch16,
                                        std::span<const float>(mono.data() + at, n)));
            at += n;
        }
    }

    check(waitFor([&] { return sliceAudioBytes >= kExpectedBytes * 8 / 10; }),
          "per-slice audio reaches the seam — this IS the TCI receiver channel");

    // r8brain has a startup latency, so the exact byte count lags by a small
    // amount on the first block. What must hold is the RATIO: 4800 mono
    // samples in at 48 kHz must produce ~2400 stereo FRAMES out at 24 kHz.
    const qint64 framesOut = sliceAudioBytes / (2 * static_cast<qint64>(sizeof(float)));
    check(framesOut > 2000 && framesOut < 2600,
          "4800 mono samples at 48 kHz become ~2400 stereo frames at 24 kHz");
    check(framesOut < kMonoSamplesIn * 3 / 4,
          "NOT a passthrough — a backend that skipped the rate conversion would emit ~4800");

    // Stereo, not mono. TciServer divides by 2*sizeof(float); a mono buffer
    // makes it see half the frames it thinks it has.
    check(sliceAudioBytes % (2 * static_cast<qint64>(sizeof(float))) == 0,
          "the buffer is a whole number of INTERLEAVED STEREO frames");

    // Both feeds, and they are different consumers: the speaker gets the mix,
    // the per-slice feed gets one slice for the decoders.
    check(speakerBuffers > 0, "the speaker feed is emitted too");
    check(sliceAudioBuffers == speakerBuffers,
          "one per-slice buffer for every speaker buffer — neither path is starved");

    // ---- spectrum (Phase 2 through the seam) ------------------------------
    {
        std::vector<std::uint8_t> body;
        body.push_back(0x00);
        body.push_back(encodeBcdByte(1));
        body.push_back(encodeBcdByte(1));
        body.push_back(0x00);                       // centre mode
        const auto centre = encodeFreq(14'100'000);
        const auto span   = encodeFreq(100'000);
        body.insert(body.end(), centre.begin(), centre.end());
        body.insert(body.end(), span.begin(), span.end());
        body.push_back(0x00);
        for (int i = 0; i < kScopePointsIc705; ++i)
            body.push_back(static_cast<std::uint8_t>(i % (kScopeMaxAmplitude + 1)));
        std::vector<std::uint8_t> civ{0xFE, 0xFE, kControllerAddress, kIc705Addr,
                                      cmd::kScope, scope::kWaveData};
        civ.insert(civ.end(), body.begin(), body.end());
        civ.push_back(kCivEom);
        radio.pushCiv(civ);
    }
    check(waitFor([&] { return spectrumSpy.count() > 0; }),
          "a scope sweep reaches the seam as a spectrum frame");
    if (spectrumSpy.count() > 0) {
        const QByteArray frame = spectrumSpy.first().at(1).toByteArray();
        check(frame.size() == kScopePointsIc705 * static_cast<int>(sizeof(float)),
              "475 float32 bins");
    }

    // ---- metering (Phase 4) through the seam ------------------------------
    {
        QSignalSpy meterSpy(&backend, &IRadioBackend::meterUpdate);
        // The radio answers the S-meter poll the scheduler is already issuing.
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kMeter,
                       meter::kSMeter, 0x01, 0x20, kCivEom});   // BCD 0120 == S9
        check(waitFor([&] { return meterSpy.count() > 0; }), "a meter reading reaches the seam");
        if (meterSpy.count() > 0) {
            const auto args = meterSpy.first();
            // SOURCE:NAME, not a bare name. This assertion was left behind when
            // the meters were re-homed onto the convention MeterModel actually
            // looks up; a bare "LEVEL" is published where nothing reads it.
            check(args.at(0).toString() == QStringLiteral("SLC:LEVEL"),
                  "as the SLC:LEVEL meter");
            // Raw 120 is S9. On 20 m that is -73 dBm; the value must be
            // calibrated, not the raw byte.
            check(std::fabs(args.at(1).toDouble() - -73.0) < 1.0,
                  "calibrated to -73 dBm (S9 on HF), not passed through raw");
        }
    }

    // ---- transmit audio is gated on keying --------------------------------
    //
    // A SAFETY GATE, not an optimisation. AudioEngine does not PTT-gate the tap
    // that feeds submitTxAudio, on purpose and by documented contract, so if
    // the backend does not gate then nothing does and the operator's live
    // microphone streams to the radio's modulation input for the whole session
    // — which a radio with VOX enabled keys on (Principle VI).
    {
        const int before = radio.audioPacketsFromClient();
        QByteArray pcm(1024 * 2 * static_cast<int>(sizeof(qint16)), 0);
        auto* w = reinterpret_cast<qint16*>(pcm.data());
        for (int i = 0; i < 1024; ++i) {
            const auto v = static_cast<qint16>(8000 * std::sin(2.0 * M_PI * 1000.0 * i / 24000.0));
            w[i * 2] = v;
            w[i * 2 + 1] = v;
        }

        // UNKEYED: nothing may reach the radio.
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == before,
              "transmit audio is DROPPED while unkeyed");

        // KEYED: the same buffers must arrive.
        radio.clearCivLog();
        backend.setKeying(true);
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     movesPttOrTuner);
              }, 1000),
              "the keyed intent reaches the radio through the scheduler");
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(200);
        const int keyed = radio.audioPacketsFromClient();
        check(keyed > before, "and flows once keyed");

        // THE TX-SAFETY PREDICATE HAS TEETH. The scrub check further down is a
        // negative — "no frame like this was sent" — and a negative passes for
        // free the moment it stops recognising the thing it forbids. Here a
        // transmitter really was keyed over the same wire and through the same
        // capture, so the predicate is proven to catch it before it is trusted
        // to say the scrub never did.
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          movesPttOrTuner),
              "and a real key IS caught by the TX-safety predicate, so the "
              "scrub's 'never keys' check is not vacuous");

        backend.setKeying(false);
        check(waitSchedulerIdle(), "unkey and its radio confirmation complete");

        // ...and stops again on unkey, rather than draining a backlog into the
        // next transmission.
        QTest::qWait(120);
        const int afterUnkey = radio.audioPacketsFromClient();
        for (int i = 0; i < 20; ++i)
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        QTest::qWait(120);
        check(radio.audioPacketsFromClient() == afterUnkey,
              "and stops again on unkey");
    }

    // ---- THE PTT CONFIRMATION WINDOW, IN BOTH DIRECTIONS ------------------
    //
    // RFC #4983's captured FT8 failure and its explicit counter-rule live here.
    // The window is what lets a newer key-on intent outlive an older poll's OFF
    // answer; it must NOT also let a client's unkey request outlive the radio
    // saying it is still transmitting.
    {
        // (a) THE CAPTURED FAILURE. Key on, then have the radio insist it is
        //     still RX — exactly the pre-key poll answer arriving late. The
        //     model must not follow it back to RX inside the window.
        backend.setKeying(false);
        check(waitSchedulerIdle(), "PTT fixture starts unkeyed");
        moxPublications.clear();

        radio.m_pttOverride = false;         // radio keeps answering "RX"
        backend.setKeying(true);
        QTest::qWait(600);                   // several 250 ms fallback polls
        check(std::find(moxPublications.begin(), moxPublications.end(), false)
                  == moxPublications.end(),
              "a contradictory PTT OFF is suppressed while a key-on intent is "
              "pending — the captured FT8 transmit-audio teardown");
        check(lastTransmitState.mox.value_or(false),
              "and the model stays keyed for the operator who asked to transmit");

        // The window is BOUNDED. Past it the radio wins again (Constitution II),
        // otherwise a client belief outlives the hardware indefinitely.
        QTest::qWait(700);
        check(!lastTransmitState.mox.value_or(true),
              "once the 1 s window expires the radio's own report wins again");

        // (b) THE DIRECTION THAT MUST NEVER BE SUPPRESSED. Ask to unkey while
        //     the radio insists it is transmitting — a lost or refused unkey.
        //     Swallowing this is the one failure that leaves an operator on the
        //     air with a UI that says otherwise (Constitution VI fails closed).
        radio.m_pttOverride = true;
        moxPublications.clear();
        backend.setKeying(false);
        check(waitFor([&] {
                  return std::find(moxPublications.begin(), moxPublications.end(),
                                   true) != moxPublications.end();
              }, 1000),
              "a radio reporting KEYED after an unkey request is published "
              "immediately, NOT suppressed by the confirmation window");
        check(lastTransmitState.mox.value_or(false),
              "and the model shows the transmitter that is actually on the air");

        radio.m_pttOverride.reset();
        backend.setKeying(false);
        check(waitFor([&] { return !lastTransmitState.mox.value_or(true); }, 2000),
              "an obedient radio then unkeys normally");
        check(waitSchedulerIdle(), "PTT fixture drains");
    }

    // ── PC AUDIO OWNS DATA OFF MOD ONLY WHEN THE OPERATOR CLICKS ─────────
    //
    // DATA OFF MOD (1A 05 item 0118 on this radio) is a SET-menu register the
    // RADIO persists. Constitution III therefore forbids the client replaying
    // its own remembered PC Audio flag onto it at connect — and the register is
    // four-valued while the button is two-valued, so "off" has to put back what
    // the operator had rather than assuming MIC. Both were live defects; both
    // are pinned here.
    //
    // The fake starts at USB (0x01), the ordinary setting for an operator with
    // a rig interface on the USB port. A fake already sitting on WLAN could not
    // tell "PC Audio put it back" from "PC Audio never touched it".
    {
        const auto writesTo118 = [](const std::vector<CivFrame>& log) {
            std::vector<int> values;
            for (const CivFrame& f : log) {
                // A WRITE is the three-byte form. The two-byte form is a read,
                // and counting those would make this assertion vacuous.
                if (f.cmd == cmd::kSetting && f.hasSub && f.sub == 0x05
                    && f.data.size() == 3
                    && decodeBcdByte(f.data[0]) * 100 + decodeBcdByte(f.data[1]) == 118) {
                    values.push_back(f.data[2]);
                }
            }
            return values;
        };

        check(waitSchedulerIdle(), "the connect burst drains before the check");
        check(writesTo118(radio.civCommands()).empty(),
              "CONNECT WRITES NOTHING to DATA OFF MOD — the client publishes its "
              "PC Audio state, it does not replay it onto radio-owned config "
              "(Constitution III)");
        check(radio.setting(118) == 0x01,
              "so the operator's own USB selection survives the connect");
        check(waitFor([&] {
                  return backend.healthSnapshot().values.contains(
                      QStringLiteral("dataoffmod"));
              }, 3000),
              "and the client ADOPTED it — Radio Health reports DATA OFF MOD");

        // The observation verb is not a back door to the write.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc.state"), 0, true);
        check(waitSchedulerIdle(), "the state publication settles");
        check(writesTo118(radio.civCommands()).empty(),
              "publishing PC Audio state writes nothing either");

        // An operator CLICK is a request, and Principle II allows exactly that.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc"), 0, true);
        check(waitSchedulerIdle(), "the PC Audio ON request converges");
        check(writesTo118(radio.civCommands()) == std::vector<int>{0x03},
              "a click selects WLAN (0x03) on an IC-705");
        check(radio.setting(118) == 0x03, "and the radio holds it");

        // ...and OFF restores what was captured, NOT a hardcoded MIC. This is
        // the assertion that fails if the restore is dropped: MIC is 0x00 and
        // the operator's USB is 0x01, so the two cannot be confused.
        radio.clearCivLog();
        backend.invokeExtension(QStringLiteral("icom"),
                                QStringLiteral("audio.pc"), 0, false);
        check(waitSchedulerIdle(), "the PC Audio OFF request converges");
        check(writesTo118(radio.civCommands()) == std::vector<int>{0x01},
              "turning PC Audio off puts back the operator's USB (0x01) — NOT a "
              "hardcoded MIC, which would destroy their rig-interface routing "
              "with no undo");
        check(radio.setting(118) == 0x01, "and the radio ends where it started");
        check(radio.setting(119) == 0x03,
              "DATA MOD is never written by PC Audio — digital routing stays "
              "radio-authoritative");
    }

    // TUNE temporarily borrows the RF-power register. Releasing it must put
    // the operator's ordinary drive back; otherwise a low-power tune silently
    // changes the next voice/data transmission (and the UI follows the poll).
    {
        backend.setTxPower(37);
        check(waitSchedulerIdle(), "ordinary RF power converges before TUNE");
        radio.clearCivLog();
        backend.setTune(true, 10);
        check(waitSchedulerIdle(), "temporary TUNE drive and tuner state converge");
        backend.setTune(false);
        check(waitSchedulerIdle(), "TUNE release and RF-power restore converge");

        std::vector<int> powerWrites;
        for (const CivFrame& f : radio.civCommands()) {
            if (f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kRfPower) {
                if (const auto raw = decodeLevel(f.data))
                    powerWrites.push_back(*raw);
            }
        }
        check(powerWrites.size() >= 2, "TUNE writes a temporary drive and a restore");
        if (powerWrites.size() >= 2) {
            check(std::abs(powerWrites.front() - 25) <= 1,
                  "TUNE applies the requested 10% temporary drive");
            check(std::abs(powerWrites.back() - 94) <= 1,
                  "TUNE release restores the operator's prior 37% RF power");
        }
    }

    // TUNE ownership must end on EVERY unkey, not only when the TUNE toggle
    // calls setTune(false). MOX, CW PTT and the automation watchdog call
    // setKeying(false) directly, while radio protection can report PTT OFF on
    // its own. A stale tune lease leaves the timer feeding the modulation input,
    // holds RF power at the tune value and suppresses all later microphone PCM.
    {
        QByteArray pcm(1024 * 2 * static_cast<int>(sizeof(qint16)), 0);
        auto* samples = reinterpret_cast<qint16*>(pcm.data());
        for (int i = 0; i < 1024; ++i) {
            const qint16 sample = static_cast<qint16>(
                8000 * std::sin(2.0 * M_PI * 1000.0 * i / 24000.0));
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;
        }
        const auto restoredPowerWasWritten = [&radio](int minimumRaw) {
            return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                               [minimumRaw](const CivFrame& frame) {
                return frame.cmd == cmd::kLevel && frame.hasSub
                    && frame.sub == level::kRfPower
                    && decodeLevel(frame.data).value_or(-1) >= minimumRaw;
            });
        };

        backend.setTxPower(37);
        check(waitSchedulerIdle(), "direct-unkey fixture's ordinary power converges");
        radio.clearCivLog();
        const int beforeDirectTune = radio.audioPacketsFromClient();
        backend.setTune(true, 10);
        check(waitFor([&] {
                  return radio.audioPacketsFromClient() > beforeDirectTune;
              }, 1000),
              "backend-owned TUNE audio starts without a microphone callback");
        check(IcomCivBackendTestAccess::tuneTimerActive(backend),
              "the backend-owned TUNE timer is active while keyed");

        backend.setKeying(false);
        check(waitSchedulerIdle(), "a direct unkey converges during TUNE");
        check(!IcomCivBackendTestAccess::tuneActive(backend)
                  && !IcomCivBackendTestAccess::tuneTimerActive(backend),
              "a direct unkey releases TUNE ownership and stops its producer");
        check(restoredPowerWasWritten(93),
              "a direct unkey restores the operator's pre-TUNE RF power");
        QTest::qWait(80);
        const int afterDirectUnkey = radio.audioPacketsFromClient();
        QTest::qWait(100);
        check(radio.audioPacketsFromClient() == afterDirectUnkey,
              "no backend-owned TUNE packets continue after direct unkey");

        // The old m_tuning latch also blocked the ordinary PCM path forever.
        // A later explicit key must own the stream normally again.
        backend.setKeying(true);
        check(waitSchedulerIdle(), "ordinary key after direct TUNE unkey converges");
        for (int i = 0; i < 20; ++i) {
            backend.submitTxAudio(pcm, 24000, /*clientLeveled=*/false);
        }
        check(waitFor([&] {
                  return radio.audioPacketsFromClient() > afterDirectUnkey;
              }, 1000),
              "ordinary microphone PCM resumes after direct TUNE unkey");
        backend.setKeying(false);
        check(waitSchedulerIdle(), "ordinary post-TUNE unkey converges");

        backend.setTxPower(41);
        check(waitSchedulerIdle(), "radio-unkey fixture's ordinary power converges");
        radio.clearCivLog();
        const int beforeRadioTune = radio.audioPacketsFromClient();
        backend.setTune(true, 10);
        check(waitFor([&] {
                  return radio.audioPacketsFromClient() > beforeRadioTune;
              }, 1000),
              "second TUNE cycle starts its backend-owned producer");

        // Confirm the optimistic ON edge first, then simulate the radio
        // protecting itself by reporting PTT OFF independently of the client.
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kControl,
                       control::kPtt, 0x01, kCivEom});
        QTest::qWait(40);
        radio.m_pttOverride = false;
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr, cmd::kControl,
                       control::kPtt, 0x00, kCivEom});
        check(waitFor([&] {
                  return !IcomCivBackendTestAccess::tuneActive(backend);
              }, 1000),
              "a radio-reported PTT OFF releases TUNE ownership");
        check(!IcomCivBackendTestAccess::tuneTimerActive(backend),
              "a radio-reported PTT OFF stops the TUNE producer");
        check(waitSchedulerIdle(), "radio-reported TUNE unkey restores power");
        check(restoredPowerWasWritten(104),
              "radio-reported unkey restores the operator's 41% RF power");
        QTest::qWait(80);
        const int afterRadioUnkey = radio.audioPacketsFromClient();
        QTest::qWait(100);
        check(radio.audioPacketsFromClient() == afterRadioUnkey,
              "no backend-owned TUNE packets continue after radio unkey");
        radio.m_pttOverride.reset();
        backend.setKeying(false);
        check(waitSchedulerIdle(),
              "radio-unkey fixture returns the fake radio to ordinary RX state");
    }

    // ---- THE CONNECT-TIME STATE PULL --------------------------------------
    //
    // An Icom remembers its own settings across power cycles and reports them on
    // request, so AetherSDR's contract with this family is to READ that state at
    // connect and never push its own (Constitution II/III). Every value the fake
    // radio holds is deliberately NOT the client's default: a client that simply
    // kept its own numbers would look identical to one that read them, and only
    // asserting the radio's exact values can tell the two apart.
    //
    // This is the regression net for the whole class of bug the registry found —
    // a read that is issued and whose reply is dropped looks exactly like a read
    // that was never issued.
    {
        const SliceDelta sl = connectedSliceState;
        const TransmitDelta tx = connectedTransmitState;

        check(sl.nr.value_or(false), "NR ON is adopted from the radio");
        check(sl.nb.value_or(false), "NB ON is adopted");
        check(!sl.anf.value_or(true), "ANF OFF is adopted");
        check(sl.mn.value_or(false), "the manual notch's ON state is adopted");
        check(sl.agcMode.value_or(QString()) == QLatin1String("slow"),
              "AGC SLOW is adopted, not our own default");
        check(std::abs(sl.nrLevel.value_or(-1) - 30) <= 1,
              "the NR LEVEL comes back as ~30%, not the client's default");
        check(std::abs(sl.nbLevel.value_or(-1) - 70) <= 1, "and the NB level as ~70%");
        check(std::abs(sl.audioGain.value_or(-1) - 50) <= 1,
              "AF gain is read at connect — the control this branch made settable");

        check(tx.speechProcEnable.value_or(false), "the compressor's ON state is adopted");
        check(std::abs(tx.speechProcLevel.value_or(-1) - 40) <= 1,
              "and its level as ~40%");
        check(std::abs(tx.rfPower.value_or(-1) - 20) <= 1,
              "RF POWER comes from the radio — pushing our own would key at the "
              "wrong drive on the first transmission");
        check(std::abs(tx.micLevel.value_or(-1) - 60) <= 1, "mic gain as ~60%");
        check(tx.sbMonitor.value_or(false),
              "the TX monitor's ON state is adopted — its reply used to be dropped "
              "by the 0x16 switch's default");
        check(tx.voxEnable.value_or(false),
              "VOX ON is adopted — same dropped-reply bug");
        check(std::abs(tx.voxLevel.value_or(-1) - 80) <= 1, "and the VOX gain as ~80%");
        check(tx.atuEnabled.value_or(false) && tx.atuStatusRaw.has_value(),
              "the antenna tuner reports its own state, so the ATU button opens "
              "where the radio is");
        check(std::fabs(tx.transmitFreq.value_or(0.0) - 14.074) < 1e-6,
              "the Icom VFO also seeds TX frequency for frequency-aware ATU toggling");

        check(sl.ritOn.value_or(false), "RIT ON is adopted");
        check(!sl.xitOn.value_or(true), "XIT OFF is adopted");
        check(sl.ritFreq.value_or(0) == -1230,
              "and the RIT OFFSET comes back SIGNED — folding the sign byte into "
              "the magnitude tunes the wrong way");

        // DATA MODE, read from 26 and NOT inferrable from anything else.
        // The fake radio sits in USB-D; mode byte 0x01 is plain USB, so a client
        // that decodes only 04 reports USB on a radio the operator has already
        // put in a data mode. That is not cosmetic — it is the state that
        // decides whether the radio modulates from the network or the mic.
        check(sl.mode.value_or(QString()) == QLatin1String("DIGU"),
              "the radio's own DATA-ON state is adopted at connect, so USB-D "
              "reads as DIGU rather than as plain USB");
    }

    // ---- DATA MODE IS SENT, AND ADOPTED (#4984) ---------------------------
    //
    // DIGU and USB are the SAME mode byte on the wire. The only thing that
    // separates them is command 26, and until it was wired the backend inferred
    // DATA from the neutral name and told the radio nothing: selecting DIGU on
    // a radio in DATA OFF left it in plain USB, modulating from the MICROPHONE,
    // while AetherSDR's mode indicator, passband and capabilities all said
    // DIGU. Nothing errored — digital transmit looked wired and made no output.
    //
    // The three halves of that, each checked below: we SEND it, we ADOPT what
    // the radio reports, and the radio's report WINS over what we asked for.
    {
        // --- app-originated: one frame carries all three -------------------
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        QTest::qWait(150);

        const auto& sent = radio.civCommands();
        auto write26 = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kVfoMode && f.hasSub && f.sub == vfoMode::kSelected
                && f.data.size() >= 3;
        });
        check(write26 != sent.end(),
              "selecting DIGU sends 26 00 — the only command that can express "
              "the DATA flag at all");
        check(write26 != sent.end()
                  && write26->data[0] == static_cast<std::uint8_t>(CivMode::Usb)
                  && write26->data[1] == 0x01,
              "carrying mode USB *and* DATA ON — without the flag the radio "
              "stays in plain USB and transmits from the microphone");
        check(write26 != sent.end() && write26->data[2] >= 1 && write26->data[2] <= 3,
              "and the filter slot in the SAME frame, so mode, DATA and slot "
              "are applied or refused as one unit");
        // The negative half, and the one that matters: 06 is what CLEARS DATA
        // on the radio. A mode change that still sent it would undo the flag it
        // had just asked for, depending only on which frame the radio saw last.
        check(std::none_of(sent.begin(), sent.end(), [](const CivFrame& f) {
                  return f.cmd == cmd::kSetMode && !f.data.empty();
              }),
              "and NO bare 06 goes out alongside it — 06 clears DATA, so the two "
              "together would be a race with the radio's own side effects");
        check(std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
                  return f.cmd == cmd::kVfoMode && f.hasSub
                      && f.sub == vfoMode::kSelected && f.data.empty();
              }),
              "followed by a CONFIRMATION read — the write is a request, only "
              "the radio's answer is state (Constitution II)");
        check(waitFor([&] { return radio.m_dataOn; }),
              "and the radio ends up actually in DATA mode");

        // --- ...and back off again -----------------------------------------
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("USB"));
        check(waitFor([&] { return !radio.m_dataOn; }),
              "selecting plain USB clears DATA on the radio rather than leaving "
              "a data mode latched under a voice-mode indicator");

        // --- a filter change must not drop the radio out of DATA ------------
        //
        // The whole reason the three travel together. Sent as 06 this took an
        // operator running FT8 in USB-D back to plain USB — and their transmit
        // audio back to the microphone — from a button that says nothing about
        // the mode.
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(waitFor([&] { return radio.m_dataOn; }), "back into DATA for the check");
        radio.clearCivLog();
        backend.setSliceFilter(0, -1800, 0);   // 1.8 kHz — the SSB ladder's FIL3
        check(waitFor([&] { return radio.m_filter == 3; }),
              "the filter request reaches the radio and selects FIL3");
        check(waitSchedulerIdle(), "the filter write's compound readback completes");
        check(radio.m_dataOn,
              "and leaves it IN DATA — a filter button must not silently take an "
              "FT8 operator back to microphone audio");
        check(std::none_of(radio.civCommands().begin(), radio.civCommands().end(),
                           [](const CivFrame& f) {
                               return f.cmd == cmd::kSetMode && !f.data.empty();
                           }),
              "because the slot went out as 26 restating DATA, not as a bare 06 "
              "— which is the frame that clears it");
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          [](const CivFrame& f) {
                              return f.cmd == cmd::kVfoMode && f.hasSub
                                  && f.sub == vfoMode::kSelected && f.data.empty();
                          }),
              "and the filter write is confirmed by reading the compound state back");

        // --- front-panel adoption -------------------------------------------
        //
        // The radio pushes 01 <mode> <filter> when the operator turns the MODE
        // knob, and that frame CANNOT carry DATA. Following the DATA half means
        // asking, which is what the 26 read on the unsolicited form does.
        lastSliceState.mode.reset();
        publishedModes.clear();
        radio.frontPanelMode(static_cast<std::uint8_t>(CivMode::Lsb), 1, /*dataOn=*/true);
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("DIGL");
              }),
              "a front-panel LSB-D reaches the model as DIGL — the 01 push says "
              "only LSB, so this proves the DATA half was asked for and adopted");
        check(std::all_of(publishedModes.begin(), publishedModes.end(), [](const QString& m) {
                  return m == QLatin1String("DIGL");
              }),
              "without first publishing a stale voice/DATA combination");

        lastSliceState.mode.reset();
        publishedModes.clear();
        radio.frontPanelMode(static_cast<std::uint8_t>(CivMode::Lsb), 1, /*dataOn=*/false);
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("LSB");
              }),
              "and leaving DATA on the front panel comes back as plain LSB, so "
              "the flag clears as well as sets");
        check(std::all_of(publishedModes.begin(), publishedModes.end(), [](const QString& m) {
                  return m == QLatin1String("LSB");
              }),
              "without transiently republishing DIGL from the stale DATA flag");

        // --- the radio wins over our optimistic publish ----------------------
        //
        // Constitution II: setSliceMode publishes DIGU immediately because the
        // passband cannot come from anywhere else, but that is a guess. A radio
        // that refuses DATA must pull the indicator back rather than leaving it
        // claiming a mode the radio is not in.
        radio.m_refuseDataMode = true;
        lastSliceState.mode.reset();
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(waitFor([&] {
                  return lastSliceState.mode.value_or(QString()) == QLatin1String("USB");
              }),
              "when the radio REFUSES DATA, its own report corrects the "
              "optimistic DIGU back to USB instead of the indicator lying");
        radio.m_refuseDataMode = false;
    }

    // ---- the passband: slot vs width vs PBT -------------------------------
    //
    // THE THREE THINGS THIS PHASE EXISTS TO SEPARATE. Before it, the backend
    // read the SLOT and looked its width up in a table of factory defaults, so
    // an operator who had redefined a slot got a passband drawn from a number
    // nobody had ever asked the radio for.
    {
        check(waitSchedulerIdle(), "the connect burst drains before the passband checks");

        // 1. THE WIDTH COMES FROM THE RADIO. The fake holds code 0x34 — 3.0 kHz
        //    in SSB — and the client is in USB, so the drawn window must be
        //    3.0 kHz centred at 1500 Hz, i.e. 0..3000.
        check(lastSliceState.filterLow.value_or(-1) == 0
                  && lastSliceState.filterHigh.value_or(-1) == 3000,
              "the connect snapshot draws the radio's OWN 3.0 kHz width, centred");

        // 2. A DRAG IS A WIDTH CHANGE, not a slot change. 600..2600 is 2.0 kHz
        //    centred on 1600 — not one of the three published ladder widths, so
        //    it must go out as 1A 03 plus a PBT pair and must NOT send 0x26.
        radio.clearCivLog();
        backend.setSliceFilter(0, 600, 2600);
        check(waitSchedulerIdle(), "the width write and its confirmation converge");

        const auto sentFrame = [&](std::uint8_t command, std::uint8_t sub) {
            return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                               [=](const CivFrame& f) {
                return f.cmd == command && f.hasSub && f.sub == sub && !f.data.empty();
            });
        };
        check(sentFrame(cmd::kSetting, settingSub::kFilterWidth),
              "dragging an edge writes the IF width (1A 03)");
        check(sentFrame(cmd::kLevel, level::kPbtInner)
                  && sentFrame(cmd::kLevel, level::kPbtOuter),
              "and moves BOTH Twin PBTs, which is what slides without narrowing");
        check(!std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                           [](const CivFrame& f) {
                               return f.cmd == cmd::kVfoMode && !f.data.empty();
                           }),
              "a drag must NOT change the filter slot — the slots are the "
              "operator's own three presets");
        check(lastSliceState.filterHigh.value_or(0) - lastSliceState.filterLow.value_or(0)
                  == 2000,
              "and the published passband is the 2.0 kHz that was drawn");
        const QList<int> actualLabels = backend.capabilities().rxFilterWidthsHz;
        check(actualLabels == QList<int>({1800, 2000, 2400}),
              "the selected RX filter label uses its actual 1A 03 width, not 3.0k");

        // 3. A LADDER WIDTH IS A SLOT PICK. 1800 Hz is one of the three widths
        //    this backend published for SSB, so it must select FIL3 with 0x26
        //    and must not redefine the slot it is leaving.
        radio.clearCivLog();
        backend.setSliceFilter(0, 300, 2100);   // 1800 Hz — on the ladder
        check(waitSchedulerIdle(), "the slot write and its confirmation converge");
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          [](const CivFrame& f) {
                              return f.cmd == cmd::kVfoMode && f.data.size() == 3;
                          }),
              "a published ladder width selects a SLOT with 0x26");
        check(!sentFrame(cmd::kSetting, settingSub::kFilterWidth),
              "and does NOT redefine the slot's stored width");

        // 4. A MODE CHANGE RE-READS THE WIDTH — the regression this whole
        //    context-stamping design exists for, and the one that shipped past
        //    an earlier version of this test.
        //
        //    setSliceMode() advances m_mode OPTIMISTICALLY before the write
        //    goes out, so by the time the radio's 26 confirmation arrives, a
        //    "did the mode change?" test compares the new mode against itself
        //    and says no. The re-read never fired, nothing zeroed the width,
        //    and every mode inherited the one read at connect. On a real
        //    IC-7300MK2 that painted AM's 9 kHz window over every SSB filter.
        //
        //    The fake holds a DIFFERENT width per (mode, DATA, slot), which is
        //    what makes carrying one across visible here instead of plausible.
        //    Pin the slot first: step 3 above left the radio on FIL3, and the
        //    fixture defines its per-mode widths on FIL1. Selecting the SSB
        //    ladder's widest entry is a slot pick, so this moves the slot
        //    without redefining anything.
        backend.setSliceFilter(0, 300, 3300);   // 3000 Hz — ladder, so FIL1
        check(waitSchedulerIdle(), "the slot returns to FIL1");

        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("AM"));
        check(waitFor([&] {
                  return lastSliceState.filterLow.value_or(0) == -4500
                      && lastSliceState.filterHigh.value_or(0) == 4500;
              }, 4000),
              "a mode change adopts THAT mode's width (AM 9 kHz), not the one "
              "the previous mode was read at");
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     [](const CivFrame& f) {
                                         return f.cmd == cmd::kSetting && f.hasSub
                                             && f.sub == settingSub::kFilterWidth
                                             && f.data.empty();
                                     });
              }, 3000),
              "and it got there by ASKING (1A 03), not by keeping the old value");

        // 5. THE DATA FLAG IS PART OF THE CONTEXT. USB and USB-D are different
        //    filter contexts on the radio and hold different widths — proven
        //    live, where plain USB read 3.0 kHz and USB-D read 3.6 kHz.
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("USB"));
        check(waitFor([&] {
                  return lastSliceState.filterHigh.value_or(0)
                             - lastSliceState.filterLow.value_or(0) == 3000;
              }, 4000),
              "plain USB reads its own 3.0 kHz");
        radio.clearCivLog();
        backend.setSliceMode(0, QStringLiteral("DIGU"));
        check(waitFor([&] {
                  return lastSliceState.filterHigh.value_or(0)
                             - lastSliceState.filterLow.value_or(0) == 3600;
              }, 4000),
              "and USB-D reads its own 3.6 kHz — the DATA flag selects a "
              "different stored width, so it must re-read across it");

        backend.setSliceMode(0, QStringLiteral("USB"));
        check(waitSchedulerIdle(), "settle back into USB");

        // 6. THE WIDTH IS RE-READ AFTER A SLOT CHANGE, because the radio holds a
        //    different one per slot and announces none of them.
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     [](const CivFrame& f) {
                                         return f.cmd == cmd::kSetting && f.hasSub
                                             && f.sub == settingSub::kFilterWidth
                                             && f.data.empty();
                                     });
              }, 3000),
              "changing slot re-reads the new slot's actual width");
    }

    // ---- TX bandwidth: a short list, not a slider -------------------------
    {
        check(waitSchedulerIdle(), "the TX bandwidth reads settle");

        // WHICH SLOT IS LIVE decides which SET item holds the passband. The
        // fake reports MID (16 58 = 01), whose stored pair is 300..2700.
        check(lastTransmitState.txFilterLow.value_or(-1) == 300
                  && lastTransmitState.txFilterHigh.value_or(-1) == 2700,
              "the TX passband is read from the slot 16 58 actually names");

        // A REQUEST SNAPS, and what the operator sees is the read-back. 150 Hz
        // does not exist on an IC-705 (it has 100/200/300/500), and 3200 Hz is
        // past the 2900 Hz ceiling.
        radio.clearCivLog();
        backend.setTxFilter(150, 3200);
        check(waitSchedulerIdle(), "the TX bandwidth write and read-back converge");
        // 150 Hz is exactly between the IC-705's 100 and 200, and a tie takes the
        // LOWER edge — the wider passband, which is the conservative direction
        // for a transmitter. THE SAME REQUEST ON AN IC-7300MK2 WOULD GIVE 150
        // EXACTLY, because the MK2 has that edge and the IC-705 does not: the
        // per-model tables in icom_family_test are what make the two differ.
        check(lastTransmitState.txFilterLow.value_or(-1) == 100,
              "150 Hz has no IC-705 equivalent and snaps to the wider 100 Hz");
        check(lastTransmitState.txFilterHigh.value_or(-1) == 2900,
              "and 3200 Hz clamps to the 2900 Hz ceiling — the applet shows the "
              "passband the transmitter has, not the one that was asked for");

        // IT MUST RESHAPE THE LIVE SLOT AND ONLY THAT ONE. Writing WIDE while
        // the radio is running MID changes a passband nobody is transmitting
        // through and leaves the real one untouched.
        const bool wroteMidItem = std::any_of(
            radio.civCommands().begin(), radio.civCommands().end(),
            [](const CivFrame& f) {
                return f.cmd == cmd::kSetting && f.hasSub && f.sub == settingSub::kMenu
                    && f.data.size() >= 3
                    && decodeBcdByte(f.data[0]) * 100 + decodeBcdByte(f.data[1]) == 20;
            });
        check(wroteMidItem, "the write lands in SET 0020 — the MID slot, which is live");

        // The slot named by 16 58 depends on COMP state. Toggling the speech
        // processor therefore invalidates more than 16 44 itself: without this
        // second read, the next TX edge write can reshape the old slot while
        // the UI continues to display its stale pair.
        radio.clearCivLog();
        backend.setSpeechProcessor(false, 0);
        check(waitSchedulerIdle(), "the compressor toggle and dependent TX bandwidth read settle");
        check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                          [](const CivFrame& f) {
                              return f.cmd == cmd::kFunction && f.hasSub
                                  && f.sub == func::kTxBandwidth && f.data.empty();
                          }),
              "a compressor change re-reads 16 58 because it can change the active slot");
    }

    // ---- shared 0..255 percentage write buckets --------------------------
    // RF power, mic gain and monitor level were the three visible reports, but
    // they all use the same Icom register scale. Prove the real setter frames
    // select raw 26 for 10%; raw 25 is what the old floor encoder sent and the
    // radio's front panel correctly displays that as 9.
    {
        radio.clearCivLog();
        backend.setTxPower(10);
        backend.setMicGain(10);
        backend.setTxMonitor(true, 10);
        check(waitSchedulerIdle(), "percentage control writes and confirmations converge");

        const auto rawFor = [&](std::uint8_t sub) -> std::optional<int> {
            const auto& frames = radio.civCommands();
            auto it = std::find_if(frames.begin(), frames.end(), [=](const CivFrame& f) {
                return f.cmd == cmd::kLevel && f.hasSub && f.sub == sub
                    && !f.data.empty();
            });
            return it == frames.end() ? std::nullopt : decodeLevel(it->data);
        };
        check(rawFor(level::kRfPower).value_or(-1) == 26,
              "RF power 10 writes raw 26, which the radio displays as 10");
        check(rawFor(level::kMicGain).value_or(-1) == 26,
              "mic gain 10 uses the same radio-exact bucket");
        check(rawFor(level::kMonitor).value_or(-1) == 26,
              "monitor level 10 uses the same radio-exact bucket");
    }

    // ---- the CI-V stall detector ------------------------------------------
    //
    // The transport can be healthy while the COMMAND PLANE is dead: the control
    // stream keeps pinging, link statistics keep climbing, isConnected() stays
    // true, and the radio has answered no CI-V frame for a minute. That happened
    // on the bench and took real time to diagnose, because nothing anywhere said
    // so — every meter simply stopped at the same instant.
    //
    // What makes it triageable is naming the LAST COMMAND SENT. "The radio
    // stopped answering after 16 02 02" is a bug report; "the radio stopped
    // answering" is a guess.
    {
        const auto periodicallyRead = [&](std::uint8_t command, std::uint8_t sub) {
            return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                               [=](const CivFrame& f) {
                return f.cmd == command && f.hasSub && f.sub == sub && f.data.empty();
            });
        };

        // Prove reconciliation while CI-V is healthy. Once replies stop, each
        // transaction is deliberately bounded by the timeout; a silence test
        // should not require the scheduler to spray every register into a
        // black hole merely to prove those reads exist.
        radio.clearCivLog();
        check(waitFor([&] {
                  return periodicallyRead(cmd::kFunction, func::kNoiseReduce)
                      && periodicallyRead(cmd::kFunction, func::kNoiseBlanker)
                      && periodicallyRead(cmd::kLevel, level::kNrLevel)
                      && periodicallyRead(cmd::kLevel, level::kNbLevel);
              }, 5000),
              "NR/NB state and levels are periodically reconciled while CI-V is live");
    }

    // ---- the pan intents --------------------------------------------------
    //
    // The sweep pushed above put the radio at a 100 kHz span (200 kHz wide),
    // which is what both of these reason against.
    {
        // A ZOOM'S CENTRE MUST NOT RETUNE THE RADIO. Centre and bandwidth
        // travel together on a range change, so every zoom click carries a
        // centre; honouring it walked the VFO across the band one click at a
        // time. Refused, and re-asserted immediately so the view does not
        // drift for a frame before the next sweep contradicts it.
        radio.clearCivLog();
        QSignalSpy panSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
        backend.setPanCenter(QStringLiteral("0"), 14'050'000.0,
                             IRadioBackend::PanCenterIntent::Range);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        const bool retuned = std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx;
        });
        check(!retuned, "a zoom's pan-centre sends NO frequency command");
        check(panSpy.count() > 0,
              "and re-asserts the radio's real centre, so the view snaps back "
              "rather than drifting for a frame");
        if (panSpy.count() > 0) {
            check(std::fabs(panSpy.first().at(1).toDouble() - 14.1) < 1e-6,
                  "re-asserted at the radio's centre, not the requested one");
        }
    }

    {
        // A DRAG RETUNES, and does not snap back.
        //
        // In centre mode the scope window IS the operating frequency, so the
        // window cannot slide over stationary spectrum: refusing a drag left
        // the trace following the mouse for one frame and then jumping back,
        // which is the panadapter's most basic gesture reading as a bug. The
        // radio is at 14.1 MHz with a 200 kHz window; 14.05 is a quarter of the
        // span away, far outside the dead zone.
        radio.clearCivLog();
        QSignalSpy panSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
        backend.setPanCenter(QStringLiteral("0"), 14'050'000.0,
                             IRadioBackend::PanCenterIntent::Drag);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        auto it = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq;
        });
        check(it != sent.end(), "a pan DRAG reaches the radio as a frequency command");
        check(panSpy.count() == 0,
              "and does NOT re-assert the old centre — no snap-back");
    }

    {
        // THE DEAD ZONE. A click with a pixel of hand movement arrives here as
        // a centre a few Hz off; one-to-one tuning would move the dial on every
        // stray click. 200 Hz against a 100 kHz half-span is well inside the
        // 1% dead zone.
        radio.clearCivLog();
        backend.setPanCenter(QStringLiteral("0"), 14'100'200.0,
                             IRadioBackend::PanCenterIntent::Drag);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        const bool retuned = std::any_of(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kSetFreq || f.cmd == cmd::kSetFreqTrx;
        });
        check(!retuned, "a drag inside the dead zone sends no frequency command");
    }

    {
        // ZOOM OUT MUST ACTUALLY WIDEN. The UI scales by 1.5 and the radio's
        // spans step by 2 and 2.5, so nearest-snapping a widen request returned
        // the span already in use — at every one of the eight spans. The
        // operator clicked zoom out and nothing happened, permanently, because
        // the view is re-seeded from the radio's own sweep 30 times a second.
        radio.clearCivLog();
        // 200 kHz * 1.5 — the exact request the zoom-out button produces here.
        backend.setPanBandwidth(QStringLiteral("0"), 300'000.0);
        QTest::qWait(120);

        const auto& sent = radio.civCommands();
        auto it = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kScope && f.hasSub && f.sub == scope::kSpan;
        });
        check(it != sent.end(), "a zoom-out request reaches the radio as a span command");
        if (it != sent.end()) {
            // data[0] is the 0x27 family's leading fixed byte; the frequency
            // starts after it.
            check(!it->data.empty() && it->data.front() == 0x00,
                  "the span frame carries the leading fixed 0x00 the radio requires");
            const auto span = decodeFreq(
                std::span<const std::uint8_t>(it->data).subspan(1));
            check(span.has_value() && *span == 250'000,
                  "and steps to the NEXT span up (250 kHz), not back to 100 kHz");
        }
    }

    // ---- controls scrub: RE-ASSERT, never re-default -----------------------
    //
    // `controls scrub` is documented as leaving the radio untouched: it drives
    // every settable control AT ITS CURRENT VALUE and watches the wire, so the
    // question is "does this intent reach a register", not "does the radio
    // obey". That contract has exactly two ways to break, and both are silent —
    // the scrub reports the row LINKED either way, because the frame did reach
    // the wire. The radio is simply left somewhere else afterwards.
    {
        radio.clearCivLog();
        const QVariantMap res = backend.controlScrub(QString());
        check(waitSchedulerIdle(), "the full control scrub drains through the scheduler");
        const auto& sent = radio.civCommands();

        check(!res.contains(QStringLiteral("error")), "the scrub runs on a live session");
        check(res.value(QStringLiteral("broken")).toInt() == 0,
              "scheduler admission keeps the synchronous scrub from falsely "
              "reporting queued controls as broken");
        check(res.value(QStringLiteral("linked")).toInt() > 0,
              "the scrub reports controls admitted to the scheduler as linked");

        // 1. THE MIRROR MUST HOLD THE RADIO'S VALUE, NOT OUR DEFAULT.
        //
        // The scrub re-asserts from the backend's "last intent per control"
        // mirrors, and those were written only by the SETTERS — so on a session
        // where the operator had touched nothing, they still held their
        // construction defaults. The fake radio runs RF gain at 100 %; a scrub
        // reading a default mirror would have driven it to 0 and handed the
        // operator a deaf receiver, then called the row LINKED.
        auto rfGain = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kRf;
        });
        check(rfGain != sent.end(), "the scrub drives RF gain");
        if (rfGain != sent.end()) {
            const auto raw = decodeLevel(rfGain->data);
            check(raw.has_value() && *raw >= 250,
                  "and re-asserts the RADIO's 100%, not the mirror's construction "
                  "default of 0 — which would have deafened the receiver and "
                  "reported the row LINKED");
        }
        auto af = std::find_if(sent.begin(), sent.end(), [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kAf;
        });
        check(af != sent.end(), "the scrub drives AF gain");
        if (af != sent.end()) {
            const auto raw = decodeLevel(af->data);
            check(raw.has_value() && std::abs(*raw - 128) <= 2,
                  "at the radio's own ~50%, adopted through the ordinary decode "
                  "path rather than invented here");
        }

        // 2. A VALUE NOBODY ESTABLISHED IS NOT A CURRENT VALUE.
        //
        // The fake IC-705 deliberately does not answer the attenuator read —
        // one lost datagram on the lossy link this backend exists for looks
        // exactly the same. Re-asserting an unestablished mirror sends
        // "attenuator OFF" to an operator who may have it engaged. NOT-TESTED
        // is the honest third outcome, and the scrub already has that state;
        // this is the nr/nb/anf/notch sentinel rule, generalised.
        const bool touchedAtten = std::any_of(sent.begin(), sent.end(),
                                              [](const CivFrame& f) {
            return f.cmd == cmd::kAttenuator && !f.data.empty();
        });
        check(!touchedAtten,
              "a control the radio never reported is NOT driven — re-asserting an "
              "unestablished mirror would switch the operator's attenuator off");

        QString attenStatus;
        for (const QVariant& row : res.value(QStringLiteral("rows")).toList()) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("id")).toString() == QLatin1String("atten"))
                attenStatus = m.value(QStringLiteral("status")).toString();
        }
        check(attenStatus == QLatin1String("NOT-TESTED"),
              "and it is REPORTED as NOT-TESTED rather than LINKED, so the check "
              "does not claim coverage it does not have");

        // 3. PTT, the ATU and power-off are never scrubbed (Principle VI).
        //
        // Payload-bearing frames only — see movesPttOrTuner. The window this
        // reads spans the qWait above, so it also catches the backend's own
        // 250 ms PTT-state READ, which is not a scrub frame and keys nothing.
        // Matching that read made this assertion fail on roughly half of all
        // runs purely on timer phase, on a check whose whole job is to be
        // believed when it fires.
        const bool keyed = std::any_of(sent.begin(), sent.end(), movesPttOrTuner);
        check(!keyed, "and the scrub never touches PTT or the antenna tuner");
    }

    // ---- a refused mode correction must SURVIVE the caller ------------------
    //
    // SAM has no IC-705 equivalent, so the backend refuses it and re-asserts
    // what the radio is actually in — otherwise the mode indicator reads SAM
    // over an AM demodulator. The correction has to be QUEUED: SliceModel calls
    // us from modeChangeRequested and emits modeChanged(requestedMode) on the
    // line after that signal returns, so a direct emit here is applied and then
    // announced away, leaving the indicator lying exactly as it did before.
    {
        int corrections = 0;
        QString correctedMode;
        auto conn = QObject::connect(&backend, &IRadioBackend::sliceChanged, &app,
                                     [&](int, const SliceDelta& d) {
            if (d.mode) { ++corrections; correctedMode = *d.mode; }
        });
        backend.setSliceMode(0, QStringLiteral("SAM"));
        check(corrections == 0,
              "a refused mode emits NOTHING synchronously — a direct emit is "
              "overwritten by the caller's own modeChanged on the next line");
        QTest::qWait(50);
        check(corrections == 1 && !correctedMode.isEmpty(),
              "and the correction arrives on the next event-loop turn, naming the "
              "mode the radio is really in");
        QObject::disconnect(conn);
    }

    // ---- the mode combo is filled from the RADIO's vocabulary (#5040) -------
    //
    // WFM has been implemented end to end in CivCodec from the start — wire
    // value, both directions of the neutral mapping, its own 200 kHz filter slot
    // and a carrier-straddling passband. It was unreachable because nothing ever
    // published a modeList, so RxApplet and VfoWidget both fell through to their
    // compiled-in FlexRadio list, which has no WFM because a FLEX-6000 has no
    // WFM. The list is the whole fix; the combos already rebuild from it.
    {
        check(!publishedModeLists.empty(),
              "the backend publishes a mode list for the radio it identified");
        const QStringList& modes = publishedModeLists.back();
        check(modes.contains(QStringLiteral("WFM")),
              "and WFM is in it, so the IC-705 operator gets the button");
        // The stale-indicator half of the same defect: findText() returns -1 for
        // a mode the combo does not hold, so a radio put into WFM at the front
        // panel left the indicator showing the PREVIOUS mode. Every mode this
        // backend can report has to be reachable in the list.
        check(modes.contains(QStringLiteral("CW")) && modes.contains(QStringLiteral("CWL")),
              "including the CW names this backend actually reports");
        check(!modes.contains(QStringLiteral("CWU")),
              "without publishing the Flex-oriented CWU alias for Icom");
        check(!modes.contains(QStringLiteral("SAM")),
              "and NOT the modes setSliceMode refuses - a list that offers SAM "
              "on an Icom is a control that silently reverts");
    }

    // ---- WFM RECEIVES ONLY, and the client refuses to key in it -------------
    //
    // 76-108 MHz broadcast; the transmitter does not follow. The refusal lives
    // client-side because "the radio will say no" is not a property CI-V lets us
    // verify — an ignored key request is indistinguishable from one that worked
    // until the meters fail to move.
    //
    // TWO GATES, and this file can only see one of them. What the operator
    // notices — the TX indicator going back out, TUNE un-latching, the interlock
    // message — is RadioModel::refuseKeyInReceiveOnlyMode(), driven by the
    // receiveOnlyModes capability asserted below; a backend cannot reach
    // TransmitModel and so cannot clear any of it (#5106 review). What THIS gate
    // owns is narrower and still worth its own rows: no PTT frame leaves by any
    // path, whether or not it came through RadioModel.
    {
        check(backend.capabilities().receiveOnlyModes
                  == QStringList{QStringLiteral("WFM")},
              "the backend DECLARES WFM as receive-only, which is what arms the "
              "key guard in RadioModel");

        backend.setSliceMode(0, QStringLiteral("WFM"));
        check(waitSchedulerIdle(), "the radio converges on WFM");
        radio.clearCivLog();
        const std::size_t moxBefore = moxPublications.size();

        backend.setKeying(true);
        QTest::qWait(200);
        check(std::none_of(radio.civCommands().begin(), radio.civCommands().end(),
                           movesPttOrTuner),
              "no PTT frame reaches a radio that cannot transmit in WFM");
        // DELIBERATELY NOT a mox publication. The backend used to emit
        // TransmitDelta{mox=false} here and it cleared nothing: applyChanges
        // assigns backend mox to m_mox, which is already false, so no signal
        // reaches the indicator — which reads m_transmitting instead. Asserting
        // that delta asserted the implementation back to itself.
        check(moxPublications.size() == moxBefore,
              "and the backend does not fake a transmit edge for the refusal");

        // TUNE composes its carrier out of the same key, and it borrows the
        // RF-power register on the way. Refusing only inside setKeying() would
        // overwrite the operator's drive for a carrier that never happened.
        backend.setTxPower(37);
        check(waitSchedulerIdle(), "ordinary RF power converges before the refused TUNE");
        radio.clearCivLog();
        backend.setTune(true, 10);
        QTest::qWait(200);
        check(std::none_of(radio.civCommands().begin(), radio.civCommands().end(),
                           movesPttOrTuner),
              "TUNE does not key in WFM either");
        bool powerTouched = false;
        for (const CivFrame& f : radio.civCommands())
            if (f.cmd == cmd::kLevel && f.hasSub && f.sub == level::kRfPower)
                powerTouched = true;
        check(!powerTouched,
              "and the refused TUNE leaves the operator's RF power alone");

        // UNKEY IS NEVER GATED. A guard that could swallow an unkey is a stuck
        // transmitter, which is worse than the emission it prevents.
        backend.setSliceMode(0, QStringLiteral("USB"));
        check(waitSchedulerIdle(), "back to a transmit mode");
        backend.setKeying(true);
        check(waitFor([&] {
                  return std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                                     movesPttOrTuner);
              }, 1000),
              "keying works again once the radio is in a transmit mode");
        backend.setKeying(false);
        check(waitSchedulerIdle(), "and the unkey converges");
    }

    // ---- the CI-V trace tag decodes the RIGHT byte in BOTH directions -------
    //
    // The two call sites hand traceCiv DIFFERENT layouts, and the tag decoder
    // has to follow:
    //
    //   TX (sendUserCommand) — the raw wire frame from buildFrame:
    //       FE FE <to> <from> <cmd> [<sub>] <data…> FD    -> cmd at index 4
    //   RX (onCivFrame)      — re-serialised, envelope deliberately dropped:
    //       <cmd> [<sub>] <data…>                         -> cmd at index 0
    //
    // Reading index 4 for both is the defect this guards: on RX it printed a
    // PAYLOAD byte as the command, and printed nothing at all for frames
    // shorter than five bytes — which is most of them.
    //
    // These assertions are falsifiable by construction: restore the old
    // `frame[4]` / `size() >= 5` form and the RX cases below fail, because
    // 1A 06 01 01 is four bytes (no tag at all) and a frequency reply puts a
    // BCD digit pair where the command was assumed to be.
    {
        QLoggingCategory::setFilterRules(QStringLiteral("aether.icom.civ.debug=true"));
        g_prevHandler = qInstallMessageHandler(civCapture);
        g_capturing = true;

        // RX, SHORT FRAME — the 1A 06 DATA-flag reply. Four bytes once the
        // envelope is stripped, so the old `size() >= 5` guard skipped it
        // silently. This is the exact reply the category was added to make
        // visible (the radio never volunteers it), so "no tag" is the whole
        // failure rather than a cosmetic one.
        g_civLines.clear();
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr,
                       cmd::kSetting, 0x06, 0x01, 0x01, kCivEom});
        QTest::qWait(120);
        {
            const QString line = lastLineStartingWith(QStringLiteral("RX <- 1a 06"));
            check(!line.isNull(), "the 1A 06 DATA-flag reply reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=1a")),
                  "and a four-byte RX frame is TAGGED — the old size()>=5 guard "
                  "dropped the tag on the very reply this category exists for");
            check(line.contains(QStringLiteral("sub=06")),
                  "with the subcommand read from index 1, not index 5");
        }

        // RX, LONG FRAME — a frequency report. Six bytes, so the old guard
        // PASSED and produced a confidently wrong label: for 14.074 MHz the
        // BCD payload puts 0x14 at index 4, which is cmd::kLevel, so a
        // frequency reply was printed as "cmd=14 sub=00" — a level command.
        // Plausible, wrong, and aimed at someone who switched this on because
        // they no longer trust their reading of the code.
        g_civLines.clear();
        radio.pushCiv({0xFE, 0xFE, kControllerAddress, kIc705Addr,
                       cmd::kReadFreq, 0x00, 0x40, 0x07, 0x14, 0x00, kCivEom});
        QTest::qWait(120);
        {
            const QString line = lastLineStartingWith(QStringLiteral("RX <- 03"));
            check(!line.isNull(), "the frequency report reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=03")),
                  "a frequency reply is tagged as READ-FREQUENCY, not as the "
                  "level command its BCD payload happens to spell at index 4");
            check(!line.contains(QStringLiteral("sub=")),
                  "and read-frequency carries no subcommand, so none is invented");
        }

        // TX — unchanged behaviour, asserted so the RX fix cannot silently
        // break the direction that was already right. The raw wire frame keeps
        // its envelope, so the command really is at index 4 here. AF gain is
        // command 14 SUB 01, which is a subcommand-bearing frame — the case
        // where a wrong index would show.
        g_civLines.clear();
        backend.setSliceAudioGain(0, 42);
        QTest::qWait(120);
        {
            // The scheduler keeps draining unrelated commands during the wait,
            // so select the AF-gain frame rather than whichever TX happened last.
            const QString line =
                lastLineStartingWith(QStringLiteral("TX -> fe fe a4 e0 14 01"));
            check(!line.isNull(), "the outbound frame reaches the CI-V trace");
            check(line.contains(QStringLiteral("cmd=14")) && line.contains(QStringLiteral("sub=01")),
                  "a TX frame still decodes from index 4/5, past the envelope");
        }

        g_capturing = false;
        qInstallMessageHandler(g_prevHandler);
        QLoggingCategory::setFilterRules(QString());
    }

    // ---- health -----------------------------------------------------------
    {
        const auto h = backend.healthSnapshot();
        check(!h.isEmpty(), "the health snapshot is populated");
        // The IC-705 reports no PA temperature, and the key is OMITTED rather
        // than reported as zero — "not reported" and "0 degrees" are different
        // answers on a health readout.
        check(!h.values.contains(QStringLiteral("patemp")),
              "no PA temperature key, because the radio does not report one");
        check(h.values.contains(QStringLiteral("model")), "the resolved model is reported");
        check(h.values.contains(QStringLiteral("lease"))
                  && h.values.value(QStringLiteral("lease")).toString().contains(
                      QStringLiteral("authenticated")),
              "health separates the authenticated RS-BA1 lease from UDP link liveness");
        check(h.values.contains(QStringLiteral("leaseseq"))
                  && h.values.contains(QStringLiteral("leasecounts")),
              "health exposes renewal sequence and reply counters");

        QVariant leaseResult;
        bool leaseAnswered = false;
        auto leaseConn = QObject::connect(
            &backend, &IRadioBackend::extensionResult, &app,
            [&](quint64 id, const QVariant& result) {
                if (id == 7300) {
                    leaseAnswered = true;
                    leaseResult = result;
                }
            });
        backend.invokeExtension(QStringLiteral("icom"), QStringLiteral("civ.session"),
                                7300, {});
        QObject::disconnect(leaseConn);
        const QVariantMap lease = leaseResult.toMap();
        check(leaseAnswered && lease.value(QStringLiteral("authenticated")).toBool(),
              "civ.session synchronously returns the live authenticated lease");
        check(lease.value(QStringLiteral("lastRenewalResponse")).toString()
                  == QStringLiteral("0x00000000"),
              "civ.session preserves the protocol response word for troubleshooting");
        check(lease.value(QStringLiteral("tokenRequestId")).toString().startsWith(
                  QStringLiteral("0x")),
              "civ.session exposes the per-login token-request correlation ID");
        check(lease.contains(QStringLiteral("reissuedTokens")),
              "civ.session distinguishes reconnect token reissue from renewal rejection");
        check(lease.value(QStringLiteral("initialMaintenanceMs")).toInt() == 30000,
              "civ.session exposes the one-time early maintenance window");
    }

    // An operator disconnect while TUNE is active must unkey and restore the
    // borrowed RF-power register before the serial command path disappears.
    backend.setTxPower(37);
    check(waitSchedulerIdle(), "disconnect fixture's ordinary power converges");
    backend.setTune(true, 10);
    check(waitSchedulerIdle(), "disconnect fixture reaches active TUNE");
    radio.setCivSilent(true);
    backend.setPanPreamp(QStringLiteral("0"), 1);
    QSignalSpy disconnectWaiterSpy(&backend, &IRadioBackend::extensionResult);
    const quint64 disconnectWaiterId = 0x5120;
    backend.invokeExtension(QStringLiteral("icom"),
                            QStringLiteral("civ.scheduler.wait-idle"),
                            disconnectWaiterId,
                            QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
    radio.clearCivLog();
    backend.disconnectRadio();
    QTest::qWait(100);
    QVariantMap disconnectWaiterResult;
    for (const QList<QVariant>& reply : disconnectWaiterSpy) {
        if (reply.at(0).toULongLong() == disconnectWaiterId) {
            disconnectWaiterResult = reply.at(1).toMap();
            break;
        }
    }
    check(disconnectWaiterResult.value(QStringLiteral("outcome")).toString()
              == QLatin1String("cancelled")
              && disconnectWaiterResult.value(QStringLiteral("cancelled")).toBool(),
          "ordinary disconnect explicitly cancels a pending scheduler waiter");
    const auto restoreOnDisconnect = std::find_if(
        radio.civCommands().begin(), radio.civCommands().end(),
        [](const CivFrame& f) {
            return f.cmd == cmd::kLevel && f.hasSub
                && f.sub == level::kRfPower
                && decodeLevel(f.data).value_or(-1) >= 93;
        });
    check(restoreOnDisconnect != radio.civCommands().end(),
          "disconnect during TUNE restores ordinary RF power before teardown");
    check(std::any_of(radio.civCommands().begin(), radio.civCommands().end(),
                      [](const CivFrame& frame) {
              return frame.cmd == cmd::kCwMessage && frame.data.size() == 1
                  && frame.data.front() == 0xFF;
          }),
          "disconnect aborts radio-buffered CW with 17 FF before teardown");
    check(!backend.isConnected(), "the backend disconnects cleanly");

    // A NETWORK RADIO NAME IS A NICKNAME, NOT A MODEL DESIGNATION.
    //
    // The primary fake above deliberately uses "IC 705", which resolves through
    // modelForName() and therefore cannot exercise the custom-name branch. Use a
    // name that no model table can recognise, snapshot the seam on connected(),
    // then let the authoritative 0x19 0x00 response arrive. This pins all three
    // promises of the identity split: the nickname beats the connect edge, the
    // later model response does not overwrite it, and capabilities keep reporting
    // the hardware model rather than reclassifying the nickname as one.
    {
        FakeIc705 namedRadio;
        namedRadio.setDeviceName("Shack 705");
        IcomCivBackend namedBackend;

        QString publishedNickname;
        QString publishedModel;
        QString nicknameAtConnect;
        QString modelAtConnect;
        QObject::connect(&namedBackend, &IRadioBackend::radioChanged, &app,
                         [&](const RadioDelta& d) {
                             if (d.nickname) {
                                 publishedNickname = *d.nickname;
                             }
                             if (d.model) {
                                 publishedModel = *d.model;
                             }
                         });
        QObject::connect(&namedBackend, &IRadioBackend::connected, &app, [&] {
            nicknameAtConnect = publishedNickname;
            modelAtConnect = publishedModel;
        });

        RadioConnectRequest namedRequest;
        namedRequest.host = QStringLiteral("127.0.0.1");
        namedRequest.port = namedRadio.controlPort();
        namedRequest.params.insert(QStringLiteral("icom.serialPort"),
                                   namedRadio.serialPort());
        namedRequest.params.insert(QStringLiteral("icom.audioPort"),
                                   namedRadio.audioPort());
        namedRequest.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
        namedRequest.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
        namedRequest.params.insert(QStringLiteral("icom.civAddress"), 0xA4);

        namedBackend.connectRadio(namedRequest);
        check(waitFor([&] { return namedBackend.isConnected(); }),
              "a radio with a custom Network Radio Name connects");
        check(nicknameAtConnect == QStringLiteral("Shack 705"),
              "and publishes that custom name as its nickname before connected()");
        check(modelAtConnect.isEmpty(),
              "without misreporting the custom nickname as an early hardware model");
        check(waitFor([&] { return publishedModel == QStringLiteral("IC-705"); }),
              "then adopts the canonical model from the authoritative 0x19 0x00 response");
        check(publishedNickname == QStringLiteral("Shack 705"),
              "without overwriting the custom nickname during model resolution");
        check(namedBackend.capabilities().model == QStringLiteral("IC-705"),
              "and reports the resolved hardware model through RadioCapabilities");

        namedBackend.disconnectRadio();
        check(!namedBackend.isConnected(), "the custom-name backend disconnects cleanly");
    }

    // =======================================================================
    // CI-V ADDRESS RESOLUTION
    // =======================================================================
    //
    // CI-V is ADDRESSED, and a wrong address fails in complete silence: the
    // radio ignores the frame, there is no error, and the session comes up
    // looking healthy with no frequency, no scope and no transmit. Every check
    // below is written around that silence — which is why they assert on the
    // `to` byte of what actually went out and on state that only arrives if
    // something answered, rather than on "it connected".
    //
    // Shared rig: a fake standing in for whichever Icom the case needs.
    struct CivCase {
        FakeIc705 radio;
        IcomCivBackend backend;
        double frequencyMHz = 0.0;
        int frequencyPublications = 0;
        QStringList warnings;

        CivCase(std::uint8_t addr, const char* name, bool echo = true)
        {
            radio.setCivAddress(addr);
            radio.setDeviceName(name);
            radio.setCivEcho(echo);
            QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                             [this](int, const SliceDelta& d) {
                                 if (d.frequency) {
                                     frequencyMHz = *d.frequency;
                                     ++frequencyPublications;
                                 }
                             });
            QObject::connect(&backend, &IRadioBackend::configurationWarning, &backend,
                             [this](const QString& m) { warnings << m; });
        }

        RadioConnectRequest request()
        {
            RadioConnectRequest r;
            r.host = QStringLiteral("127.0.0.1");
            r.port = radio.controlPort();
            r.params.insert(QStringLiteral("icom.serialPort"), radio.serialPort());
            r.params.insert(QStringLiteral("icom.audioPort"), radio.audioPort());
            r.params.insert(QStringLiteral("icom.username"), QStringLiteral("beer"));
            r.params.insert(QStringLiteral("icom.password"), QStringLiteral("beerbeer"));
            return r;
        }

        // How many frames the client addressed to `to`. Counted from the
        // radio's own log, which records everything the bus carried including
        // the frames it then ignored — so this reports where we SENT, not where
        // we were answered, and those are the two different things this whole
        // change is about.
        [[nodiscard]] int sentTo(std::uint8_t to) const
        {
            return static_cast<int>(std::count_if(
                radio.civCommands().begin(), radio.civCommands().end(),
                [to](const CivFrame& f) { return f.to == to; }));
        }
        // ...AND WHICH COMMAND went there. sentTo() alone cannot tell "the
        // reads recovered" from "the scope switches recovered", and after a
        // retarget those are two different questions with two different
        // answers - which is exactly how a black panadapter hid behind a
        // passing test.
        [[nodiscard]] int sentCmdTo(std::uint8_t to, std::uint8_t command) const
        {
            return static_cast<int>(std::count_if(
                radio.civCommands().begin(), radio.civCommands().end(),
                [to, command](const CivFrame& f) {
                    return f.to == to && f.cmd == command;
                }));
        }
        [[nodiscard]] bool warnedAbout(const char* fragment) const
        {
            return std::any_of(warnings.begin(), warnings.end(),
                               [fragment](const QString& w) {
                                   return w.contains(QLatin1String(fragment));
                               });
        }
    };

    // A user-visible Network Radio Name is only a handshake hint. The 0x19
    // reply is authoritative and can replace the hinted model's meter set —
    // here IC-705 Vd+Id becomes IC-9700 Vd-only. Definitions must be replaced
    // by stable identity, not compacted into new indices, or OVF takes a former
    // meter slot and the status bar reads an overflow flag as telemetry.
    {
        CivCase c(0xA2, "IC-705");
        QMap<int, MeterDef> activeMeters;
        QList<int> removedMeters;
        QObject::connect(&c.backend, &IRadioBackend::meterDefined, &c.backend,
                         [&activeMeters](const MeterDef& def) {
                             activeMeters.insert(def.index, def);
                         });
        QObject::connect(&c.backend, &IRadioBackend::meterRemoved, &c.backend,
                         [&activeMeters, &removedMeters](int index) {
                             activeMeters.remove(index);
                             removedMeters.append(index);
                         });

        c.backend.connectRadio(c.request());
        check(waitFor([&] {
                  return c.backend.isConnected()
                      && c.backend.model().civAddress == 0xA2;
              }),
              "identity correction: the wire identity replaces the handshake profile");

        const int vdIndex = static_cast<int>(MeterId::Vd);
        const int idIndex = static_cast<int>(MeterId::Id);
        const int overflowIndex = static_cast<int>(MeterId::Overflow);
        check(!removedMeters.contains(vdIndex) && removedMeters.contains(idIndex),
              "identity correction: IC-9700 retains Vd and withdraws unsupported Id");
        check(activeMeters.contains(vdIndex) && !activeMeters.contains(idIndex),
              "identity correction: IC-9700 publishes voltage without claiming current");
        check(activeMeters.contains(overflowIndex)
                  && activeMeters.value(overflowIndex).name == QStringLiteral("OVF"),
              "identity correction: OVF retains its stable meter identity");
        check(activeMeters.size() == 7,
              "identity correction: calibrated-independent meters plus Vd remain");

        c.radio.clearCivLog();
        check(waitFor([&] {
                  return std::ranges::any_of(
                      c.radio.civCommands(), [](const CivFrame& frame) {
                          return frame.cmd == cmd::kMeter && frame.hasSub
                              && frame.sub == meter::kVd;
                      });
              }, 2500),
              "identity correction: IC-9700 schedules its supported Vd poll");
        check(std::ranges::none_of(c.radio.civCommands(), [](const CivFrame& frame) {
                  return frame.cmd == cmd::kMeter && frame.hasSub
                      && frame.sub == meter::kId;
              }),
              "identity correction: IC-9700 does not poll unsupported Id");

        c.backend.disconnectRadio();
    }

    // XFC release is an obligation created when ON is sent, not a capability
    // lookup repeated at mouse-up. Authoritative identity can narrow the
    // profile while the control is held; that transition must send OFF to the
    // newly reported address before the UI loses the momentary control.
    {
        CivCase c(0x98, "IC-705");
        bool xfcPressedAtConnect = false;
        QObject::connect(&c.backend, &IRadioBackend::connected, &c.backend, [&] {
            xfcPressedAtConnect = true;
            c.backend.setTransmitFrequencyCheck(true);
        });
        c.backend.connectRadio(c.request());
        check(waitFor([&] {
                  return c.backend.isConnected()
                      && c.backend.model().civAddress == 0x98
                      && !c.backend.capabilities().hasTransmitFrequencyCheck;
              }),
              "XFC capability withdrawal: authoritative identity narrows the profile");
        check(xfcPressedAtConnect,
              "XFC capability withdrawal: ON was requested under the handshake profile");
        check(waitFor([&] {
                  return std::ranges::any_of(c.radio.civCommands(), [](const CivFrame& frame) {
                      return frame.to == 0x98 && frame.cmd == cmd::kControl
                          && frame.hasSub && frame.sub == control::kXfc
                          && frame.data == std::vector<std::uint8_t>{0x00};
                  });
              }),
              "XFC capability withdrawal: OFF follows the corrected identity");
        c.backend.disconnectRadio();
    }

    // ---- MODEL-SCOPED CI-V STALL RECOVERY -------------------------------
    // Targeted 0x04 data-pipe restart is measured only on IC-9700. Other Icom
    // models retain main's established warn-only behavior.
    {
        CivCase c(0xA2, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "IC-9700 recovery: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 0.0; }),
              "IC-9700 recovery: startup reaches a live command plane");

        // The fake remains deaf until the restart envelope itself reopens its
        // command plane. Its delayed frequency reply intentionally lands after
        // the scheduler's 350 ms matching window, reproducing a valid late
        // reply that is classified Unmatched rather than Accepted.
        c.radio.setCivRestartRecovery(true, 500);
        c.radio.setCivSilent(true);
        c.backend.setPanPreamp(QStringLiteral("0"), 1);
        QSignalSpy healthSpy(&c.backend, &IRadioBackend::linkStatsUpdated);
        QSignalSpy waiterSpy(&c.backend, &IRadioBackend::extensionResult);
        c.backend.invokeExtension(QStringLiteral("icom"),
                                  QStringLiteral("civ.scheduler.wait-idle"),
                                  0x5119,
                                  QVariantMap{{QStringLiteral("timeoutMs"), 10000}});
        check(waitFor([&] {
                  return IcomCivBackendTestAccess::recoveryActive(c.backend);
              }, 8000),
              "IC-9700 recovery: targeted restart begins only after a real stall");
        check(healthSpy.count() > 0,
              "IC-9700 recovery: transport statistics continue during CI-V silence");

        QVariantMap waiterResult;
        for (const QList<QVariant>& reply : waiterSpy) {
            if (reply.at(0).toULongLong() == 0x5119) {
                waiterResult = reply.at(1).toMap();
                break;
            }
        }
        check(waiterResult.value(QStringLiteral("outcome")).toString()
                  == QLatin1String("failed"),
              "IC-9700 recovery: the detected stall explicitly fails waiters");

        // A frequency-shaped frame from another CI-V device may be present on
        // a shared bus. It must not verify this radio's recovery probe.
        std::vector<std::uint8_t> otherFrequency{
            0xFE, 0xFE, kControllerAddress, 0x98, cmd::kReadFreq,
        };
        const std::vector<std::uint8_t> encoded = encodeFreq(14'200'000);
        otherFrequency.insert(otherFrequency.end(), encoded.begin(), encoded.end());
        otherFrequency.push_back(kCivEom);
        c.radio.pushCiv(otherFrequency);
        QTest::qWait(100);
        check(IcomCivBackendTestAccess::recoveryActive(c.backend),
              "IC-9700 recovery: another device's frequency frame cannot verify the probe");

        check(waitFor([&] {
                  return !IcomCivBackendTestAccess::recoveryActive(c.backend);
              }, 2500),
              "IC-9700 recovery: the restart causally restores a delayed selected-radio reply");
        check(c.backend.isConnected(),
              "IC-9700 recovery: verified targeted restart preserves the session");
    }

    {
        CivCase c(0xA2, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "IC-9700 exhaustion: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 0.0; }),
              "IC-9700 exhaustion: startup reaches a live command plane");
        c.radio.setCivSilent(true);
        c.backend.setPanPreamp(QStringLiteral("0"), 1);
        QSignalSpy errorSpy(&c.backend, &IRadioBackend::connectionError);
        check(waitFor([&] { return !c.backend.isConnected(); }, 12000),
              "IC-9700 exhaustion: bounded restarts fall back to session replacement");
        const std::vector<qint64>& attempts = c.radio.serialRestartTimesMs();
        check(IcomCivBackendTestAccess::maxRecoveryAttempts(c.backend) == 3
                  && attempts.size() == 3,
              "IC-9700 exhaustion: exactly three targeted restarts are attempted");
        check(IcomCivBackendTestAccess::recoveryIntervalMs(c.backend) == 1000,
              "IC-9700 exhaustion: configured retry cadence remains one second");
        bool cadenceValid = attempts.size() == 3;
        for (std::size_t i = 1; cadenceValid && i < attempts.size(); ++i) {
            const qint64 elapsed = attempts[i] - attempts[i - 1];
            cadenceValid = elapsed >= 900;
        }
        check(cadenceValid,
              "IC-9700 exhaustion: observed restart attempts follow one-second cadence");
        check(!errorSpy.isEmpty(),
              "IC-9700 exhaustion: fallback reports the full-session reconnect reason");
    }

    for (const auto& [address, modelName] :
         std::array<std::pair<std::uint8_t, const char*>, 2>{{
             {0xA4, "IC-705"}, {0xB6, "IC-7300MK2"}}}) {
        CivCase c(address, modelName);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "non-9700 stall: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 0.0; }),
              "non-9700 stall: startup reaches a live command plane");
        c.radio.setCivSilent(true);
        c.backend.setPanPreamp(QStringLiteral("0"), 1);
        const auto terminalBefore =
            IcomCivBackendTestAccess::terminalRequestCounts(c.backend);
        QTest::qWait(7000);
        check(c.backend.isConnected(),
              "non-9700 stall: main's established warn-only behavior is retained");
        check(c.radio.serialRestartTimesMs().empty(),
              "non-9700 stall: no unverified 0x04 data restart is sent");
        check(!IcomCivBackendTestAccess::recoveryActive(c.backend),
              "non-9700 stall: no model-specific recovery timer starts");
        check(IcomCivBackendTestAccess::terminalRequestCounts(c.backend)
                  == terminalBefore,
              "non-9700 stall: the shared scheduler is not reset or failed");
        check(IcomCivBackendTestAccess::linkPollIntervalMs(c.backend) == 1000,
              "non-9700 stall: the existing link timer cadence is unchanged");
    }

    // MainWindow/RadioModel application shutdown invokes the backend's public
    // disconnect before object destruction. Exercise that complete backend
    // route separately from both IcomSession's partial-login stop test and the
    // ordinary mid-session disconnect fixture above.
    {
        CivCase c(0xA4, "IC-705");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "app shutdown: the backend session comes up");
        IcomCivBackendTestAccess::stopPollers(c.backend);
        check(waitFor([&] {
                  return IcomCivBackendTestAccess::pumpUntilIdle(c.backend);
              }),
              "app shutdown: startup work drains");
        c.radio.setCivSilent(true);
        c.backend.setPanPreamp(QStringLiteral("0"), 1);
        c.radio.clearCivLog();
        c.backend.disconnectRadio();
        check(!c.backend.isConnected(),
              "app shutdown: backend disconnect completes synchronously");
        check(waitFor([&] { return c.radio.deauthOuterSequences().size() >= 2; }),
              "app shutdown: token removal is sent before session destruction");
        check(std::ranges::none_of(c.radio.civCommands(), [](const CivFrame& frame) {
                  return frame.cmd == cmd::kControl && frame.hasSub
                      && frame.sub == control::kPtt && !frame.data.empty()
                      && frame.data.front() != 0;
              }),
              "app shutdown: teardown cannot key or re-key the transmitter");
        const auto [cancelled, failed] =
            IcomCivBackendTestAccess::terminalRequestCounts(c.backend);
        check(cancelled > 0 && failed == 0,
              "app shutdown: discarded commands are consumed as cancellations");
    }

    // ---- AUTO: the bug, and the fix ---------------------------------------
    //
    // An IC-9700 lives on 0xA2. Before this change, an operator who left the
    // CI-V field blank got 0xA4 — the IC-705's address — hardcoded at the
    // connect path, so every read went somewhere nobody was listening. The
    // model still resolved by name, capabilities still published, and the
    // result read as "this backend has no panadapter yet".
    {
        CivCase c(0xA2, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "auto: the session comes up on an IC-9700");

        // THE DISCRIMINATING ASSERTION. Not "it connected" and not "the model
        // resolved" — both of those were already true on the broken path.
        check(waitFor([&] { return c.sentTo(0xA2) > 1; }),
              "auto: the reads are addressed to 0xA2, the address this radio "
              "actually answers on");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "auto: and the radio ANSWERS - a frequency arrives, where the "
              "0xA4-hardcoded path produced a permanently blank session");
        check(c.backend.model().civAddress == 0xA2, "auto: resolved as the IC-9700");
        const RadioCapabilities caps = c.backend.capabilities();
        check(caps.txPowerBands.size() == 3
                  && caps.txPowerMaxWattsAt(146'000'000.0) == 100.0
                  && caps.txPowerMaxWattsAt(432'000'000.0) == 75.0
                  && caps.txPowerMaxWattsAt(1'296'000'000.0) == 10.0,
              "auto: IC-9700 capabilities carry all three PA ceilings");

        // cmd::kSetFreq (0x05), NOT cmd::kSetFreqTrx (0x00). 0x00 is the
        // TRANSCEIVE frame a radio sends the controller when its own VFO
        // moved; AetherSDR never emits it, so counting it made this assertion
        // 0 == 0 whether or not the gate existed. Verified by neutering the
        // gate: with 0x00 the check still passed while a real set-frequency
        // went out on the wire. cmdSetFrequency() builds 0x05 — CivCodec.cpp.
        const int tuneCommandsBefore = static_cast<int>(std::ranges::count_if(
            c.radio.civCommands(), [](const CivFrame& frame) {
                return frame.cmd == cmd::kSetFreq;
            }));
        const int frequencyPublicationsBefore = c.frequencyPublications;
        const double actualFrequencyBefore = c.frequencyMHz;
        c.backend.setSliceFrequency(0, 500'000'000.0);
        check(waitFor([&] {
                  return c.frequencyPublications > frequencyPublicationsBefore;
              }),
              "auto: a rejected IC-9700 gap tune still produces a publication");
        const int tuneCommandsAfter = static_cast<int>(std::ranges::count_if(
            c.radio.civCommands(), [](const CivFrame& frame) {
                return frame.cmd == cmd::kSetFreq;
            }));
        check(tuneCommandsAfter == tuneCommandsBefore,
              "auto: an IC-9700 gap frequency sends no CI-V tune command");
        check(c.warnedAbout("outside its supported bands"),
              "auto: an IC-9700 gap frequency explains why it was rejected");
        check(c.frequencyMHz == actualFrequencyBefore,
              "auto: the corrective frequency is radio state, not the refused request");

        // ONE broadcast per connect. Never polled, never retried on a timer:
        // RFC #4983 attributes an unrecoverable CI-V stall to request volume,
        // so the cost of this feature has to stay at exactly one frame.
        const int broadcasts = c.sentTo(kBroadcastAddress);
        check(broadcasts == 1,
              "auto: exactly one broadcast 0x19 0x00 is sent for the whole connect");
    }

    // ---- THE GATE IS IC-9700-ONLY, PROVEN ON THE WIRE ---------------------
    //
    // #5116's non-goals name IC-705 and IC-7300MK2 explicitly: neither may
    // change tuning behaviour. The gate above keys on `bandsFor()` being
    // non-empty, which today is the IC-9700 alone — but "the predicate reads
    // right" is not evidence, and the assertion that WAS meant to carry this
    // was counting a command AetherSDR never sends.
    //
    // So ask it of the wire instead: 500 MHz is above the IC-705's 470 MHz
    // ceiling AND far above the IC-7300MK2's 74.8 MHz, so a gate that ever
    // generalised to model.tuningMinHz/tuningMaxHz would silence both. The
    // discriminating outcome is that the set-frequency still goes out, and no
    // band warning is raised.
    for (const auto& [addr, label] :
         std::array<std::pair<std::uint8_t, const char*>, 2>{{{0xA4, "IC-705"},
                                                             {0xB6, "IC-7300MK2"}}}) {
        CivCase c(addr, label);
        std::vector<std::vector<std::uint8_t>> startupInventory;
        QObject::connect(&c.backend, &IRadioBackend::connected, &app, [&] {
            // connected() is emitted after the complete connect snapshot has
            // been queued and before the first pump. Capture that exact edge;
            // responses and periodic timers cannot add follow-up work yet.
            startupInventory = IcomCivBackendTestAccess::queuedFrames(c.backend);
            QTimer::singleShot(0, &c.backend, [&] {
                IcomCivBackendTestAccess::stopPollers(c.backend);
            });
        });
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "continuous model: the session comes up");
        check(waitFor([&] { return c.backend.model().civAddress == addr; }),
              "continuous model: it resolved to the address under test");
        check(bandsFor(c.backend.model()).empty(),
              "continuous model: it declares no discontinuous band table, so "
              "the IC-9700 gate cannot apply to it");

        std::vector<std::vector<std::uint8_t>> expectedStartup{
            cmdReadId(kBroadcastAddress),
            cmdReadId(addr),
            cmdReadFrequency(addr),
            cmdReadMode(addr),
            cmdReadVfoMode(addr),
        };
        const std::vector<int> startupSetItems = addr == 0xA4
            ? std::vector<int>{118, 119, 116, 117}
            : std::vector<int>{84, 85, 81, 82, 83};
        for (int item : startupSetItems) {
            expectedStartup.push_back(cmdReadSetting(addr, item));
        }
        for (std::uint8_t which : {
                 level::kRfPower, level::kAf, level::kSquelch,
                 level::kMicGain, level::kCompLevel, level::kMonitor,
                 level::kNrLevel, level::kNbLevel, level::kNotchPos,
                 level::kRf, level::kVoxGain, level::kCwPitch,
                 level::kKeySpeed}) {
            expectedStartup.push_back(cmdReadLevel(addr, which));
        }
        for (std::uint8_t function : {
                 func::kPreamp, func::kAgc, func::kNoiseReduce,
                 func::kNoiseBlanker, func::kAutoNotch,
                 func::kManualNotch, func::kCompressor,
                 func::kMonitorFn, func::kVox, func::kBreakIn}) {
            expectedStartup.push_back(cmdReadFunction(addr, function));
        }
        expectedStartup.push_back(cmdReadRepeaterOffsetDirection(addr));
        expectedStartup.push_back(cmdReadRepeaterOffset(addr));
        expectedStartup.push_back(cmdReadFunction(addr, func::kRepeaterTone));
        expectedStartup.push_back(cmdReadRepeaterTone(addr));
        expectedStartup.push_back(cmdReadTransmitFrequencyCheck(addr));
        expectedStartup.push_back(cmdReadFilterWidth(addr));
        expectedStartup.push_back(cmdReadLevel(addr, level::kPbtInner));
        expectedStartup.push_back(cmdReadLevel(addr, level::kPbtOuter));
        expectedStartup.push_back(cmdReadFunction(addr, func::kTxBandwidth));
        expectedStartup.push_back(cmdReadAttenuator(addr));
        expectedStartup.push_back(cmdReadTuneOffset(addr, tuneOffset::kFrequency));
        expectedStartup.push_back(cmdReadTuneOffset(addr, tuneOffset::kRitOnOff));
        expectedStartup.push_back(cmdReadTuneOffset(addr, tuneOffset::kXitOnOff));
        expectedStartup.push_back(cmdReadTuner(addr));
        expectedStartup.push_back(cmdScopeOnOff(addr, true));
        expectedStartup.push_back(cmdScopeDataOutput(addr, true));
        check(startupInventory == expectedStartup,
              "continuous model: startup inventory, order, and request count are unchanged");
        const QVariantList controls = IcomCivBackendTestAccess::controls(c.backend);
        const bool extendedRowsStayUnsupported = std::ranges::all_of(
            controls, [](const QVariant& value) {
                const QVariantMap row = value.toMap();
                const QString feature = row.value(QStringLiteral("profileFeature")).toString();
                return feature != QLatin1String("fm-repeater-extended-readback")
                    || !row.value(QStringLiteral("supported")).toBool();
            });
        check(extendedRowsStayUnsupported,
              "continuous model: extended IC-9700 readback is absent from the "
              "IC-705/IC-7300MK2 effective control surface");

        // #5119 freezes the healthy IC-705 / IC-7300MK2 scheduler contract.
        // Pin every distinct steady-state group, including the phase-12 SET
        // leaves whose numbers differ by model. Exact order, item and count are
        // observable on the wire; the timer interval pins their cadence.
        check(IcomCivBackendTestAccess::linkPollIntervalMs(c.backend) == 1000,
              "continuous model: healthy control reconciliation remains on its 1 s cadence");
        IcomCivBackendTestAccess::stopPollers(c.backend);
        check(waitFor([&] { return IcomCivBackendTestAccess::pumpUntilIdle(c.backend); }),
              "continuous model: startup work drains before poll inventory capture");
        using CommandSignature = std::array<int, 3>; // command, sub, SET item
        const auto signature = [](const CivFrame& frame) {
            int item = -1;
            if (frame.cmd == cmd::kSetting && frame.hasSub
                && frame.sub == 0x05 && frame.data.size() >= 2) {
                item = decodeBcdByte(frame.data[0]) * 100
                    + decodeBcdByte(frame.data[1]);
            }
            return CommandSignature{frame.cmd, frame.hasSub ? frame.sub : -1, item};
        };
        const std::vector<CommandSignature> basePolls{
            {cmd::kFunction, func::kAutoNotch, -1},
            {cmd::kFunction, func::kManualNotch, -1},
            {cmd::kFunction, func::kNoiseReduce, -1},
            {cmd::kFunction, func::kNoiseBlanker, -1},
            {cmd::kFunction, func::kRepeaterTone, -1},
        };
        std::vector<CommandSignature> phase2Extra{
            {cmd::kReadFreq, -1, -1},
            {cmd::kVfoMode, vfoMode::kSelected, -1},
            {cmd::kDuplex, -1, -1},
            {cmd::kReadRepeaterOffset, -1, -1},
            {cmd::kTone, 0x00, -1},
            {cmd::kFunction, func::kMonitorFn, -1},
            {cmd::kFunction, func::kVox, -1},
        };
        const std::vector<CommandSignature> phase3Extra{
            {cmd::kLevel, level::kRf, -1},
            {cmd::kLevel, level::kMicGain, -1},
            {cmd::kLevel, level::kMonitor, -1},
            {cmd::kLevel, level::kVoxGain, -1},
            {cmd::kLevel, level::kNotchPos, -1},
            {cmd::kLevel, level::kNrLevel, -1},
            {cmd::kLevel, level::kNbLevel, -1},
            {cmd::kLevel, level::kRfPower, -1},
            {cmd::kFunction, func::kPreamp, -1},
            {cmd::kFunction, func::kAgc, -1},
            {cmd::kAttenuator, -1, -1},
            {cmd::kControl, control::kTuner, -1},
            {cmd::kTuneOffset, tuneOffset::kFrequency, -1},
            {cmd::kTuneOffset, tuneOffset::kRitOnOff, -1},
            {cmd::kTuneOffset, tuneOffset::kXitOnOff, -1},
        };
        const std::vector<int> setItems = addr == 0xA4
            ? std::vector<int>{118, 119, 116, 117}
            : std::vector<int>{84, 85, 81, 82, 83};

        const auto checkPhase = [&](int previousPhase,
                                    std::vector<CommandSignature> expected,
                                    const char* description) {
            c.radio.clearCivLog();
            IcomCivBackendTestAccess::runControlPhase(c.backend, previousPhase);
            const bool drained = waitFor([&] {
                return IcomCivBackendTestAccess::pumpUntilIdle(c.backend);
            });
            std::vector<CommandSignature> actual;
            for (const CivFrame& frame : c.radio.civCommands()) {
                actual.push_back(signature(frame));
            }
            check(drained && actual == expected, description);
        };

        checkPhase(0, basePolls,
                   "continuous model: phase-1 poll sequence and count are unchanged");
        std::vector<CommandSignature> phase2 = basePolls;
        phase2.insert(phase2.end(), phase2Extra.begin(), phase2Extra.end());
        checkPhase(1, phase2,
                   "continuous model: phase-2 poll sequence and count are unchanged");
        std::vector<CommandSignature> phase3 = basePolls;
        phase3.insert(phase3.end(), phase3Extra.begin(), phase3Extra.end());
        checkPhase(2, phase3,
                   "continuous model: phase-3 poll sequence and count are unchanged");
        std::vector<CommandSignature> phase12 = basePolls;
        phase12.insert(phase12.end(), phase2Extra.begin(), phase2Extra.end());
        phase12.insert(phase12.end(), phase3Extra.begin(), phase3Extra.end());
        for (int item : setItems) {
            phase12.push_back({cmd::kSetting, 0x05, item});
        }
        checkPhase(11, phase12,
                   "continuous model: phase-12 poll sequence, SET items, and count are unchanged");


        const auto setFrequencyCount = [&c] {
            return static_cast<int>(std::ranges::count_if(
                c.radio.civCommands(),
                [](const CivFrame& f) { return f.cmd == cmd::kSetFreq; }));
        };
        const int before = setFrequencyCount();
        const int warningsBefore = static_cast<int>(c.warnings.size());
        c.backend.setSliceFrequency(0, 500'000'000.0);
        check(waitFor([&] {
                  (void)IcomCivBackendTestAccess::pumpUntilIdle(c.backend);
                  return setFrequencyCount() > before;
              }),
              "continuous model: a frequency outside its own declared range is "
              "still sent unchanged — the gate did not leak");
        check(static_cast<int>(c.warnings.size()) == warningsBefore,
              "continuous model: and it raises no band warning");
    }

    // ---- A DELIBERATELY WRONG TYPED ADDRESS MUST STILL FAIL ----------------
    //
    // The other half of the A/B, and the half that makes the first half mean
    // anything. If auto-detect quietly rescued a wrong entry too, the test
    // above would pass on a backend that simply ignored the operator — and an
    // operator who types an address on a shared bus is SELECTING A DEVICE.
    {
        CivCase c(0xA2, "IC-9700");
        RadioConnectRequest r = c.request();
        r.params.insert(QStringLiteral("icom.civAddress"), 0xA4);
        r.params.insert(QStringLiteral("icom.civAddressPinned"), true);
        c.backend.connectRadio(r);
        check(waitFor([&] { return c.backend.isConnected(); }),
              "wrong pin: the session still comes up - the RS-BA1 transport is fine");
        check(waitFor([&] { return c.sentTo(0xA4) > 1; }, 6000),
              "wrong pin: the reads go where the operator said, not where the "
              "radio is");
        check(c.frequencyMHz == 0.0,
              "wrong pin: and nothing answers - the typed address is NOT "
              "silently overridden");
    }

    // ---- A MODEL PICK IS A SHORTCUT, AND THE WIRE MAY CORRECT IT -----------
    //
    // Same wrong address, expressed the other way: picked from the list rather
    // than typed. The operator picked an IC-705 and is in front of an IC-9700,
    // or picked correctly and later changed the address on the radio. Either
    // way the radio is authoritative about itself, and a pick that is merely a
    // stale shortcut must not cost them the session.
    {
        CivCase c(0xA2, "IC-9700");
        RadioConnectRequest r = c.request();
        r.params.insert(QStringLiteral("icom.civAddress"), 0xA4);   // NOT pinned
        c.backend.connectRadio(r);
        check(waitFor([&] { return c.backend.isConnected(); }),
              "model pick: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "model pick: the broadcast reply retargets the session and the "
              "radio answers");
        check(c.sentTo(0xA2) > 1, "model pick: the burst is re-issued at 0xA2");
    }

    // ---- A NAMED MODEL WHOSE ADDRESS WAS CHANGED ON THE RADIO --------------
    //
    // The case auto-detect exists for, and the one every case above misses: the
    // handshake name RESOLVES - so the session is seeded to 0xA2 and the connect
    // edge addresses everything there - but the radio actually answers on 0x50,
    // because someone changed it on the front panel. The cases above either
    // resolve by name to the right address (no retarget) or resolve by neither
    // (no model), so nothing yet exercises a live model on a moved address.
    //
    // The reads recovering is NOT enough to call this fixed. A retarget moves
    // the destination, and everything already sent went to a device that is not
    // there; asserting on frequency alone passes while the panadapter stays
    // black for the rest of the session.
    {
        CivCase c(0x50, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "retarget: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }),
              "retarget: the broadcast reply moves the reads to 0x50 and the "
              "radio answers");
        check(c.sentTo(0xA2) == 0,
              "retarget: the broadcast identity probe leads the scheduler, so "
              "the queued snapshot for the stale name-derived address is "
              "discarded before it reaches the wire");

        // THE DISCRIMINATING ASSERTION. 0x27 is the scope pair and an IC-9700
        // has a scope, so if "started" is latched per SESSION those two frames
        // exist only at 0xA2: the radio is never told to send sweeps, while
        // capabilities() goes on advertising a panadapter that cannot fill.
        // Frequency and mode all arrive, so the session reads as healthy.
        check(waitFor([&] { return c.sentCmdTo(0x50, cmd::kScope) > 0; }),
              "retarget: the scope switches are re-sent at 0x50 - 'started' is "
              "per DESTINATION, and a retarget is a new destination");
    }

    // ---- AN UNKNOWN NAME IN FRONT OF A MODEL THE TABLE DOES KNOW -----------
    //
    // The RS-BA1 shape: the handshake names the SERVER rather than the radio, so
    // no seed is possible and the burst waits on the broadcast. But the address
    // that comes back is in kModels - 0xB6 is the IC-7300MK2 - so the model IS
    // resolvable, and the burst has to be built against THAT model rather than
    // the conservative fallback the unresolved name left in place.
    //
    // 0x26 is the read #4984 added so a radio left in USB-D is not adopted as
    // plain USB and pushed the rest of the way out of DATA by our first write.
    // It is gated on hasVfoModeCommand, which only the resolved model sets - so
    // a burst re-issued before resolution silently drops it, on the one path
    // where the radio most needs it.
    {
        CivCase c(0xB6, "IC-7760");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "late model: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }, 6000),
              "late model: broadcast alone finds 0xB6 and the reads land");
        check(waitFor([&] { return c.sentCmdTo(0xB6, cmd::kVfoMode) > 0; }, 6000),
              "late model: the DATA-mode read goes out too - the burst is built "
              "against the model the ADDRESS resolved, not the fallback the "
              "unrecognised name left behind");
        check(c.backend.capabilities().hasTransmitFrequencyCheck,
              "late IC-7300MK2 identity enables XFC from its attested profile");
        const auto sawProfiledRepeaterRead = [&](std::uint8_t command,
                                                 std::optional<std::uint8_t> sub) {
            return std::any_of(c.radio.civCommands().begin(), c.radio.civCommands().end(),
                               [=](const CivFrame& frame) {
                return frame.cmd == command
                    && (!sub || (frame.hasSub && frame.sub == *sub))
                    && frame.data.empty();
            });
        };
        check(waitFor([&] {
                  return sawProfiledRepeaterRead(cmd::kDuplex, std::nullopt)
                      && sawProfiledRepeaterRead(cmd::kReadRepeaterOffset, std::nullopt)
                      && sawProfiledRepeaterRead(cmd::kFunction, func::kRepeaterTone)
                      && sawProfiledRepeaterRead(cmd::kTone, 0x00)
                      && sawProfiledRepeaterRead(cmd::kControl, control::kXfc);
              }, 6000),
              "late IC-7300MK2 identity enables the basic repeater/XFC read burst "
              "from model-profile evidence");
    }

    // ---- A MODEL NOT IN THE TABLE ------------------------------------------
    //
    // No name seed is possible, so the broadcast is the ONLY thing that can
    // resolve this - which is exactly the case a model chooser cannot cover and
    // auto-detect can. The IC-7760 (0xB2) is a real example: it is missing from
    // kModels today.
    {
        CivCase c(0xB2, "IC-7760");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "unknown model: the session comes up");
        check(waitFor([&] { return c.frequencyMHz > 14.0 && c.frequencyMHz < 14.1; }, 6000),
              "unknown model: broadcast alone finds 0xB2 and the reads land");
        check(!c.backend.model().isKnown(),
              "unknown model: capabilities stay on the conservative fallback - "
              "the ADDRESS was learned, the model was not");
        check(!c.backend.capabilities().hasTransmitFrequencyCheck,
              "unknown model: XFC remains dark without a profile attestation");
        const auto isRepeaterOrXfc = [](const CivFrame& frame) {
            return frame.cmd == cmd::kDuplex
                || frame.cmd == cmd::kReadRepeaterOffset
                || frame.cmd == cmd::kSetRepeaterOffset
                || (frame.cmd == cmd::kFunction && frame.hasSub
                    && (frame.sub == func::kRepeaterTone
                        || frame.sub == repeaterAccess::kFunction))
                || (frame.cmd == cmd::kTone && frame.hasSub
                    && (frame.sub == repeaterTone::kTxCtcss
                        || frame.sub == repeaterTone::kRxCtcss
                        || frame.sub == repeaterTone::kDtcs))
                || (frame.cmd == cmd::kControl && frame.hasSub
                    && (frame.sub == control::kXfc
                        || frame.sub == control::kReadTxFreq));
        };
        check(std::none_of(c.radio.civCommands().begin(), c.radio.civCommands().end(),
                           isRepeaterOrXfc),
              "unknown model: connect sends no repeater or XFC reads without a profile");
        c.radio.clearCivLog();
        c.backend.setSliceFmRepeater(0, QStringLiteral("up"), 600'000.0,
                                     QStringLiteral("ctcss_tx"), 88.5);
        c.backend.setTransmitFrequencyCheck(true);
        QCoreApplication::processEvents();
        check(std::none_of(c.radio.civCommands().begin(), c.radio.civCommands().end(),
                           isRepeaterOrXfc),
              "unknown model: repeater and XFC writes fail closed without a profile");
        check(c.warnedAbout("not a model AetherSDR has data for"),
              "unknown model: and the reduced radio is EXPLAINED rather than "
              "left looking like a half-finished backend");
    }

    // ---- A SILENT RADIO STILL CONNECTS -------------------------------------
    //
    // The bounded wait must not become a way to hang the connect. Unknown name
    // AND no CI-V answers at all: the worst case, and it has to degrade to
    // exactly today's behaviour rather than to a session that never proceeds.
    {
        CivCase c(0xA2, "IC-7760");
        c.radio.setCivSilent(true);
        QSignalSpy errorSpy(&c.backend, &IRadioBackend::connectionError);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "silent radio: the session still comes up");
        check(waitFor([&] { return c.sentTo(0xA4) > 1; }, 6000),
              "silent radio: the wait times out and the burst goes out at the "
              "fallback address rather than never going out at all");
        QTest::qWait(7000);
        check(c.backend.isConnected(),
              "silent unknown radio: CI-V stall retains main's warn-only behavior");
        check(c.radio.serialRestartTimesMs().empty(),
              "silent unknown radio: no IC-9700-specific data restart is sent");
        check(errorSpy.isEmpty(),
              "silent unknown radio: no IC-9700 recovery error is reported");
        check(std::ranges::none_of(c.radio.civCommands(), [](const CivFrame& frame) {
                  return frame.cmd == cmd::kControl && frame.hasSub
                      && frame.sub == control::kPtt && !frame.data.empty()
                      && frame.data.front() != 0;
              }),
              "silent radio: neither recovery nor teardown can key the transmitter");
    }

    // ---- TWO RESPONDERS: ADOPT NEITHER -------------------------------------
    //
    // Icom's own RS-BA1 server can front a serial CI-V bus carrying a second
    // radio, a rotator or an amplifier, and every one of them answers a
    // broadcast. Taking whichever replied first would decode the rest of the
    // session against a device the operator never chose - silently, and with a
    // plausible-looking result.
    //
    // UNMEASURED on hardware: both lab radios are single direct-LAN devices.
    // The behaviour is reasoned, not reproduced, and the test says so.
    {
        CivCase c(0xA2, "IC-7760");
        c.radio.setSecondResponder(0x98);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "two responders: the session comes up");
        check(waitFor([&] { return c.warnedAbout("More than one device"); }, 6000),
              "two responders: the ambiguity is REPORTED, not resolved by guessing");
        check(!c.backend.model().isKnown(),
              "two responders: and neither identity is adopted");

        // THE DESTINATION HAS TO REVERT AND STAY REVERTED, which the two
        // assertions above cannot see — both survive the session quietly
        // drifting back onto one of the responders.
        //
        // It really does drift without a guard: the burst issued at the adopted
        // address is already in flight and carries its own directed 0x19 0x00,
        // and that reply arrives AFTER the revert. It matches the address we
        // recorded, so it is not a third responder — and would otherwise fall
        // straight through to the retarget branch and undo the revert.
        QTest::qWait(400);
        c.radio.clearCivLog();
        QTest::qWait(600);
        check(c.sentTo(0xA2) == 0,
              "two responders: and NOTHING further is addressed to the responder "
              "that answered first - the fallback holds");
    }

    // ---- THE REPLY THAT DISAGREES WITH ITSELF --------------------------------
    //
    // The address arrives twice in one frame — the `from` byte and the payload —
    // and they agreed on every measured run on both lab radios. They are still
    // read separately, because a disagreement means something is rewriting
    // frames between the radio and us, and that is worth knowing before it gets
    // diagnosed as a wrong address. The payload is preferred: it is what the
    // command is defined to answer.
    {
        CivCase c(0xA2, "IC-9700");
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "IC-9700 XFC: the session comes up");
        check(c.backend.capabilities().hasTransmitFrequencyCheck,
              "IC-9700 advertises the verified momentary XFC capability");
        check(waitFor([&] {
                  const QVariantMap state =
                      IcomCivBackendTestAccess::repeaterState(c.backend);
                  return state.value(QStringLiteral("accessMode")).toString()
                             == QLatin1String("dtcs_txrx")
                      && std::abs(state.value(QStringLiteral("rxCtcssHz")).toDouble()
                                  - 67.0) < 0.001
                      && state.value(QStringLiteral("dtcsCode")).toInt() == 23
                      && !state.value(QStringLiteral("dtcsTxReverse")).toBool()
                      && state.value(QStringLiteral("dtcsRxReverse")).toBool()
                      && state.value(QStringLiteral("txFrequencyHz")).toULongLong()
                             == 448'425'600ULL;
              }),
              "IC-9700 adopts capability-gated access, RX CTCSS, DTCS polarity, "
              "and TX-frequency readback without changing the shared UI model");
        const QVariantList controls = IcomCivBackendTestAccess::controls(c.backend);
        const int supportedExtendedRows = static_cast<int>(std::ranges::count_if(
            controls, [](const QVariant& value) {
                const QVariantMap row = value.toMap();
                return row.value(QStringLiteral("profileFeature")).toString()
                           == QLatin1String("fm-repeater-extended-readback")
                    && row.value(QStringLiteral("supported")).toBool();
            }));
        check(supportedExtendedRows == 4,
              "IC-9700 effective control map exposes exactly four read-only "
              "extended repeater rows");
        c.radio.clearCivLog();
        c.backend.setTransmitFrequencyCheck(true);
        check(waitFor([&] { return c.radio.m_transmitFrequencyCheck; }),
              "IC-9700 accepts the shared XFC command");
        check(c.sentTo(0xA2) > 0 && c.sentTo(kIc705Addr) == 0,
              "IC-9700 XFC is addressed to 0xA2, never the IC-705 default");
        c.backend.setTransmitFrequencyCheck(false);
        check(waitFor([&] { return !c.radio.m_transmitFrequencyCheck; }),
              "IC-9700 accepts the fail-safe XFC release");
        c.backend.disconnectRadio();
    }
    {
        // DEAF, so the crafted frame below is the FIRST id reply of the session.
        // A radio that answered the broadcast normally first would make the
        // disagreeing frame a SECOND, different address — which is the
        // two-responder case above, not this one.
        CivCase c(0xA2, "IC-7760");
        c.radio.setCivSilent(true);
        c.backend.connectRadio(c.request());
        check(waitFor([&] { return c.backend.isConnected(); }),
              "disagreeing reply: the session comes up");
        // Hand-built so the two fields differ, which no real radio produced.
        c.radio.pushCiv({0xFE, 0xFE, kControllerAddress, 0xA2, cmd::kReadId, 0x00,
                         0x98, kCivEom});
        check(waitFor([&] { return c.sentTo(0x98) > 0; }, 6000),
              "disagreeing reply: the PAYLOAD wins over the frame's from byte");
        check(c.sentTo(0xA2) == 0,
              "disagreeing reply: and the from byte is NOT what gets addressed");
    }

    if (g_failures == 0)
        std::printf("icom_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
