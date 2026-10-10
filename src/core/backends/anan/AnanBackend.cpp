#include "core/backends/anan/AnanBackend.h"
#include "core/AppActivity.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/AnanDroopDefaults.h"
#include "core/backends/WdspNoiseBlanker.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

Q_LOGGING_CATEGORY(lcAnanDefaults, "aether.anan.droopdefaults", QtWarningMsg)

namespace AetherSDR::anan {

const QString AnanBackend::kPanId = QStringLiteral("anan-0");

namespace {

// Placeholder until a bench measurement exists -- see the class comment.
// Named and zero rather than silently absent, so the next reader finds a
// TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

// One FFT AVG slider step as analyzer averaging time. deskHPSDR sets its
// analyzer's averaging as a time in 10 ms steps (default 250 ms), so 0 = none,
// 25 = deskHPSDR's default, 100 = one second. Published in capabilities()
// as BackendPanAveraging::msPerAverageStep.
constexpr int kMsPerAverageStep = 10;

// S-meter: WDSP's RXA_S_AV is 10*log10 mean I^2+Q^2 at the P2 24-bit full scale
// (kFullScale24Bit), i.e. dBFS. deskHPSDR uses the same scaling (1/2^23) and a
// 0 dB ANAN offset, so dBFS reads as dBm; this follows it. onDspMeter() adds the
// ADC attenuation before smoothing. No per-radio rx_gain_calibration yet. Kept
// apart from kUncalibratedDbfsToDbmOffset, which labels the panadapter axis.
constexpr double kSMeterDbmOffset = 0.0;

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
    // WDSP has no RTTY demod: RTTY is FSK decoded externally from a wide passband,
    // so map it to DIGU rather than falling through to USB (HERMES.md 15.7/16.7).
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;   // unknown mode: same fallback as Hl2Backend's
}

bool AnanBackend::isKnownModeString(const QString& mode) noexcept
{
    // Every branch of modeFromString() above, aliases included.
    static const QStringList kKnown = {
        QStringLiteral("LSB"),  QStringLiteral("USB"),  QStringLiteral("DSB"),
        QStringLiteral("CWU"),  QStringLiteral("CW"),   QStringLiteral("CWL"),
        QStringLiteral("FM"),   QStringLiteral("NFM"),  QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("RTTY"),
        QStringLiteral("SAM"),  QStringLiteral("DRM"),  QStringLiteral("WBFM"),
        QStringLiteral("WFM"),
    };
    return kKnown.contains(mode.toUpper());
}

bool AnanBackend::isRestorableFrequencyHz(double hz) noexcept
{
    // Strictly inside (0, Nyquist): zero is the sentinel, and at or above
    // kDspClockHz/2 phaseWord() aliases rather than tuning.
    return std::isfinite(hz) && hz > 0.0
           && hz < static_cast<double>(kDspClockHz) / 2.0;
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

AnanBackend::AnanBackend(QObject* parent)
    : IRadioBackend(parent)
    , m_droopCalibrator(AnanDroopCalibrator::Hooks{
        [this] { return m_connected && !m_radioSerial.isEmpty()
                         && capabilities().hostDroopCalibration; },
        [this](int rateKsps) { setPanBandwidth(kPanId, rateKsps * 1000.0); },
        [this](bool bypass) {
            if (m_dsp) {
                QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionBypassed",
                    Qt::QueuedConnection, Q_ARG(bool, bypass));
            }
        },
        [this](const QMap<int, DroopCorrectionTable>& tables) {
            return applyDroopTables(tables);
        }})
{
    connect(&m_droopCalibrator, &AnanDroopCalibrator::started, this, [this] {
        m_droopMessage = QStringLiteral("Sweeping");
        m_droopPercent = 0;
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::progress, this,
            [this](int rateIndex, int totalRates, int percent) {
        m_droopPercent = percent;
        m_droopMessage = QStringLiteral("Sweeping — rate %1 of %2…")
                            .arg(rateIndex + 1).arg(totalRates);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [this](const QString& reason) {
        m_droopMessage = QStringLiteral("Error: %1").arg(reason);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::finished, this, [this](bool applied) {
        if (!m_droopMessage.startsWith(QLatin1String("Error:"))) {
            m_droopMessage = applied
                ? QStringLiteral("Applied — the measured correction is now live and saved.")
                : (m_droopCalibrator.hasResult()
                    ? QStringLiteral("Sweep stopped — review the result, then Apply or Discard.")
                    : QStringLiteral("Sweep stopped — no result to apply."));
        }
        publishDroopStatus();
    });
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
    AetherSDR::startStreamThread(m_ioThread);   // High QoS; see AppActivity.h

    // Build-only thread for a rate change's background DSP rebuild -- see
    // the member declaration comment. m_dspBuildContext owns no state; it
    // exists purely so QMetaObject::invokeMethod has a thread-affinity
    // target to hop the (slow) AnanRxDsp::buildChannel() call onto.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    // Same High QoS as the io thread: this one holds the FFTW planner mutex the
    // io thread takes in WdspChannel::setShift, so lower would be a priority
    // inversion. Idle except during a rebuild.
    AetherSDR::startStreamThread(m_dspBuildThread);

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
        if (!wasRateChange) {
            emit connected();
            defineMeters();
            // A new session's needle starts from its first reading, not from
            // where the last session's left off.
            m_sMeter.reset();
        }
        emitSliceState();
        emitPanState();
        // A rate change suppresses connected(), but a page opened during
        // its brief link restart still needs the fresh calibration status.
        publishDroopStatus();
        // Real zoom limits (HERMES.md §15.1): otherwise the GUI falls back to a FlexLib
        // model table (5.4 MHz for an unknown "ANAN-G2") and zooms past the 1.536 MHz
        // ceiling into unsampled black bars. Constant per connection (one DDC), so emit
        // on every linkUp; bounds are capabilities().sampleRatesHz's endpoints.
        emit panBandwidthLimitsChanged(kPanId,
                                      kDdc0RatesKsps.front() * 1000.0 / 1.0e6,
                                      kDdc0RatesKsps.back() * 1000.0 / 1.0e6);
        // The RF Gain slider IS the step attenuator: -31..0 dB in 1 dB steps.
        // Same every linkUp, for the same reason as the limits above.
        emit panRfGainInfoChanged(kPanId, -kMaxStepAttenuationDb, 0, 1);
        emit panRfGainChanged(kPanId, -m_attenuationDb);
        if (wasRateChange) {
            // Audio was muted in beginRateChange() before this session started (see there).
            // Unmute after a settle window: the rebuilt WdspChannel's AGC/filters/DC
            // blocker start from zero against live RF. The display is unaffected.
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
        if (!m_rateChanging) {
            m_droopCalibrator.stop(false);
            m_droopCalibrator.setLandedRate(0);
            emit disconnected();
        }
    });
    connect(m_client, &P2Client::connectionError, this,
            [this](const QString& reason) { emit connectionError(reason); });
    // Drops reach the readout through the counters snapshot below, which
    // carries the same total on a fixed cadence. Not wired per-drop: the
    // LinkStats tick is deliberately periodic rather than event-driven, so a
    // link that has gone silent still reports.
    connect(m_client, &P2Client::linkCountersUpdated, this,
            &AnanBackend::onLinkCounters);
    // Only DDC0 feeds this DSP. Like ddc0IqReady above, notification and
    // processing run directly on the I/O thread, with the DSP as context.
    // Invalidate its partial FFT before the discontinuous block arrives.
    connect(m_client, &P2Client::ddcSequenceGap, m_dsp, [dsp = m_dsp](int ddcIndex) {
        if (ddcIndex == 0) {
            dsp->onSequenceGap();
        }
    }, Qt::DirectConnection);
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
        // The shipped droop defaults are derived from one gateware's filter
        // (AnanDroopDefaults.h) and still applied on a mismatch, so say so. Logged here
        // because connectRadio() zeroes m_discoveredFirmwareVer and the reply fills it
        // later. Keyed on firmwareVer only (board type must not gate behaviour), so a
        // non-Saturn board on the same build number goes unflagged. Informational.
        if (firmwareVer != kDefaultsGatewareVersion) {
            qCWarning(lcAnanDefaults).nospace()
                << "ANAN: radio reports gateware " << firmwareVer
                << ", shipped droop defaults were derived against "
                << kDefaultsGatewareVersion
                << " -- applying them anyway; run the in-app droop sweep if "
                   "the panadapter edges look wrong";
        }
        emit capabilitiesChanged();
    });

    connect(m_dsp, &AnanRxDsp::pcmReady, this, [this](const PcmFrame& frame) {
        QByteArray bytes = frame.legacyStereo24();
        if (bytes.isEmpty()) {
            return;
        }
        // THIS SLICE's audio for per-slice consumers (a TCI receiver channel, a
        // decoder), published BEFORE the receiver's audio stage. The seam
        // contract on IRadioBackend::sliceAudioFrameReady is pre-mute, pre-gain
        // and pre-balance, so muting a slice does not stop WSJT-X decoding on it
        // -- the same split a Flex gets from DAX and HL2 makes in its mixer.
        publishLegacySliceAudio(kSliceId, bytes);

        // The receiver's own audio stage, applied HERE rather than in the DSP.
        //
        // legacyStereo24() already hands back a fresh buffer this backend owns,
        // and the non-const data() below detaches from anything the slice tap
        // above may still share, so the stage never reaches that tap. Doing it here also keeps the whole stage on one thread with
        // the state the three setters write, so a mute needs no queued hop to
        // take effect on the next block. Both listening paths -- this computer's
        // speaker and the radio's own speaker below -- see the same audio.
        applySliceAudioInPlace(reinterpret_cast<float*>(bytes.data()),
                               static_cast<std::size_t>(bytes.size())
                                   / (2 * sizeof(float)),
                               m_sliceAudioMuted, m_sliceAudioGainPercent,
                               m_sliceAudioPanPercent);
        // One DDC, so "mixing" the speaker feed is the identity -- no
        // separate mix stage needed for a single receiver.
        publishLegacyAudio(bytes);
        sendSpeakerAudioToRadio(bytes);
    });
    // Ungated by design: m_dsp emits on m_ioThread, the thread that emits linkDown,
    // so every frame is delivered before disconnected() (contract rule 6).
    connect(m_dsp, &AnanRxDsp::spectrumReady, this, [this](const std::vector<float>& binsDbfs) {
        std::vector<float> dbm(binsDbfs.size());
        // Attenuation added back, as deskHPSDR does for its panadapter: a
        // signal keeps its level when the operator attenuates, and only the
        // part of the floor that was the ADC overloading actually drops.
        const float attenuation = static_cast<float>(m_attenuationDb);
        for (std::size_t i = 0; i < binsDbfs.size(); ++i) {
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset + attenuation;
        }
        m_droopCalibrator.onSpectrumFrame(dbm);
        emit spectrumFrameReady(kSliceId, floatBytes(dbm));
    });
    connect(m_dsp, &AnanRxDsp::meterUpdate, this, &AnanBackend::onDspMeter);
}

AnanBackend::~AnanBackend()
{
    m_droopCalibrator.stop(false);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup/rebuild callback

    // Join the build thread BEFORE deleting m_dsp/m_client: the build lambda's tail
    // posts invokeMethod(m_dsp/this, ...) after buildChannel() returns, so deleting
    // first is a use-after-free if the app closes mid-build. The wait is bounded by
    // the remaining FFTW planning.
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
    c.broadcastFmReceive = std::nullopt;
    c.family = QStringLiteral("anan");
    // No setTune() implementation, so no tune generator to select a mode on.
    c.twoToneGenerator = std::nullopt;
    // No VOX, monitor or speech-processor setters. PROC would be the host
    // ClientComp, but canTransmit is false below, so nothing serves it and the
    // drop notice correctly stands.
    c.voxControl = std::nullopt;
    c.speechProcessorControl = std::nullopt;
    c.txMonitorControl = std::nullopt;
    // The panadapter dBm axis is dBFS with a dBm label: kUncalibratedDbfsToDbmOffset
    // is 0.0f and bin levels depend on window/normalisation, unverified against a
    // known input. Internally consistent, but not comparable: never publish as a
    // spot, compare with another station, or use as an absolute threshold. The
    // S-meter (kSMeterDbmOffset) is separate and follows deskHPSDR's scaling.
    PanAmplitudeModel amplitude;
    amplitude.calibratedDbm = false;
    // The bins are raw dBFS + 0, so they are ABSOLUTE: nothing in them depends on
    // the display reference level, and the auto-floor loop converges.
    amplitude.binsAbsolute = true;
    // No register holds a display range: the client is its only memory.
    amplitude.clientPersistsDbmRange = true;
    c.panAmplitude = amplitude;
    // No measured squelch map; the SQL line and Auto SQL keep Flex's scale.
    c.squelchLevelScale = legacyDbmSquelchScale();

    // Span follows the sample rate: it snaps by ratio to one of the six DDC0 rates
    // (no continuous zoom); panBandwidthLimitsChanged clamps to that list's ends.
    // radioWide is false: one receiver, so no span budget is shared (unlike the
    // HL2's one DDC in front of every receiver) (#5725).
    PanSpanModel span;
    span.followsSampleRate = true;
    span.radioWide = false;
    c.panSpanModel = span;
    c.hasAgcThreshold = true; // Host receiver DSP implements threshold/off gain.
    c.manufacturer = QStringLiteral("Apache Labs");
    c.model = QStringLiteral("ANAN-G2");
    // The session takes its receive front-end options from AnanSettings
    // (populateFamilyParams()), so the setup page that edits them applies.
    c.adcFrontEnd = RadioCapabilities::AdcFrontEnd{};

    // ---- PA telemetry ----
    // hasSupplyVoltageTelemetry stays FALSE although the radio sends a
    // supply-rail count (P2Protocol decodes it): no known scale fits it. On a
    // G2 bench the 5 V-reference scale read a 13.8 V rail as ~40 V, 2.9x high,
    // and the 3.3 V board's scale differs from it by only 1.5x, so no board
    // choice fixes it. A volts readout waits for an operator calibration.
    //
    // There is NO PA temperature in this protocol. The radio's whole analog
    // payload is six fields and none of them is a temperature, so the readout
    // can never resolve and is withdrawn rather than left showing a placeholder.
    // hasPaTemperatureTelemetry stays false on its own (nothing is received);
    // this says the stronger thing -- that nothing CAN be.
    c.paTelemetryAudit = RadioCapabilities::PaTelemetryAudit{
        /*temperatureAbsent=*/true};
    c.canCreateSlices = false;
    c.maxSlices = 1;
    c.maxPanadapters = 1;
    for (const int ksps : kDdc0RatesKsps)
        c.sampleRatesHz.append(ksps * 1000);
    // Not reported -- no verified G2 tuning range (RFC: "I have not fetched
    // the Apache Labs G2 manual"). RadioCapabilities.h's own convention:
    // both zero means "not reported", not a guess.
    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    // State is engine-owned, but verified coverage is still unavailable.
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine, 0, 0};
    c.receiveModeControl = std::nullopt; // mode/passband transition contract not yet qualified
    c.receiveFilterControl = std::nullopt;
    // Gain and mute both act on this receiver's audio and are applied by this
    // backend, which is what the record promises. Engine authority: the state is
    // ours, the radio echoes nothing back, and there is no register to read.
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receivePanCenterControl = std::nullopt; // center also retunes the slice
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                         48'000, 1'536'000};
    c.canTransmit = false;         // P2Client has no PTT capability -- see class comment
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;        // client-side WDSP, like the HL2
    c.takesTxAudioOverSeam = true; // moot while canTransmit is false
    // Host-modulated like the HL2, so when this backend gains a drive path it
    // will own the register and report intent, not a readback (#5518). Declared
    // rather than left absent because the ownership answer is already known; it
    // populates no TransmitDelta::rfPower today, so nothing publishes drive yet.
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine, /*tunePowerAppliesLive=*/false};
    c.hasRadioPttReadback = false; // no PTT at all, so no readback either
    c.hasTuner = false;            // G2 has no internal ATU (Apache Labs spec)
    c.hasTunerMemories = false;    // no internal ATU, so no tuner-memory surface
    c.hasAmplifier = false;
    c.hasRadioSideDsp = false;     // DSP is engine-side (AnanRxDsp), not firmware
    c.hasAudioPeakingFilter = false; // no firmware APF verb on this path
    c.hasHostNoiseBlanker = true;  // WDSP ANB on the raw IQ, in AnanRxDsp
    c.radioOwnsDbmScale = false;   // client computes it from raw IQ
    c.hasDdcPanEdgeRolloff = true; // see RadioCapabilities.h's own comment
    c.backendPanAveraging = BackendPanAveraging{kMsPerAverageStep, false, {}, {}}; // AnanPanAnalyzer
    c.panFrameRateShaping = std::nullopt;  // no client owner is declared for FFT FPS
    // No band/segment zoom: the protocol carries no per-pan zoom flag.
    c.panZoomModes = std::nullopt;
    c.persistsMemories = false;    // default; stated explicitly
    // The radio reports no frequency, mode or output level of its own, so the
    // client is their only memory, per radio.
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RfGain
                              | RadioCapabilities::ClientSettingsDomain::Tuning
                              | RadioCapabilities::ClientSettingsDomain::ReceiveOutputLevel;
    c.hostDroopCalibration = true; // AnanDroopCorrection.h -- real DDC0 roll-off,
                                    // corrected client-side via AnanDroopCalibrator
    c.extensionNamespaces = {QStringLiteral("anan")};  // "droop.apply" -- see invokeExtension()
    // From this session's own Discovery reply (P2Client::discoveryInfoReceived()).
    // Absent until it lands ("not reported yet", like tuningMinHz/MaxHz);
    // capabilitiesChanged() fires when it does. numDdc is informational:
    // maxSlices/maxPanadapters stay 1 because only DDC0 is driven.
    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("numDdc")] = m_discoveredNumDdc;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        c.extensions[QStringLiteral("anan")] = anan;
    }
    return c;
}

