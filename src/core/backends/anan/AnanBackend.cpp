#include "core/backends/anan/AnanBackend.h"

#include <QHostAddress>
#include <QMetaObject>
#include <QTimer>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace AetherSDR::anan {

const QString AnanBackend::kPanId = QStringLiteral("anan-0");

namespace {

// Placeholder until a bench measurement exists -- see the class comment.
// Named and zero rather than silently absent, so the next reader finds a
// TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
           static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

WdspChannel::Mode AnanBackend::modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    // "CW" is the spelling TciProtocol::tciToSmartSDR produces and the one a
    // Flex reports -- HERMES.md §16.7's own regression was this falling
    // through to the USB fallback because only "CWU" was mapped.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("CWL")) return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    // WDSP has no dedicated RTTY demod (WdspChannel::Mode has no Rtty member --
    // RTTY is FSK decoded by an external app from a wide IQ passband, the same
    // arrangement DIGU already gives every other digital mode here). Found by
    // `radiocert tune`'s mode-map stage: RTTY silently fell through to the
    // USB fallback below -- readback still said "RTTY" (the slice keeps
    // whatever string it's given), so the passband LOOKED digital-shaped by
    // coincidence (this function's own USB fallback and DIGU's passband
    // aren't the same numbers) while the demod underneath was actually USB.
    // Exactly the HERMES.md 15.7/16.7 shape this file already cites for
    // CW/NFM -- same bug, a mode neither of those fixes happened to cover.
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;   // unknown mode: same fallback as Hl2Backend's
}

std::pair<int, int> AnanBackend::defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    // Same window as DIGU -- see modeFromString()'s own comment on why RTTY
    // maps to Digu -- made explicit rather than landing here only because it
    // happens to equal this function's own unknown-mode fallback below.
    if (u == QLatin1String("RTTY")) return {150, 3000};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {-250, 250};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

double AnanBackend::cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return static_cast<double>(pitchHz);
    if (u == QLatin1String("CWL"))
        return -static_cast<double>(pitchHz);
    return 0.0;
}