void AnanBackend::applyRestoredState(const RestoredRadioState& state)
{
    // A missing document resets both ADCs on a same-family radio swap.
    // RadioStateMemory owns persistence; validate its opaque extension here.
    const QJsonObject gain = state.extension.value(QStringLiteral("rfGain")).toObject();
    m_pendingParams.adc0AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc0AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_pendingParams.adc1AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc1AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb : m_pendingParams.adc0AttenuationDb;

    // Unconditional, like the ADC values above, so a same-family backend reuse
    // cannot carry radio A's dial or level onto radio B. A rejected value leaves
    // the sentinel, and connectRadio() falls to the first-connect default.
    m_restoredFreqHz = isRestorableFrequencyHz(state.rfFrequencyHz)
        ? state.rfFrequencyHz : 0.0;
    m_restoredMode = isKnownModeString(state.mode) ? state.mode.toUpper() : QString();
    m_lineoutGainPercent = state.receiveOutputLevelPct >= 0
        ? state.receiveOutputLevelPct : kDefaultLineoutGainPercent;
}

RestoredRadioState AnanBackend::currentOperatingState() const
{
    RestoredRadioState state;
    // The live dial and level, not the restored ones. A level of 0 is reported:
    // a silenced speaker is a choice, and "nothing to remember" is -1.
    state.rfFrequencyHz = m_sliceFreqHz;
    state.mode = m_mode;
    state.receiveOutputLevelPct = m_lineoutGainPercent;
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
        {QStringLiteral("adc0AttenuationDb"), m_pendingParams.adc0AttenuationDb},
        {QStringLiteral("adc1AttenuationDb"), m_pendingParams.adc1AttenuationDb}}}};
    return state;
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

    // Per-radio identity for RadioSettingsScope (droop calibration -- see
    // invokeExtension()'s "droop.apply" handler). Set BEFORE the seed load
    // just below, and before anything else in this function needs it,
    // matching Hl2Backend's own m_radioSerial assignment ordering.
    m_radioSerial = request.serial;
    if (m_dsp) {
        // Forget the PREVIOUS radio's tables before seeding this one's. The
        // seed below only inserts, and m_dsp is constructed once for the
        // lifetime of this backend -- so without this, connecting a second,
        // uncalibrated G2 in the same session renders it through the first
        // one's per-bin corrections. See
        // AnanRxDsp::clearDroopCorrectionTables().
        QMetaObject::invokeMethod(m_dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);

        auto pushTable = [this](int rateKsps, const DroopCorrectionTable& t) {
            QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, rateKsps),
                Q_ARG(std::vector<float>, std::vector<float>(t.begin(), t.end())));
        };

        // Shipped defaults FIRST, so an uncalibrated radio is corrected on first
        // connect (one gateware-derived curve for all rates, AnanDroopDefaults.h). Both
        // lists come from kDdc0RatesKsps, so the null test is a structural guard only.
        for (const int rateKsps : defaultDroopRatesKsps()) {
            if (const DroopCorrectionTable* t = defaultDroopTableForRate(rateKsps))
                pushTable(rateKsps, *t);
        }

        // Then THIS radio's own bench calibration on top, per rate. An
        // operator who measured their own hardware always outranks a shipped
        // default, and a partial sweep only overrides the rates it actually
        // covered rather than wiping the rest back to the default.
        const auto tables = AnanDroopCalibrator::loadTables(
            RadioSettingsScope(QStringLiteral("anan"), m_radioSerial));
        for (auto it = tables.constBegin(); it != tables.constEnd(); ++it)
            pushTable(it.key(), it.value());
    }

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
    // FALSE by default, unlike the options above, and for the opposite reason:
    // their default is the hardware's shipped state, while this one's is "the
    // operator has not asked for an outbound stream". A connect that never
    // populated this param must not start one.
    m_pendingParams.speakerAudioEnabled =
        request.params.value(QStringLiteral("anan.speakerAudioEnabled"), false).toBool();
    // applyRestoredState() seeds both ADC values before this connect.
    m_attenuationDb = m_pendingParams.ddc0AdcIndex == 1
        ? m_pendingParams.adc1AttenuationDb
        : m_pendingParams.adc0AttenuationDb;

    // An explicit connect frequency, then this radio's restored one, then the
    // first-connect default.
    if (request.params.contains(QStringLiteral("anan.rxFrequencyHz"))) {
        m_sliceFreqHz = request.params.value(QStringLiteral("anan.rxFrequencyHz")).toDouble();
    } else if (m_restoredFreqHz > 0.0) {
        m_sliceFreqHz = m_restoredFreqHz;
    } else {
        m_sliceFreqHz = kFirstConnectFrequencyHz;
    }
    // Before m_pendingDspConfig is built below, which reads m_mode. No restored
    // mode means the default, never the previous radio's m_mode. The passband
    // resets only on a mode change, as in setSliceMode().
    const QString mode = m_restoredMode.isEmpty()
        ? QString::fromLatin1(kFirstConnectMode) : m_restoredMode;
    if (mode.compare(m_mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    m_mode = mode;

    m_speakerAudioEnabled = m_pendingParams.speakerAudioEnabled;
    m_pendingDspConfig = AnanRxDsp::Config{};
    m_pendingDspConfig.inputSampleRateHz = m_pendingParams.ddc0RateKsps * 1000;
    m_pendingDspConfig.audioSampleRateHz = 24000;
    m_pendingDspConfig.dspBlockSize = 1024;
    // The panel's width in points (setPanPixelWidth()), or one point per
    // droop-table entry until the GUI has reported one; the analyzer's FFT
    // behind them is larger (see AnanPanAnalyzer). spectrumFps keeps Config's
    // default until RadioModel pushes the operator's rate through
    // setPanFrameRate().
    m_pendingDspConfig.panPoints = m_panPoints;
    m_pendingDspConfig.mode = modeFromString(m_mode);
    m_pendingDspConfig.filterLowHz = static_cast<double>(m_filterLowHz) + cwBfoHz();
    m_pendingDspConfig.filterHighHz = static_cast<double>(m_filterHighHz) + cwBfoHz();
    m_pendingDspConfig.agcMode = 3;
    // 60 dB, not Hl2RxDsp's 39 dB: on the G2 bench audio was too quiet until the
    // ceiling slider reached 100 (= 60 dB via setSliceAgc()'s *0.6 mapping); this
    // chain's uncalibrated gain sits lower than the HL2's. AGC only applies up to
    // the ceiling on weak signals, so this cannot clip a strong one.
    m_pendingDspConfig.maximumAgcGainDb = 60.0;
    // This backend retains the NB request across reconnects. emitSliceState()
    // also supplies that pair if a different radio requires a fresh slice.
    m_pendingDspConfig.noiseBlanker = AetherSDR::toWdsp(m_nbKind);
    m_pendingDspConfig.noiseBlankerLevel = m_nbLevel;
    m_pendingDspConfig.noiseBlankerFill = AetherSDR::toWdsp(m_nbFill);

    ++m_connectGeneration;
    beginDspSetup();
}

void AnanBackend::beginDspSetup()
{
    // Build on m_dspBuildThread, not m_ioThread: buildChannel() can take ~19 s cold
    // (FFTW PATIENT), and a disconnect/close in that window must not block on a
    // BlockingQueuedConnection to m_client queued behind configure().
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
    // Rebuilt here rather than at connect, because this is the one place BOTH a
    // first connect and a rate change pass through. A converter carries filter
    // state, and the audio either side of a DSP rebuild is not one continuous
    // stream, so carrying that state across would ring the tail of the old rate
    // into the head of the new one.
    resetSpeakerResamplers();
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
    retirePcmStreams();
    m_droopCalibrator.stop(false);
    m_droopCalibrator.setLandedRate(0);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup callback
    // Blocking, like ~AnanBackend() and closeEvent()'s other worker stops: the caller
    // may tear down GUI objects or this backend next (the window X button runs this
    // synchronously with the event loop pumping), so the I/O thread must have closed
    // the socket first. disconnected() still arrives via linkDown().
    if (m_client)
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    m_pendingBandwidthKsps = 0;
    // The next session's counters restart from zero in P2Client::start(), so a
    // cached snapshot from this one would make the first tick of the next read
    // as a huge backward jump in every total.
    m_linkCounters = {};
    m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = 0;
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
    // The droop tables are per-RADIO and m_dsp outlives any one connection,
    // so they go with the radio they were measured on. Clearing here (as well
    // as before connectRadio()'s seed) means a disconnected session cannot
    // leave a stale correction armed for whatever connects next, by any path.
    // The bypass flag is cleared too: a disconnect mid-sweep stops the
    // calibrator (the backend's disconnect handler) but its
    // finishSweep() cannot reach a backend that is already gone.
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionBypassed",
                                  Qt::QueuedConnection, Q_ARG(bool, false));
    }
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
    // The Tuning capture rides the same throttle: applyTuneToRadioAndPan().
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
    emit operatingStateChanged();   // Tuning capture; a mode change is discrete
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
    // 0..100 operator units -> 0..60 dB ceiling, same map the HL2 uses
    // (Hl2DbReference::kAgcCeilingDbPerUnit = 0.6) -- a WDSP-range fact, not an
    // HL2 fact. The HL2 additionally REFERS this ceiling to its LNA gain, which
    // is an HL2 fact and deliberately not copied here.
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

ReceiveDispatch AnanBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    // #5824 already owns the worker and NB readback. The typed desktop route
    // must consume that implementation, not replace it or invent other DSP.
    if (sliceId != kSliceId || !request.valid()
        || request.feature != SliceDspRequest::Feature::Nb) {
        return ReceiveDispatch::Unsupported;
    }
    // Level and Fill ride along so the one verb always gets the whole state.
    setSliceNoiseBlanker(sliceId, request.requestedBlanker(), request.level, request.fill);
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                                       int level, AetherSDR::NoiseBlankerFill fill)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    m_nbKind = kind;
    m_nbLevel = std::clamp(level, 0, 100);
    m_nbFill = fill;
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setNoiseBlanker", Qt::QueuedConnection,
            Q_ARG(WdspChannel::NoiseBlanker, AetherSDR::toWdsp(m_nbKind)),
            Q_ARG(int, m_nbLevel),
            Q_ARG(WdspChannel::NoiseBlankerFill, AetherSDR::toWdsp(m_nbFill)));
    }
}

ReceiveDispatch AnanBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (sliceId != kSliceId || !request.valid()
        || request.origin != SliceAudioRequest::Origin::Operator) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case SliceAudioRequest::Field::Gain: setSliceAudioGain(sliceId, request.value); break;
    case SliceAudioRequest::Field::Mute: setSliceAudioMute(sliceId, request.value != 0); break;
    case SliceAudioRequest::Field::Pan: setSliceAudioPan(sliceId, request.value); break;
    }
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceAudioMute(int sliceId, bool mute)
{
    Q_UNUSED(sliceId);   // one slice in this phase
    if (m_sliceAudioMuted == mute) {
        return;
    }
    m_sliceAudioMuted = mute;
    // Published, not just stored. The gate that decides whether the control is
    // offered at all requires an OBSERVATION as well as the capability, so a
    // mute that is applied but never echoed leaves the control refused with
    // "capability.unavailable" -- working audio behind a dead control.
    emitSliceState();
}

void AnanBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    Q_UNUSED(sliceId);
    const int bounded = std::clamp(gainPercent, 0, 100);
    if (m_sliceAudioGainPercent == bounded) {
        return;
    }
    m_sliceAudioGainPercent = bounded;
    emitSliceState();
}

void AnanBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    Q_UNUSED(sliceId);
    // NOT published: SliceDelta carries no balance field, so there is no
    // observation to echo and nothing downstream reads one. Applied all the
    // same -- the audio stage is the same stage either way.
    m_sliceAudioPanPercent = std::clamp(panPercent, 0, 100);
}

void AnanBackend::setLineoutGain(int percent)
{
    // The percent runs through the SAME dB law as the per-slice fader
    // (sliceAudioAmplitude, at the send), so 50 here is -20 dB and not half
    // amplitude. Worth knowing when comparing families: a Flex's own lineout
    // route sends `mixer lineout gain 50` and whatever that means at the radio is
    // its own scale, so the same slider position is not promised to be the same
    // loudness across radios. One law for both of OUR level controls is the
    // trade taken -- the alternative is a fader whose feel changes depending on
    // which of the three levels the operator happens to be moving.
    const int wanted = std::clamp(percent, 0, 100);
    if (wanted == m_lineoutGainPercent) {
        return;
    }
    m_lineoutGainPercent = wanted;
    emit operatingStateChanged();   // ReceiveOutputLevel capture
}