AnanBackend::AnanBackend(QObject* parent) : IRadioBackend(parent)
{
    m_client = new P2Client(nullptr);   // nullptr parent: moveToThread requires it
    m_dsp = new AnanRxDsp(nullptr);

    // Leading+trailing throttle for setSliceFrequency()'s expensive side
    // effects -- see scheduleTuneApply()'s comment. Lives on this object
    // (the GUI thread), same as the backend itself; not moved to m_ioThread.
    m_tuneThrottleTimer = new QTimer(this);
    m_tuneThrottleTimer->setSingleShot(true);
    connect(m_tuneThrottleTimer, &QTimer::timeout, this, [this] {
        if (m_tunePendingApply) {
            m_tunePendingApply = false;
            applyTuneToRadioAndPan();
            m_tuneThrottleTimer->start(kTuneThrottleMs);   // cooldown for the trailing apply too
        }
        // else: nothing arrived during the cooldown -- let the timer sit
        // idle until the next scheduleTuneApply() restarts it.
    });

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("anan-io"));
    m_client->moveToThread(m_ioThread);
    m_dsp->moveToThread(m_ioThread);
    m_ioThread->start();

    // Build-only thread for a rate change's background DSP rebuild -- see
    // the member declaration comment. m_dspBuildContext owns no state; it
    // exists purely so QMetaObject::invokeMethod has a thread-affinity
    // target to hop the (slow) AnanRxDsp::buildChannel() call onto.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    m_dspBuildThread->start();

    // Both live on the I/O thread -- a same-thread call either way, but
    // explicit to document intent and match Hl2Backend's own explicit
    // DirectConnection on the equivalent wire-to-DSP wiring: no queue, no
    // event-loop hop for the sample path.
    connect(m_client, &P2Client::ddc0IqReady, m_dsp, &AnanRxDsp::processIqBlock,
            Qt::DirectConnection);

    // Everything below crosses from the I/O thread to whichever thread this
    // object lives on -- plain AutoConnection resolves to Queued, which is
    // why AnanRxDsp's constructor registers std::vector<float> as a Qt
    // metatype (P2Client's ddc0IqReady never needs that: it is only ever
    // DirectConnection'd, above).
    connect(m_client, &P2Client::linkUp, this, [this] {
        m_connected = true;
        // See m_rateChanging's declaration comment: a live rate change is a
        // real stop+restart of this session, but the operator only asked to
        // zoom, so connected() is suppressed for that one round trip.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        if (!wasRateChange)
            emit connected();
        emitSliceState();
        emitPanState();
        // Real limits, not a guess -- HERMES.md §15.1: without this, the GUI
        // clamps the zoom control against a FlexLib model-name table that
        // falls through to 5.4 MHz for "ANAN-G2" (which it does not
        // recognise), so the operator could zoom 3.5x past this radio's
        // real 1.536 MHz ceiling and see black bars over spectrum that was
        // never sampled -- the exact defect class documented there for the
        // HL2 before it emitted this. Constant for the life of a Step 1b
        // connection (one DDC, no multi-receiver bandwidth budget to share
        // the way HL2's does), so emitting it once here -- on every linkUp,
        // rate change included, cheap and simpler than gating on
        // wasRateChange for values that never actually change -- is enough;
        // unlike Hl2Backend's own two call sites, there is no second place
        // this needs to be re-derived from. Bounds match
        // capabilities().sampleRatesHz's own endpoints (48-1536 ksps).
        emit panBandwidthLimitsChanged(kPanId, 48'000.0 / 1.0e6, 1'536'000.0 / 1.0e6);
        if (wasRateChange) {
            // Audio was muted in beginRateChange(), BEFORE this session's
            // session even started -- see that function's comment for why
            // reactively muting here, after the fact, was one block too
            // late. Unmute after a short settle window instead of
            // immediately: a rate change rebuilds WdspChannel from scratch,
            // and its AGC/filters/DC-blocker all start from zero state
            // against already-live RF, not silence, so a beat of quiet lets
            // that settle before sound resumes. Spectrum/waterfall are
            // unaffected by any of this (AnanRxDsp::setAudioMuted() only
            // zeroes the audio-path input, per its own header comment), so
            // the display keeps updating live throughout.
            const quint64 generation = m_connectGeneration;
            QTimer::singleShot(kRateChangeAudioSettleMs, this, [this, generation] {
                // Superseded by a newer connect/rate-change/disconnect --
                // that cycle's own beginRateChange()/disconnectRadio() owns
                // muting from here, not this timer.
                if (generation != m_connectGeneration || !m_dsp)
                    return;
                QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                          Q_ARG(bool, false));
            });
            retryPendingRateChange();
        }
    });
    connect(m_client, &P2Client::linkDown, this, [this] {
        m_connected = false;
        if (!m_rateChanging)
            emit disconnected();
    });
    connect(m_client, &P2Client::connectionError, this,
            [this](const QString& reason) { emit connectionError(reason); });
    connect(m_client, &P2Client::dropsUpdated, this, [](quint64) {
        // No LinkStats wiring in this phase -- see the design plan's
        // "explicitly not in this commit" list. Connected so the signal has
        // a receiver rather than going nowhere; a future commit can surface
        // it through IRadioBackend::linkStats().
    });
    connect(m_client, &P2Client::discoveryInfoReceived, this,
            [this](quint8 boardId, quint8 firmwareVer, quint8 numDdc) {
        // See capabilities()'s own comment for where these surface. Reported
        // real, not hardcoded -- the working plan's Step 2 exit item this
        // closes -- but this phase still only ever DRIVES one DDC regardless
        // of what numDdc says, so nothing here may touch maxSlices/
        // maxPanadapters; that stays a Phase 1b scope decision, not a
        // capability the radio gets to raise on our behalf.
        m_discoveredBoardId = boardId;
        m_discoveredFirmwareVer = firmwareVer;
        m_discoveredNumDdc = numDdc;
        m_discoveryInfoReceived = true;
        emit capabilitiesChanged();
    });

    connect(m_dsp, &AnanRxDsp::audioReady, this, [this](const std::vector<float>& pcm) {
        const QByteArray bytes = floatBytes(pcm);
        emit sliceAudioFrameReady(kSliceId, bytes);
        // One DDC, so "mixing" the speaker feed is the identity -- no
        // separate mix stage needed for a single receiver.
        emit audioFrameReady(bytes);
    });
    connect(m_dsp, &AnanRxDsp::spectrumReady, this, [this](const std::vector<float>& binsDbfs) {
        std::vector<float> dbm(binsDbfs.size());
        for (std::size_t i = 0; i < binsDbfs.size(); ++i)
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset;
        emit spectrumFrameReady(kSliceId, floatBytes(dbm));
    });
    // Deliberately do not publish AnanRxDsp::meterUpdate yet. It is WDSP
    // SignalPeak in uncalibrated dBFS; registering it as SLC:LEVEL would feed
    // the shared S-meter, whose scale and S-unit labels require real dBm.
}

AnanBackend::~AnanBackend()
{
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup/rebuild callback

    // Join the build thread BEFORE touching m_dsp/m_client, not after.
    // AnanRxDsp::buildChannel() itself (the slow part, up to ~60s cold FFTW
    // planning) never touches them -- but the SAME lambda's tail, which runs
    // right after that slow call returns, posts follow-up work via
    // QMetaObject::invokeMethod(m_dsp, ...) and
    // QMetaObject::invokeMethod(this, ...). Deleting m_dsp/m_client first
    // left a window where that still-running lambda could dereference an
    // already-freed m_dsp the moment its slow work finally returned, if the
    // app closed mid-rate-change or mid-first-connect. quit()/wait() here
    // blocks for whatever's left of an in-flight build -- a bounded, rare
    // app-close delay, deliberately traded for correctness over instant
    // close.
    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }
    delete m_dspBuildContext;

    if (m_ioThread) {
        if (m_client)
            QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    }
    delete m_dsp;
    delete m_client;
}