void AnanBackend::setLineoutMute(bool mute)
{
    m_lineoutMuted = mute;
}

void AnanBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);   // one pan in this phase
    Q_UNUSED(intent);  // ignored, matching Hl2Backend::setPanCenter
    setSliceFrequency(kSliceId, hz);
}

int AnanBackend::nearestDdc0RateKsps(int requestedKsps) noexcept
{
    // DDC0 runs at exactly one of these rates (no continuous zoom). Nearest by RATIO
    // (log domain), like Hl2Backend::nearestIqSampleRateHz (HERMES.md §15.1): the
    // rates are octave-spaced and zoom is multiplicative, so 140 ksps belongs to 192
    // (geometric mean 135.8), not 96. The equidistant point (e.g. 96*sqrt(2)) is
    // never an integer request, so no tie-break is needed.
    if (requestedKsps <= 0)
        return kDdc0RatesKsps.front();
    int best = kDdc0RatesKsps.front();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int r : kDdc0RatesKsps) {
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
        // Already at the closest rate: republish so an optimistic GUI span snaps back
        // (as PanadapterModel::republishCenterBandwidth(), #4470), and clear any pending
        // request so a stale older zoom cannot fire after this one.
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
    // A live rate change: build the new WdspChannel on m_dspBuildThread FIRST (up to
    // ~a minute cold) while the old channel and session keep running undisturbed;
    // only then does finishRateChange() mute, swap and send the new rate to the
    // RUNNING session (P2Client::setDdcRateLive()). Build-before-disturb is what
    // keeps zoom from freezing. m_preRateChangeKsps records the running rate so the
    // failure path can roll back: emitPanState() reports from m_pendingDspConfig.
    m_preRateChangeKsps = m_pendingParams.ddc0RateKsps;
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
    m_pendingDspConfig.noiseBlanker = AetherSDR::toWdsp(m_nbKind);
    m_pendingDspConfig.noiseBlankerLevel = m_nbLevel;
    m_pendingDspConfig.noiseBlankerFill = AetherSDR::toWdsp(m_nbFill);

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
        // Roll the reported rate back to the one still running BEFORE
        // emitting pan state. AnanDroopCalibrator infers "the rate landed"
        // from pan bandwidth reaching its target; told the failed rate had
        // landed, it would measure the OLD rate's spectrum into the NEW
        // rate's correction table and persist it -- one rate's droop curve
        // applied to another rate's bins, which is exactly the cross-rate
        // corruption anan_rxdsp_handedness_test's Group 6 exists to prevent.
        if (m_preRateChangeKsps > 0) {
            m_pendingParams.ddc0RateKsps = m_preRateChangeKsps;
            m_pendingDspConfig.inputSampleRateHz = m_preRateChangeKsps * 1000;
        }
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

    // The new channel is already installed (queued just before this call). Mute
    // here, not on a later event: frames reach AnanRxDsp::processIqBlock() by
    // DirectConnection on the I/O thread, so anything triggered after the rate
    // lands would be at least one block late.
    QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, true));

    // LIVE rate change on the running session: p2app applies DDC-Specific rate
    // changes by a direct FPGA register write with no teardown (see
    // P2Client::setDdcRateLive()). Until it lands the new channel sees old-rate
    // samples; the mute and settle window cover that. Sent THREE times
    // (kRateChangeResendMs): it is fire-and-forget UDP, the keepalive never resends
    // DDC-Specific, and a lost packet would leave the radio at the old rate with no
    // error while everything here records the new one. Duplicates are no-ops
    // (WriteP2DDCRateRegister() fires on change; HandlerCheckDDCSettings() is
    // empty) (#5547).
    const int rateKsps = m_pendingDspConfig.inputSampleRateHz / 1000;
    auto sendRate = [this, rateKsps, generation]() {
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_client, "setDdcRateLive", Qt::QueuedConnection,
                                  Q_ARG(int, 0), Q_ARG(int, rateKsps));
    };
    sendRate();
    for (const int delayMs : kRateChangeResendMs)
        QTimer::singleShot(delayMs, this, sendRate);

    QTimer::singleShot(kRateChangeLiveSettleMs, this, [this, generation]() {
        // Same generation guard the restart path used: a newer rate
        // change/connect/disconnect during the settle window supersedes this.
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, false));
        m_rateChanging = false;
        emitPanState();
        // A zoom the operator kept turning while this ran: apply the latest
        // request now rather than stranding the display a step behind.
        retryPendingRateChange();
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