RadioCapabilities AnanBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("anan");
    c.manufacturer = QStringLiteral("Apache Labs");
    c.model = QStringLiteral("ANAN-G2");
    c.maxSlices = 1;
    c.maxPanadapters = 1;
    c.sampleRatesHz = {48000, 96000, 192000, 384000, 768000, 1536000};
    // Not reported -- no verified G2 tuning range (RFC: "I have not fetched
    // the Apache Labs G2 manual"). RadioCapabilities.h's own convention:
    // both zero means "not reported", not a guess.
    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    c.canTransmit = false;         // P2Client has no PTT capability -- see class comment
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;        // client-side WDSP, like the HL2
    c.takesTxAudioOverSeam = true; // moot while canTransmit is false
    c.hasTuner = false;            // G2 has no internal ATU (Apache Labs spec)
    c.hasTunerMemories = false;    // no internal ATU, so no tuner-memory surface
    c.hasAmplifier = false;
    c.hasRadioSideDsp = false;     // DSP is engine-side (AnanRxDsp), not firmware
    c.radioOwnsDbmScale = false;   // client computes it from raw IQ
    c.hasDdcPanEdgeRolloff = true; // see RadioCapabilities.h's own comment
    c.persistsMemories = false;    // default; stated explicitly
    c.clientSettingsDomains = {};  // no applyRestoredState()/currentOperatingState() yet
    c.extensionNamespaces = {};    // no "anan" extension VERBS yet -- see below,
                                    // this is about invokeExtension(), not this map
    // Genuinely discovered, not hardcoded (working plan Step 2's "Capabilities
    // from discovery" item) -- P2Client::discoveryInfoReceived() parses THIS
    // session's own Discovery reply opportunistically as it arrives (P2Client's
    // class comment). Empty/zero until that reply lands, same "not reported yet"
    // convention RadioCapabilities.h already uses for tuningMinHz/MaxHz -- a
    // caller reading this before connect (or in the brief window right after)
    // gets an honest "don't know yet", not a guess. capabilitiesChanged() fires
    // when it does land, for anything that wants to re-read rather than poll.
    //
    // numDdc is informational only in this phase -- maxSlices/maxPanadapters
    // above stay fixed at 1 regardless of what the radio reports, because this
    // backend only ever drives DDC0 (Step 3, not started, is where a real
    // multi-DDC count would apply).
    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("numDdc")] = m_discoveredNumDdc;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        c.extensions[QStringLiteral("anan")] = anan;
    }
    return c;
}

void AnanBackend::connectRadio(const RadioConnectRequest& request)
{
    const QHostAddress hostAddr(request.host);
    if (hostAddr.isNull()) {
        emit connectionError(QStringLiteral("ANAN: invalid host '%1'").arg(request.host));
        return;
    }
    if (m_connected)
        disconnectRadio();

    // A fresh connect (possibly to a different host) must not keep reporting
    // a prior radio's discovered identity until its own reply lands -- see
    // capabilities()'s own comment.
    m_discoveryInfoReceived = false;
    m_discoveredBoardId = 0;
    m_discoveredFirmwareVer = 0;
    m_discoveredNumDdc = 0;

    m_pendingParams.host = request.host;
    m_pendingParams.ddc0RateKsps =
        request.params.value(QStringLiteral("anan.ddc0RateKsps"), 48).toInt();
    // Connect-time-only ADC options -- see P2Client::Params' own comment
    // for why none of these have a live setter. Defaults match
    // AnanSettings' own defaults, so a caller that never populated these
    // params (a picker/auto-reconnect connect, same reasoning as
    // ddc0RateKsps above) still gets sane values, not zeroed-out ones.
    m_pendingParams.ditherEnabled =
        request.params.value(QStringLiteral("anan.ditherEnabled"), true).toBool();
    m_pendingParams.randomEnabled =
        request.params.value(QStringLiteral("anan.randomEnabled"), true).toBool();
    m_pendingParams.ddc0AdcIndex =
        request.params.value(QStringLiteral("anan.ddc0AdcIndex"), 0).toInt() == 1 ? 1 : 0;
    m_pendingParams.bypassAdc0Filters =
        request.params.value(QStringLiteral("anan.bypassAdc0Filters"), true).toBool();
    m_pendingParams.bypassAdc1Filters =
        request.params.value(QStringLiteral("anan.bypassAdc1Filters"), true).toBool();

    // The frequency this session comes up on. Mirrors Hl2Backend's own
    // startFreqHz fallback (10 MHz -- WWV, a live signal on any HF antenna,
    // useful for exactly this kind of first-connect bring-up test) minus the
    // restored-state branch: capabilities().clientSettingsDomains is empty
    // for this backend (Phase 1b persists nothing), so applyRestoredState()
    // is never called and there is no prior session to prefer. Without this,
    // m_sliceFreqHz stays at its 0.0 member default, finishDspSetup()'s
    // `m_sliceFreqHz > 0.0` guard never fires, and DDC0 stays parked at the
    // phase word P2Client::start() sends by default (0 -- baseband/DC).
    m_sliceFreqHz = request.params.contains(QStringLiteral("anan.rxFrequencyHz"))
        ? request.params.value(QStringLiteral("anan.rxFrequencyHz")).toDouble()
        : 10'000'000.0;

    m_pendingDspConfig = AnanRxDsp::Config{};
    m_pendingDspConfig.inputSampleRateHz = m_pendingParams.ddc0RateKsps * 1000;
    m_pendingDspConfig.audioSampleRateHz = 24000;
    m_pendingDspConfig.dspBlockSize = 1024;
    m_pendingDspConfig.fftSize = 1024;
    m_pendingDspConfig.mode = modeFromString(m_mode);
    m_pendingDspConfig.filterLowHz = static_cast<double>(m_filterLowHz) + cwBfoHz();
    m_pendingDspConfig.filterHighHz = static_cast<double>(m_filterHighHz) + cwBfoHz();
    m_pendingDspConfig.agcMode = 3;
    // 60 dB, not Hl2RxDsp::Config's 39 dB (= slice default 65 * 0.6): bench
    // testing against the real G2 found audio too quiet at the HL2-matched
    // default even with the operator's own AGC ceiling slider and this
    // app's output volume both already at their own maximums, only becoming
    // comfortable once the slider was ALSO pushed to 100 (= 60 dB via
    // setSliceAgc()'s same *0.6 mapping) -- this backend's own uncalibrated
    // signal-chain gain (kUncalibratedDbfsToDbmOffset, see its own comment)
    // evidently sits lower than the HL2's. AGC only ever applies UP TO this
    // ceiling on weak signals; it backs off on its own for strong ones, so
    // raising the default does not risk clipping a loud signal the way a
    // fixed gain increase would.
    m_pendingDspConfig.maximumAgcGainDb = 60.0;

    ++m_connectGeneration;
    beginDspSetup();
}