void AnanBackend::setPanRfGain(const QString& panId, int gainDb)
{
    Q_UNUSED(panId);   // one pan in this phase
    // The G2 has no gain stage the host drives, only the step attenuator in
    // front of each ADC (Protocol v4.4 pp.34,36, HP bytes 1442/1443).
    // deskHPSDR offers it as a 0-31 dB attenuation control; here it rides the RF Gain
    // slider, negated, so "more gain" still means "slider right". Clamped
    // rather than refused, per the seam's contract.
    const int attenuation = -std::clamp(gainDb, -kMaxStepAttenuationDb, 0);
    m_attenuationDb = attenuation;
    // Kept in the params too: a rate change that restarts the session
    // resends everything from m_pendingParams.
    if (m_pendingParams.ddc0AdcIndex == 1) {
        m_pendingParams.adc1AttenuationDb = attenuation;
    } else {
        m_pendingParams.adc0AttenuationDb = attenuation;
    }
    if (m_client) {
        QMetaObject::invokeMethod(m_client, "setStepAttenuationDb", Qt::QueuedConnection,
                                  Q_ARG(int, m_pendingParams.ddc0AdcIndex),
                                  Q_ARG(int, attenuation));
    }
    emit panRfGainChanged(kPanId, -attenuation);
    emit operatingStateChanged();
}

void AnanBackend::setPanAverage(const QString& panId, int average)
{
    Q_UNUSED(panId);   // one pan in this phase
    // The operator's FFT AVG slider, which otherwise reaches nothing on this
    // radio: the panadapter is averaged inside WDSP's analyzer, as a TIME --
    // see kMsPerAverageStep.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumAverageMs", Qt::QueuedConnection,
                                  Q_ARG(int, std::max(0, average) * kMsPerAverageStep));
}

void AnanBackend::setPanWeightedAverage(const QString& panId, bool on)
{
    Q_UNUSED(panId);   // one pan in this phase
    // deskHPSDR offers the analyzer's averaging MODE alongside its time. The
    // toggle picks between the two recursive modes: on = log-recursive
    // (deskHPSDR's default, the smoother look), off = linear-recursive (an
    // average of power, which reads noise and weak signals at their true
    // level).
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumLogAverage", Qt::QueuedConnection,
                                  Q_ARG(bool, on));
}

void AnanBackend::setPanPixelWidth(const QString& panId, int pixels)
{
    Q_UNUSED(panId);   // one pan in this phase
    // One analyzer point per screen pixel, as deskHPSDR sizes its analyzer
    // from the panel width. A fixed 1024 points stretched across a wider
    // panel is what made the waterfall look soft.
    if (pixels < 2)
        return;
    m_panPoints = std::min(pixels, AnanPanAnalyzer::kMaxPoints);
    m_pendingDspConfig.panPoints = m_panPoints;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setPanPoints", Qt::QueuedConnection,
                                  Q_ARG(int, m_panPoints));
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

void AnanBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    // canTransmit is false and P2Client has no PTT capability -- there is
    // nothing this method could do even if the engine's TX guard (which
    // gates every call site above this seam) let a key-on request through.
    // A key-on request reaching here at all means that guard was bypassed,
    // which is itself the bug to chase -- not something to silently absorb.
    if (key)
        qWarning("AnanBackend::setKeying(true) called on a receive-only backend "
                 "(canTransmit=false) -- the engine TX guard should have refused this");
}

QString AnanBackend::persistDroopTables(const QMap<int, anan::DroopCorrectionTable>& tables)
{
    if (tables.isEmpty())
        return QStringLiteral("the request carried no valid correction table");
    // Never write an empty radio_id row (AGENTS.md): RadioSettingsScope falls
    // back exact-radio -> family-wide on read, so a row written with no
    // identity would be silently adopted by every ANAN that has none of its
    // own -- the same guard Hl2Backend::applyFreqCalPpb() uses. This is the
    // one thing saveTables() cannot judge for itself: a family-wide scope is
    // perfectly VALID, just not what a per-radio calibration wants.
    if (m_radioSerial.isEmpty()) {
        return QStringLiteral("no radio identity yet -- the correction is live "
                              "for this session only");
    }
    return AnanDroopCalibrator::saveTables(
        RadioSettingsScope(QStringLiteral("anan"), m_radioSerial), tables);
}