void AnanBackend::beginDspSetup()
{
    // Same background-build pattern beginRateChange() uses (see its own
    // comment): AnanRxDsp::buildChannel() can take ~19s cold (FFTW PATIENT
    // planning). Running configure() synchronously on m_ioThread -- where
    // m_client also lives -- meant a disconnectRadio()/close reached during
    // that window blocked on its BlockingQueuedConnection to m_client,
    // which cannot service the stop() request until the queued configure()
    // call already in the same thread's event queue finishes first.
    // Building on the dedicated m_dspBuildThread instead keeps m_ioThread
    // free the entire time, so a disconnect or close during initial setup
    // can proceed immediately, matching beginRateChange()'s own reasoning.
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    // Seed the requested startup state before the off-thread build. The
    // install path reapplies the object's current state so operator edits made
    // during a build win; without this first-connect seed it would reapply the
    // Config member defaults over cfg instead.
    QMetaObject::invokeMethod(m_dsp, [this, generation, cfg]() {
        m_dsp->beginInitialBuild(cfg);

        QMetaObject::invokeMethod(m_dspBuildContext, [this, generation, cfg]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP configure failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(m_dsp, [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, generation, ok, errStr]() {
                finishDspSetup(generation, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishDspSetup(quint64 generation, bool ok, const QString& error)
{
    // A newer connectRadio() or a disconnectRadio() happened while
    // configure() was running on the I/O thread -- this result is stale.
    if (generation != m_connectGeneration)
        return;

    if (!ok) {
        emit connectionError(QStringLiteral("ANAN: DSP configure failed: %1")
                             .arg(error.isEmpty() ? QStringLiteral("unknown error") : error));
        // Only ever reached from connectRadio()'s first-connect path now --
        // beginRateChange() builds the new channel off-thread and never
        // calls beginDspSetup()/finishDspSetup() -- so m_rateChanging is
        // always false here. Kept for symmetry with
        // startP2ClientSession()'s own failure handling rather than because
        // this branch can currently observe a rate change in flight.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        emitPanState();
        if (wasRateChange)
            retryPendingRateChange();
        return;
    }

    startP2ClientSession(generation);
}

void AnanBackend::startP2ClientSession(quint64 generation)
{
    const P2Client::Params params = m_pendingParams;
    const bool isRateChange = m_rateChanging;
    QMetaObject::invokeMethod(m_client, [this, generation, params, isRateChange]() {
        const bool started = isRateChange
            ? m_client->start(params, kRateChangeConnectTimeoutMs)
            : m_client->start(params);
        if (!started) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (generation != m_connectGeneration)
                    return;
                emit connectionError(QStringLiteral("ANAN: could not open the UDP socket"));
                if (m_pendingBandwidthKsps != 0) {
                    // Same retry-window fix as finishRateChange()'s failure
                    // branch (see its comment) -- a socket-open failure
                    // right after a background rebuild can hit the exact
                    // same hazard: clearing m_rateChanging here, before the
                    // queued zoom retries, would let this failed attempt's
                    // own teardown emit an unsuppressed connected()/
                    // disconnected() pair before the retry re-arms it.
                    retryPendingRateChange();
                } else {
                    m_rateChanging = false;
                    emitPanState();
                }
            }, Qt::QueuedConnection);
            return;
        }
        // Push the operator's current tune once the session is up.
        // linkUp() (P2Client's own signal, connected in the constructor)
        // is what actually fires connected()/emitSliceState()/emitPanState()
        // once the first genuine DDC0 frame arrives -- not this call.
        if (m_sliceFreqHz > 0.0)
            m_client->setDdc0FrequencyHz(m_sliceFreqHz);
    }, Qt::QueuedConnection);
}

void AnanBackend::disconnectRadio()
{
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup callback
    // Blocking, not fire-and-forget: matches ~AnanBackend()'s own stop() call
    // and every other worker-thread stop closeEvent() performs (dxCluster,
    // rbnClient, ...) for the same reason -- a disconnect reached while
    // shutting down must not return to a caller that may go on to tear down
    // GUI objects (or this backend itself) before the I/O thread has
    // actually drained its queue and closed the socket. The prior
    // fire-and-forget QueuedConnection left a window where MainWindow::
    // closeEvent() (via the X button, which — unlike File > Quit — runs this
    // synchronously while the event loop is still pumping) could proceed to
    // destroy the radio session out from under an I/O-thread call still in
    // flight. disconnected() still fires later via linkDown()'s own queued
    // round trip, unchanged -- only the stop() call itself is now waited on.
    if (m_client)
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    m_pendingBandwidthKsps = 0;
    // A genuine operator-initiated disconnect always fires disconnected(),
    // even one that lands mid-rate-change -- this is not the zoom case
    // m_rateChanging exists to hide.
    m_rateChanging = false;
    // m_audioMuted lives on AnanRxDsp and survives configure() (see that
    // function's own comment), so a disconnect landing mid-rate-change --
    // while audio was deliberately muted for the settle window -- would
    // otherwise leave the NEXT session starting muted with nothing left to
    // ever clear it.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, false));
    // linkDown() (constructor-wired) sets m_connected = false and emits
    // disconnected() once P2Client::stop() actually runs.
}

void AnanBackend::pushModeFilterShift()
{
    if (!m_dsp)
        return;
    const double bfo = cwBfoHz();
    QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
        Q_ARG(WdspChannel::Mode, modeFromString(m_mode)));
    QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
        Q_ARG(double, static_cast<double>(m_filterLowHz) + bfo),
        Q_ARG(double, static_cast<double>(m_filterHighHz) + bfo));
    // No NCO-vs-slice offset in this backend (see the class comment) --
    // the shift is exactly the CW BFO, negated (HERMES §5: the shift names
    // the RF frequency the detector treats as zero, so pushing that zero
    // DOWN a pitch is what lifts the marker UP onto it).
    QMetaObject::invokeMethod(m_dsp, "setShift", Qt::QueuedConnection, Q_ARG(double, -bfo));
}

void AnanBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    m_sliceFreqHz = hz;
    // Digit readout / slice model: unthrottled. Cheap (no radio round trip,
    // no pan re-layout), and it is what makes a drag gesture feel like it is
    // tracking the mouse at all.
    emitSliceState();
    // Everything that touches the radio or the pan's display geometry goes
    // through the coalescing throttle instead of firing here directly -- see
    // applyTuneToRadioAndPan()'s comment for why: a click/drag-tune gesture
    // calls this once per mouse-move event (tens of times a second), and
    // Phase 1b has no NCO-vs-slice decoupling (the class comment is explicit:
    // "the shift is ALWAYS exactly -cwBfoHz... because the NCO IS the slice
    // frequency, always") -- so unthrottled, EVERY one of those events would
    // retune the actual DDC0 hardware and re-broadcast the pan's geometry.
    scheduleTuneApply();
    // No operatingStateChanged() emit: that signal is documented as "emitted
    // only by backends with a non-empty clientSettingsDomains declaration"
    // (IRadioBackend.h), and this backend's is empty in this phase.
}

void AnanBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);
    // Idempotence: only reset the passband to the mode's default when the
    // mode actually changes, so a repeated set does not clobber an
    // operator's manual filter edit.
    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    m_mode = mode;
    pushModeFilterShift();   // HERMES §16.7: mode changes re-push the passband, every time
    emitSliceState();
}

void AnanBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Q_UNUSED(sliceId);
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_dsp) {
        const double bfo = cwBfoHz();
        QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(lowHz) + bfo),
            Q_ARG(double, static_cast<double>(highHz) + bfo));
    }
    emitSliceState();
}

void AnanBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(sliceId);
    // 0..100 operator units -> 0..60 dB ceiling, same map Hl2Backend uses
    // (kAgcCeilingDbPerUnit = 0.6) -- a WDSP-range fact, not an HL2 fact.
    const QString m = mode.trimmed().toLower();
    int wdspMode = 3;   // medium, WDSP's own default
    if (m == QLatin1String("off"))  wdspMode = 0;
    else if (m == QLatin1String("slow")) wdspMode = 2;
    else if (m == QLatin1String("fast")) wdspMode = 4;
    const double ceilingDb = static_cast<double>(thresholdDb) * 0.6;
    // Live state for beginRateChange() to refresh m_pendingDspConfig from --
    // see the member declaration comment.
    m_agcMode = wdspMode;
    m_agcCeilingDb = ceilingDb;
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspMode), Q_ARG(double, ceilingDb));
    }
    emitSliceState();
}

void AnanBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);   // one pan in this phase
    Q_UNUSED(intent);  // ignored, matching Hl2Backend::setPanCenter
    setSliceFrequency(kSliceId, hz);
}