QVariantMap AnanBackend::droopStatus() const
{
    // EVERY DDC0 rate, tagged with where its correction came from -- not just
    // the swept ones. Reporting only measuredTables() told the operator
    // "nothing measured" while the defaults connectRadio() seeds were live on
    // the display, so a panadapter that looked wrong had no surface anywhere
    // connecting it to a correction that was in fact being applied. The tag
    // is what keeps "derived from the gateware" and "measured on this radio"
    // distinguishable rather than collapsing them into one list.
    QVariantList corrections;
    const auto& measured = m_droopCalibrator.measuredTables();
    for (const int rateKsps : defaultDroopRatesKsps()) {
        const auto it = measured.constFind(rateKsps);
        const bool isMeasured = it != measured.constEnd();
        // A default is only live once connectRadio() has actually seeded it.
        // Before that the rate genuinely carries no correction and must not
        // claim one -- measured results, by contrast, outlive the connection
        // because the calibrator holds them for the session.
        const DroopCorrectionTable* table =
            isMeasured ? &it.value()
                       : (m_connected ? defaultDroopTableForRate(rateKsps) : nullptr);
        if (!table)
            continue;
        const auto [lo, hi] = std::minmax_element(table->begin(), table->end());
        corrections.append(QVariantMap{
            {QStringLiteral("rateKsps"), rateKsps},
            {QStringLiteral("minDb"), *lo},
            {QStringLiteral("maxDb"), *hi},
            {QStringLiteral("source"), isMeasured ? QStringLiteral("measured")
                                                  : QStringLiteral("default")}});
    }
    return {{QStringLiteral("running"), m_droopCalibrator.isRunning()},
            {QStringLiteral("rateIndex"), m_droopCalibrator.rateIndex()},
            {QStringLiteral("totalRates"), m_droopCalibrator.totalRates()},
            {QStringLiteral("hasResult"), m_droopCalibrator.hasResult()},
            {QStringLiteral("percent"), m_droopPercent},
            {QStringLiteral("message"), m_droopMessage},
            {QStringLiteral("corrections"), corrections}};
}

void AnanBackend::publishDroopStatus()
{
    emit extensionStatus(QStringLiteral("anan"), QStringLiteral("droop"), droopStatus());
}

QString AnanBackend::applyDroopTables(const QMap<int, DroopCorrectionTable>& tables)
{
    // Preserve the existing apply/persist behavior. The sweep and its data
    // stay below the seam; clients can apply a result, never inject tables.
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        if (m_dsp) {
            QMetaObject::invokeMethod(m_dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, it.key()), Q_ARG(std::vector<float>,
                    std::vector<float>(it.value().begin(), it.value().end())));
        }
    }
    return persistDroopTables(tables);
}

void AnanBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);
    if (ns == QLatin1String("anan") && verb == QLatin1String("nb.get")) {
        if (requestId != 0) {
            const AnanRxDsp::NoiseBlankerState applied = m_dsp
                ? m_dsp->noiseBlankerState() : AnanRxDsp::NoiseBlankerState{};
            const QVariantMap receiver{
                {QStringLiteral("ddc"), 0},
                {QStringLiteral("panId"), kPanId},
                // `on` stays, and stays first: it is what every existing script
                // reads. `kind` is the same fact with the second blanker in it.
                {QStringLiteral("on"), applied.on()},
                {QStringLiteral("kind"), static_cast<int>(applied.kind)},
                {QStringLiteral("level"), applied.level},
                {QStringLiteral("fill"), static_cast<int>(applied.fill)},
                {QStringLiteral("requestedOn"),
                 m_nbKind != AetherSDR::NoiseBlankerKind::Off},
                {QStringLiteral("requestedKind"), static_cast<int>(m_nbKind)},
                {QStringLiteral("requestedLevel"), m_nbLevel},
                {QStringLiteral("requestedFill"), static_cast<int>(m_nbFill)},
                {QStringLiteral("hasChain"), applied.hasChain},
                {QStringLiteral("threshold"),
                 WdspChannel::noiseBlankerThresholdForLevel(applied.level)},
            };
            emit extensionResult(requestId, QVariantMap{
                {QStringLiteral("receivers"), QVariantList{receiver}},
            });
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb.startsWith(QLatin1String("droop."))) {
        if (!capabilities().hostDroopCalibration || !m_connected || m_radioSerial.isEmpty()) {
            emit extensionError(requestId, QStringLiteral("connect an ANAN before calibrating"));
            return;
        }
        const QString action = verb.mid(6);
        if (action == QLatin1String("status")) {
            emit extensionResult(requestId, droopStatus());
            return;
        }
        if (action != QLatin1String("start") && action != QLatin1String("stop")
            && action != QLatin1String("apply") && action != QLatin1String("discard")) {
            emit extensionError(requestId, QStringLiteral("unknown droop calibration action"));
            return;
        }
        if (m_droopCalibrator.isRunning() && action != QLatin1String("stop")) {
            emit extensionError(requestId, QStringLiteral("a sweep is already running -- stop it first"));
            return;
        }
        QString failure;
        const QMetaObject::Connection errorConnection = connect(
            &m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [&failure](const QString& reason) { failure = reason; });
        m_droopMessage.clear();
        if (action == QLatin1String("start")) {
            m_droopCalibrator.start();
        } else if (action == QLatin1String("stop")) {
            m_droopCalibrator.stop();
        } else if (action == QLatin1String("apply")) {
            m_droopCalibrator.applyResult();
        } else {
            m_droopCalibrator.clear();
            m_droopPercent = 0;
            m_droopMessage = QStringLiteral("Discarded — no sweep result staged.");
        }
        QObject::disconnect(errorConnection);
        publishDroopStatus();
        if (!failure.isEmpty()) {
            emit extensionError(requestId, failure);
        } else {
            emit extensionResult(requestId, droopStatus());
        }
        return;
    }
    if (requestId != 0) {
        emit extensionError(requestId, QStringLiteral("ANAN: unknown extension verb '%1.%2'")
                                           .arg(ns, verb));
    }
}

void AnanBackend::defineMeters()
{
    // Index is ours to choose -- nothing on a P2 radio assigns meter ids. Same
    // name, unit and range as Hl2Backend::defineMeters()'s receive meter, the
    // bare "SLC:LEVEL" MeterModel binds to the S-meter. Receive-only: no TX
    // meters until this backend can transmit.
    MeterDef d;
    d.index = 1;
    d.source = QStringLiteral("SLC");
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level");
    emit meterDefined(d);
}

IRadioBackend::LinkStats AnanBackend::linkStats() const
{
    LinkStats s;
    // Nothing measured yet: the consumer keeps whatever source it already had
    // rather than being handed a snapshot of zeros.
    if (!m_connected || !m_linkCountersSeen)
        return s;
    s.reported = true;
    s.alive = m_linkCounters.rxBytes > m_lastSnapshotRxBytes;
    s.rxBytes = static_cast<qint64>(m_linkCounters.rxBytes);
    s.txBytes = static_cast<qint64>(m_linkCounters.txBytes);
    s.rxPackets = m_linkCounters.rxPackets;
    s.rxPacketsLost = m_linkCounters.drops;
    s.localEndpoint = m_linkCounters.localEndpoint;
    // LEFT AT -1, WHICH THE STRUCT DEFINES AS "NOT MEASURED" AND THE MODEL
    // HONOURS BY IGNORING. This transport is stream-only: the radio is told
    // where to send and then sends, with no request/response exchange to time,
    // so there is no round trip here to measure. Reporting 0 instead would
    // render as a perfect "< 1 ms" link -- a measurement never taken.
    // rttMs, jitterMs, gapMs and gapMaxMs all keep their -1 defaults.
    return s;
}