int AnanBackend::nearestDdc0RateKsps(int requestedKsps) noexcept
{
    // capabilities().sampleRatesHz, in ksps. DDC0 runs at exactly one of
    // these -- there is no continuous zoom on this radio.
    //
    // RATIO distance (log-domain), not linear -- HERMES.md §15.1 (the HL2's
    // own version of this exact function, Hl2Backend::nearestIqSampleRateHz,
    // is what this mirrors): these six rates are octave-spaced and zoom is
    // multiplicative, so linear "nearest" is provably wrong for a request
    // between the geometric and arithmetic mean of two adjacent rates --
    // e.g. between 96 and 192 ksps the geometric mean is ~135.8 but the
    // arithmetic mean is 144, so a 140 ksps request belongs to 192 by ratio
    // and to 96 by linear distance. A previous version of this function used
    // linear distance and needed an explicit tie-break for the one case that
    // bit it on the bench: 384*1.5 = 576 sits EXACTLY halfway between 384
    // and 768 in linear terms, an easy integer for a real zoom gesture to
    // land on, and ties resolved toward the lower/current rate no matter
    // which way the operator was zooming. That exact tie does not recur
    // under ratio distance -- the equivalent equidistant point is
    // 96*sqrt(2) =~ 135.76 ksps, not an integer any real zoom request lands
    // on -- so no tie-break is needed here, matching the HL2 version exactly.
    static constexpr std::array<int, 6> kRatesKsps = {48, 96, 192, 384, 768, 1536};
    if (requestedKsps <= 0)
        return kRatesKsps.front();
    int best = kRatesKsps.front();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int r : kRatesKsps) {
        const double distance = std::abs(std::log(static_cast<double>(requestedKsps) / r));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = r;
        }
    }
    return best;
}

void AnanBackend::setPanBandwidth(const QString& panId, double hz)
{
    Q_UNUSED(panId);   // one pan in this phase
    if (hz <= 0.0 || !m_connected)
        return;

    const int requestedKsps = static_cast<int>(hz / 1000.0 + 0.5);
    const int snappedKsps = nearestDdc0RateKsps(requestedKsps);

    if (snappedKsps == m_pendingParams.ddc0RateKsps) {
        // Already at the closest rate this radio can do -- republish rather
        // than sit silent, so an optimistic wider/narrower span the GUI
        // applied locally snaps back to what the data will actually be.
        // RadioModel's own handler does this same "unchanged -> republish"
        // dance for exactly this reason (see PanadapterModel::
        // republishCenterBandwidth()'s comment, #4470).
        //
        // Also clears any pending request left over from an earlier zoom
        // in this same in-flight cycle -- m_rateChanging's own comment
        // states the invariant "a newer one supersedes an older," but
        // landing back on the currently-in-flight (or just-finished)
        // target rate took this early-exit branch instead of the one
        // below that updates m_pendingBandwidthKsps, so a DIFFERENT
        // stale request queued moments earlier would otherwise survive
        // and fire once this cycle finished -- overriding the operator's
        // actual latest intent with an older one.
        m_pendingBandwidthKsps = 0;
        emitPanState();
        return;
    }

    if (!m_rateChanging) {
        beginRateChange(snappedKsps);
    } else {
        // A reconfigure is already running -- remember the latest request
        // rather than starting a second one on top of it. See m_rateChanging's
        // own comment for why this waits for that cycle to actually finish
        // rather than a fixed cooldown.
        m_pendingBandwidthKsps = snappedKsps;
    }
}

void AnanBackend::retryPendingRateChange()
{
    if (m_pendingBandwidthKsps == 0)
        return;
    const int ksps = m_pendingBandwidthKsps;
    m_pendingBandwidthKsps = 0;
    beginRateChange(ksps);
}