void AnanBackend::onLinkCounters(const P2Client::LinkCounters& counters)
{
    // A session restart shows as a BACKWARD jump, since P2Client::start() zeroes
    // its counters; the cache is then a dead session's and is discarded here,
    // whichever path restarted it. The first snapshot of a session compares
    // against ZERO: against itself, it would read dead on the first tick.
    if (counters.rxBytes < m_linkCounters.rxBytes)
        m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = m_linkCountersSeen ? m_linkCounters.rxBytes : 0;
    m_linkCounters = counters;
    m_linkCountersSeen = true;
    emit linkStatsUpdated(linkStats());
}

void AnanBackend::onDspMeter(float dbfs)
{
    const double dbm = static_cast<double>(dbfs) + kSMeterDbmOffset + m_attenuationDb;
    // Smooth EVERY reading, publish only on the tick -- the same smoother,
    // and the same reasons, as Hl2Backend's receive meter (SMeterSmoother).
    // AnanRxDsp reads the tap at one DSP-rate block's cadence whatever the
    // DDC0 rate (WdspSMeter::emitEveryBlocks), so this sees ~47 readings a
    // second at 48 and at 1536 ksps alike.
    if (const auto out = m_sMeter.feed(dbm))
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), *out);
}

void AnanBackend::emitSliceState()
{
    SliceDelta d;
    d.frequency = m_sliceFreqHz / 1.0e6;   // SliceDelta::frequency is MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.active = true;
    // A different radio replaces the slice model but retains this backend.
    // Publish the retained NB request so that fresh model agrees with the DSP.
    d.nb = m_nbKind != AetherSDR::NoiseBlankerKind::Off;
    d.nbKind = m_nbKind;
    d.nbLevel = m_nbLevel;
    d.nbFill = m_nbFill;
    // The audio stage as APPLIED. Both are required, not cosmetic: the receive
    // control gate refuses an operation whose observation is absent, so without
    // these two the mute and the fader are offered and then rejected.
    //
    // Safe to echo unconditionally -- SliceModel::applyChanges() assigns these
    // into its observation without re-emitting a command, so a published value
    // cannot loop back as a fresh intent (Principle II).
    d.audioGain = m_sliceAudioGainPercent;
    d.audioMute = m_sliceAudioMuted;
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
    // Reports the TRUE rate, not a cropped fraction: the pan widget's zoom math uses
    // this as its baseline, and a reduced value stops zoom-out from ever crossing to
    // the next rate. The decimation roll-off stays visible (and is corrected by the
    // droop tables).
    m_droopCalibrator.setLandedRate(static_cast<int>(sampleRateHz / 1000.0));
    emit panCenterBandwidthChanged(kPanId, m_sliceFreqHz / 1.0e6, sampleRateHz / 1.0e6);
}

void AnanBackend::resetSpeakerResamplers()
{
    m_speakerResampleL.reset();
    m_speakerResampleR.reset();
    m_speakerOutL.clear();
    m_speakerOutR.clear();
    if (!m_speakerAudioEnabled) {
        return;
    }
    const int src = m_pendingDspConfig.audioSampleRateHz;
    if (src <= 0 || src == kSpeakerSampleRateHz) {
        // Already at the stream's rate: no converter, and none is built rather
        // than a 1:1 one being built and trusted to be transparent. A ratio-1
        // resampler still costs a filter and still adds its group delay.
        return;
    }
    m_speakerResampleL = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
    m_speakerResampleR = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
}

void AnanBackend::sendSpeakerAudioToRadio(const QByteArray& stereoFloat)
{
    if (!m_speakerAudioEnabled || m_client == nullptr) {
        return;
    }
    const std::size_t frames =
        static_cast<std::size_t>(stereoFloat.size()) / (2 * sizeof(float));
    if (frames == 0) {
        return;
    }
    const auto* in = reinterpret_cast<const float*>(stereoFloat.constData());

    // Split before converting. The two channels have to travel through separate
    // converters to keep their difference (see the member declaration), and a
    // deinterleaved copy is what a mono converter takes.
    m_speakerSrcL.resize(frames);
    m_speakerSrcR.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        m_speakerSrcL[i] = in[i * 2];
        m_speakerSrcR[i] = in[i * 2 + 1];
    }

    const float* outL = m_speakerSrcL.data();
    const float* outR = m_speakerSrcR.data();
    std::size_t outFrames = frames;
    if (m_speakerResampleL && m_speakerResampleR) {
        const int nL = m_speakerResampleL->process(m_speakerSrcL.data(),
                                                   static_cast<int>(frames), m_speakerOutL);
        const int nR = m_speakerResampleR->process(m_speakerSrcR.data(),
                                                   static_cast<int>(frames), m_speakerOutR);
        // Identical configuration and identical input length, so these agree;
        // taking the shorter is not a correction but a refusal to walk off the
        // end of one buffer if they ever stop agreeing. A converter's first
        // calls legitimately return 0 while its filter fills.
        const int n = std::min(nL, nR);
        if (n <= 0) {
            return;
        }
        outFrames = static_cast<std::size_t>(n);
        outL = reinterpret_cast<const float*>(m_speakerOutL.constData());
        outR = reinterpret_cast<const float*>(m_speakerOutR.constData());
    }

    // The RADIO's own level, on top of the per-slice stage; applied here so turning
    // the radio down never quiets the computer's speakers. A mute sends SILENCE, it
    // does not stop sending: stopping would underflow the codec FIFO (polluting the
    // underflow signal and clicking on unmute) and invalidate the pacer's estimate.
    const float lineout = m_lineoutMuted ? 0.0f
                                         : sliceAudioAmplitude(m_lineoutGainPercent);

    // Interleave and quantise. sliceAudioToInt16() carries the
    // clamp-before-scale law and the non-finite guard, with the tests for both.
    m_speakerInterleaved.resize(outFrames * 2);
    for (std::size_t i = 0; i < outFrames; ++i) {
        m_speakerInterleaved[i * 2] = sliceAudioToInt16(outL[i] * lineout);
        m_speakerInterleaved[i * 2 + 1] = sliceAudioToInt16(outR[i] * lineout);
    }

    // One queued hand-off per block. P2Client owns the socket, the packetising
    // and the pacing on its own thread; this side owns the DSP and the audio
    // stage. Copying the block is the price of that boundary and it is a few
    // hundred bytes.
    QMetaObject::invokeMethod(m_client, "enqueueSpeakerAudio", Qt::QueuedConnection,
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(m_speakerInterleaved.data()),
                                     static_cast<qsizetype>(outFrames * 2 * sizeof(qint16)))));
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
    // Tuning capture, once per settled gesture. Outside the connected guard:
    // where the dial ended up is worth keeping even during a teardown.
    emit operatingStateChanged();
}

}  // namespace AetherSDR::anan