void AnanBackend::beginRateChange(int newRateKsps)
{
    // A live rate change is a clean stop + reconfigure + restart of the
    // P2Client session, NOT an in-place DDC-Specific resend while streaming
    // -- P2Client::Params::ddc0RateKsps is a connect-time-only parameter
    // with no live setter (see its own comment). Whether the radio would
    // accept a live rate change without a session restart at all is a
    // separate, unverified protocol question, not attempted here.
    //
    // The SLOW part -- AnanRxDsp rebuilding WdspChannel from scratch, up to
    // ~a minute cold for a block size never used before in this process
    // (WdspChannel.cpp) -- now runs on a dedicated background thread
    // (m_dspBuildThread), BEFORE anything about the live session is
    // touched. The OLD channel and the OLD P2Client session both keep
    // running, completely undisturbed -- audio, spectrum, and the radio's
    // own C&C keepalive all continue exactly as before -- for the entire
    // build. Only once the new channel is actually ready does
    // finishRateChange() run the mute/stop/restart sequence, fed a
    // pre-built channel instead of building one synchronously. This
    // ordering -- build first, disturb the session second -- is what
    // actually removes the freeze a rate change used to cause; moving the
    // build to another thread alone would do nothing if the old
    // stop-before-build ordering were kept, since the session would still
    // sit torn down for however long the build takes either way.
    m_pendingParams.ddc0RateKsps = newRateKsps;
    m_pendingDspConfig.inputSampleRateHz = newRateKsps * 1000;
    // Refresh from CURRENT live operator state, not connectRadio()'s
    // connect-time snapshot: m_pendingDspConfig otherwise only ever had its
    // rate touched here, so a rate change silently reverted mode/filter/AGC
    // to whatever they were at connect. m_shiftHz is not part of Config --
    // AnanRxDsp::installChannel() re-applies it separately, after the swap.
    m_pendingDspConfig.mode = modeFromString(m_mode);
    m_pendingDspConfig.filterLowHz = static_cast<double>(m_filterLowHz) + cwBfoHz();
    m_pendingDspConfig.filterHighHz = static_cast<double>(m_filterHighHz) + cwBfoHz();
    m_pendingDspConfig.agcMode = m_agcMode;
    m_pendingDspConfig.maximumAgcGainDb = m_agcCeilingDb;

    m_rateChanging = true;
    ++m_connectGeneration;   // orphans any in-flight prior connect/reconfigure/rebuild
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = m_pendingDspConfig;

    // Mark "rebuild in flight" on m_dsp's own thread FIRST, so any
    // setMode/setFilter/setAgc/setShift call that lands while the build is
    // running defers pushing to the (still-live, still-playing) old channel
    // instead of blocking on WDSP's process-wide setup mutex -- see
    // AnanRxDsp::beginRebuild()'s own comment. THEN hand the slow build to
    // the dedicated build thread.
    QMetaObject::invokeMethod(m_dsp, [this, generation, cfg]() {
        m_dsp->beginRebuild();

        QMetaObject::invokeMethod(m_dspBuildContext, [this, generation, cfg]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP rebuild failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(m_dsp, [this, r = std::move(result)]() mutable {
                m_dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, generation, ok, errStr]() {
                finishRateChange(generation, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishRateChange(quint64 generation, bool ok, const QString& error)
{
    if (generation != m_connectGeneration)
        return;   // superseded by a newer rate change/connect/disconnect

    if (!ok) {
        emit connectionError(error);
        // Safe to clear m_rateChanging immediately here, unlike
        // startP2ClientSession()'s own failure branch below: the OLD
        // P2Client session was never touched on this path -- beginRateChange()
        // no longer stops it before building (see that function's own
        // comment) -- so there is no live teardown in flight whose
        // linkDown()/linkUp() could leak through unsuppressed. The old
        // channel and old session simply keep running at the OLD rate.
        m_rateChanging = false;
        emitPanState();
        retryPendingRateChange();
        return;
    }

    // The new channel is already installed (queued just before this call --
    // see beginRateChange()). Only now does the live session get touched.
    // Mute BEFORE tearing down, not reactively once linkUp fires:
    // P2Client::onReadyRead() emits linkUp() and THEN ddc0IqReady() for the
    // very same first frame, but ddc0IqReady is a same-thread
    // DirectConnection to AnanRxDsp::processIqBlock() while linkUp() is a
    // cross-thread queued connection to this class -- so that first block
    // is ALREADY processed, synchronously, by the time a linkUp-triggered
    // mute could ever run. Muting here, before stop() is even queued,
    // guarantees it lands on m_dsp before any new session's first frame can
    // possibly exist.
    QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, true));
    QMetaObject::invokeMethod(m_client, "stop", Qt::QueuedConnection);
    // Give the radio real idle time before asking it to restart -- see
    // kRateChangeRestartSettleMs's own comment for why this is now needed
    // on purpose (the background-rebuild fix removed the accidental delay
    // the old synchronous rebuild used to leave here). Re-checks generation
    // after the wait: a newer rate change/connect/disconnect during the
    // settle window supersedes this one, same guard startP2ClientSession()
    // itself already relies on for its own queued calls.
    QTimer::singleShot(kRateChangeRestartSettleMs, this, [this, generation]() {
        if (generation != m_connectGeneration)
            return;
        startP2ClientSession(generation);
    });
}

void AnanBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);   // one pan in this phase
    // Default no-op inherited from IRadioBackend was written for Flex,
    // whose own display engine paces its frames -- silently wrong here,
    // since it left AnanRxDsp's spectrum production uncapped (measured on
    // the bench: the waterfall scrolled far faster than the operator's own
    // FPS setting, because nothing was ever telling it what that setting
    // was). Mirrors Hl2Backend::setPanFrameRate()'s shape exactly.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumRateFps", Qt::QueuedConnection,
                                  Q_ARG(int, fps));
}

void AnanBackend::setCwPitch(int hz)
{
    m_cwPitchHz = hz;
    // Both the shift AND the filter depend on the pitch -- HERMES §5:
    // "entering or leaving CW has to re-push the shift, not just the
    // filter." Re-derive both from current state rather than adjusting in
    // place, so this is correct whether or not the mode is currently CW.
    pushModeFilterShift();
    emitSliceState();
}

void AnanBackend::setKeying(bool key)
{
    // canTransmit is false and P2Client has no PTT capability -- there is
    // nothing this method could do even if the engine's TX guard (which
    // gates every call site above this seam) let a key-on request through.
    // A key-on request reaching here at all means that guard was bypassed,
    // which is itself the bug to chase -- not something to silently absorb.
    if (key)
        qWarning("AnanBackend::setKeying(true) called on a receive-only backend "
                 "(canTransmit=false) -- the engine TX guard should have refused this");
}

void AnanBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(ns);
    Q_UNUSED(verb);
    Q_UNUSED(arg);
    // No extension namespaces advertised (capabilities().extensionNamespaces
    // is empty) -- matches FlexBackend/Hl2Backend's own precedent for a
    // namespace with no encode path yet: fail the specific request rather
    // than hang a caller waiting for a reply that will never come.
    if (requestId != 0)
        emit extensionError(requestId, QStringLiteral("ANAN: no extension namespaces implemented"));
}

void AnanBackend::emitSliceState()
{
    SliceDelta d;
    d.frequency = m_sliceFreqHz / 1.0e6;   // SliceDelta::frequency is MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.active = true;
    // Without this, RadioModel::sliceChanged's handler never assigns the
    // slice a panId (SliceDelta::panId is std::optional and SliceModel::
    // applyChanges() only touches it when set) -- the slice materialised by
    // "Gap B" stays permanently unassociated with the pan emitPanState()
    // creates, and every click/drag-tune on the panadapter silently no-ops
    // because MainWindow_Wiring.cpp's tune-target resolver matches on
    // panId equality. Hl2Backend::emitSliceState() sets this every time for
    // the same reason.
    d.panId = kPanId;
    emit sliceChanged(kSliceId, d);
}

void AnanBackend::emitPanState()
{
    const double sampleRateHz = static_cast<double>(m_pendingDspConfig.inputSampleRateHz > 0
        ? m_pendingDspConfig.inputSampleRateHz : 48000);
    // Reports the TRUE rate, not a display-cropped fraction of it -- see
    // AnanRxDsp::processIqBlock(), which no longer crops spectrum edges.
    // An earlier attempt at hiding the always-present decimation roll-off
    // reported a reduced bandwidth here while still snapping zoom requests
    // against the real rate; the widget's OWN zoom math then used that
    // reduced value as ITS baseline for the next request, so a 1.5x
    // zoom-out computed from an already-10%-shrunk span landed too close to
    // the current rate to ever cross into "closer to the next one up" --
    // zoom-out silently stopped doing anything (bench-confirmed). Reporting
    // the true rate keeps the zoom math and the pan geometry using the same
    // number; the roll-off is visible again, same as before that attempt.
    emit panCenterBandwidthChanged(kPanId, m_sliceFreqHz / 1.0e6, sampleRateHz / 1.0e6);
}

void AnanBackend::scheduleTuneApply()
{
    if (!m_tuneThrottleTimer->isActive()) {
        // Leading edge: nothing in flight, apply immediately so the first
        // move of a gesture has no added latency.
        applyTuneToRadioAndPan();
        m_tuneThrottleTimer->start(kTuneThrottleMs);
    } else {
        // Already inside a cooldown window from a very recent apply --
        // remember that a trailing catch-up is owed once it elapses, rather
        // than doing the expensive work again right now. m_sliceFreqHz
        // already holds the latest value (set by the caller before this
        // runs), so the timeout handler always applies the newest position,
        // not a stale intermediate one.
        m_tunePendingApply = true;
    }
}

void AnanBackend::applyTuneToRadioAndPan()
{
    // The two genuinely expensive things a retune does: an actual UDP
    // High Priority packet to the radio (P2Client::setDdc0FrequencyHz --
    // real hardware DDC0/NCO retuning, which is not glitch-free at the rate
    // a raw mouse-move stream would otherwise drive it), and the pan's
    // center re-broadcast (PanadapterModel::setCenterBandwidth(), which
    // re-lays-out the spectrum/waterfall geometry on every call). Both are
    // throttled together by scheduleTuneApply() -- see its comment.
    if (m_client && m_connected) {
        const double hz = m_sliceFreqHz;
        QMetaObject::invokeMethod(m_client, [this, hz]() { m_client->setDdc0FrequencyHz(hz); },
                                  Qt::QueuedConnection);
    }
    emitPanState();
}

}  // namespace AetherSDR::anan
