#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Bands.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"

#include <QJsonObject>

#include <cmath>
#include <limits>

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/dsp/WdspProcessTally.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/Hl2AdcPairing.h"
#include "core/backends/hl2/Hl2BandMemoryPolicy.h"
#include "core/backends/hl2/Hl2BandscopeHeadroom.h"
#include "core/backends/hl2/Hl2OverloadPolicy.h"
#include "core/backends/hl2/Hl2DspSetupPolicy.h"
#include "core/backends/hl2/Hl2GainSplit.h"
#include "core/backends/hl2/Hl2TxLevelPolicy.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include "core/AutomationBridgeSettings.h"
#include "core/LogManager.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2FreqCal.h"
#include "core/backends/hl2/Hl2Settings.h"

#include <QByteArray>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

// Hl2Backend.h seeds m_alcTargetPeak with a literal (it can only
// forward-declare Hl2TxDsp); this keeps the seed equal to the modulator's
// default so a pre-connect snapshot is honest.
static_assert(AetherSDR::hl2::Hl2TxDsp::Config{}.alcTargetPeak == 0.85,
              "Hl2Backend.h seeds m_alcTargetPeak with this value — keep them equal");

// lcHl2Tx moved to LogManager.h/.cpp beside every other category: this string
// has a second writer now (MetisClient's host-queue starvation lines), and one
// shared object is better than two with separate enabled flags.

namespace AetherSDR::hl2 {

// Declared incomplete in the header so it keeps its forward declarations of
// MetisClient/Hl2RxDsp/Hl2TxDsp; the async connect is the only thing that needs
// their Config types by value. See beginDspSetup().
struct Hl2Backend::PendingConnect {
    MetisClient::Params mp;
    Hl2RxDsp::Config dc;
    Hl2TxDsp::Config tc;
    int actualNumRx = 0;
    // Which connect this is. A disconnect, or a second connect, bumps
    // m_connectGeneration; a build whose generation is stale on completion
    // tears itself down instead of starting a wire nobody asked for.
    quint64 generation = 0;
    // How long this phase has been running. Belongs to the connect rather than
    // to the backend so a superseded build cannot report the new one's elapsed.
    QElapsedTimer clock;
    // Set when the phase watchdog has already released the caller with a
    // dspSetupFinished(). The build is still running and finishDspSetup() will
    // reach its stale branch later; this stops that branch emitting a second
    // end-of-phase edge for one connect.
    bool finishSignalled = false;
};

// What the I/O thread carries back. Parallel arrays indexed by DDC rather than
// a vector of structs so a partial build — the trim-on-failure case — reads the
// same way as a complete one.
struct Hl2Backend::DspSetupResult {
    std::vector<bool> rxOk;
    std::vector<int> rxChannelId;
    std::vector<std::string> rxErr;
    bool txOk = false;
    std::string txErr;
};

namespace {

SampleRate sampleRateEnum(int hz) noexcept
{
    switch (hz) {
    case 96000:  return SampleRate::R96k;
    case 192000: return SampleRate::R192k;
    case 384000: return SampleRate::R384k;
    default:     return SampleRate::R48k;
    }
}

// kIqSampleRatesHz has moved to Hl2Backend.h — still ONE list, now visible to
// the capability test so the declaration is pinned against the array production
// reads rather than against a copy. See the comment there.

// The radio's rated output, in watts, as the gauges' full-scale reference.
//
// The HL2 wiki's own FAQ: "The Hermes-Lite 2.0 is a QRP transceiver and
// achieves 5W out on all HF amateur radio bands." A published figure rather
// than a measurement, which is the right kind of number for a scale — it must
// be the same on every operator's radio, not a property of one unit's PA.
constexpr int kHl2RatedOutputWatts = 5;

// The widest rate this session will offer. "Use low bandwidth mode" caps it at
// 96 kHz (6.3 Mbps) instead of 384 kHz (25.2 Mbps, 3048 packets/s), for
// requests and advertised limits alike.
int maxIqSampleRateHz() noexcept
{
    constexpr int kLowBandwidthCeilingHz = 96000;
    if (!Hl2Settings::lowBandwidth())
        return kIqSampleRatesHz[std::size(kIqSampleRatesHz) - 1];
    return kLowBandwidthCeilingHz;
}

// Snap a requested span (Hz) to the nearest offered rate in the log domain:
// the rates are octave-spaced and zoom is multiplicative, so linear-nearest
// would bias toward the wider neighbour.
int nearestIqSampleRateHz(double requestedHz) noexcept
{
    // Below the narrowest rate there is nothing to interpolate toward, and log()
    // of a non-positive request is undefined.
    if (!(requestedHz > 0.0))
        return kIqSampleRatesHz[0];

    const int ceiling = maxIqSampleRateHz();
    int best = kIqSampleRatesHz[0];
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int rate : kIqSampleRatesHz) {
        if (rate > ceiling)
            break;                     // ascending list; nothing wider is offered
        const double distance =
            std::abs(std::log(requestedHz / static_cast<double>(rate)));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = rate;
        }
    }
    return best;
}

// Neutral AGC vocabulary -> WDSP RXA AGC mode. WDSP also has "long" (1), which
// the slice model's four-way control never produces, so it is unreachable here
// rather than silently aliased onto something else.
//
// A free function because two callers need it: the operator's AGC change, and the
// rebuild a sample-rate change forces (a reconfigured channel opens on WDSP's own
// defaults, so the current mode has to be reapplied or the operator's AGC would
// silently revert every time they zoomed).
int wdspAgcMode(const QString& mode) noexcept
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("off"))   return 0;
    if (m == QLatin1String("slow"))  return 2;
    if (m == QLatin1String("fast"))  return 4;
    return 3;                                  // medium: WDSP's own default
}

WdspChannel::Mode modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    if (u == QLatin1String("CWL"))  return WdspChannel::Mode::Cwl;
    // "CW" is the upper-sideband CW mode name the rest of the app uses (it is
    // what TciProtocol::tciToSmartSDR produces for TCI's `cw`, and what a Flex
    // reports); "CWU" is the explicit spelling. Only CWU was listed, so plain CW
    // fell through to the USB fallback below and was demodulated as SSB -- the
    // mode indicator read CW while the passband and detector were not.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM"))
        return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;
}

// wdspAgcMode() falls back to medium for unknown strings, so the restore
// boundary drops unknown values instead of persisting a silent "med". Uses
// SliceModel's four-way vocabulary ("med", not "medium").
bool isKnownAgcModeString(const QString& mode) noexcept
{
    const QString m = mode.trimmed().toLower();
    return m == QLatin1String("off") || m == QLatin1String("slow")
           || m == QLatin1String("med") || m == QLatin1String("fast");
}

// Default RX passband per mode, in Hz relative to the carrier; the sign carries
// the sideband (SliceModel's convention), and a wrong sign would be silently
// "corrected" by normalizeFilterPolarity. Digital modes pass the full 3 kHz
// window WSJT-X expects.
std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    // CW: 500 Hz centred on the carrier; the pitch offset is in the BFO
    // (cwBfoHz), so CWU and CWL share this entry. Matches VfoWidget's
    // {-w/2, +w/2} presets and FlexLib Slice.cs (cuts clamped to
    // ±12000 - CWPitch).
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {-250, 250};
    // Carrier-straddling modes: symmetric about the carrier, which the envelope
    // and synchronous detectors both need.
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

// The CW BFO offset for `mode`, in Hz of audio: where a signal on the marker
// should come out. Positive for upper-sideband CW, negative for lower, zero
// otherwise. WDSP's detector has no BFO (the NBP edges select the sideband),
// so the pitch comes from offsetting the detector's zero: dspFilterHz() maps
// carrier-relative cuts to audio and rxShiftHz() moves the zero.
double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return static_cast<double>(pitchHz);
    if (u == QLatin1String("CWL"))
        return -static_cast<double>(pitchHz);
    return 0.0;
}

// Default TX passband per mode, in Hz, positive for every mode. RX
// (RXANBPSetFreqs) selects the sideband by the passband's sign; Hl2TxDsp
// selects it by mode (isLowerSideband() negates Q) and treats the passband as
// an audio magnitude, so a negative pair flips LSB/DIGL onto USB
// (hl2_txdsp_test). WDSP TXA is signed like RXA, so read docs/HERMES.md
// section 5 before moving TX onto a TXA channel. QString adapter over
// hl2::defaultTxPassbandForModeName, which holds the mapping.
std::pair<int, int> defaultTxPassbandForMode(const QString& mode) noexcept
{
    const QByteArray utf8 = mode.toUtf8();
    return AetherSDR::hl2::defaultTxPassbandForModeName(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
}

// Phase-1 data-plane payload: a raw little-endian float32 array. RadioModel's
// relay decodes it; the binary step-4 frame format supersedes this later.
QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

Hl2Backend::Hl2Backend(QObject* parent) : IRadioBackend(parent)
{
    // THE DEFAULT CONTROL LAW, set here rather than as a member initialiser
    // because its sampling-bias budget is a logarithm of the gate period and
    // therefore not a constant expression. See installDefaultAutoGainLaw.
    //
    // Setting a law is not arming one: m_autoRfGainEnabled stays false, no
    // stream starts, and nothing about this installation's behaviour changes
    // until an operator switches the control on.
    installDefaultAutoGainLaw();

    // No parent: moveToThread() refuses an object that has one, and both of
    // these belong on the I/O thread rather than the GUI thread. They are
    // destroyed explicitly in the destructor after the thread is joined.
    m_metis = new MetisClient(nullptr);
    m_txDsp = new Hl2TxDsp(nullptr);
    // One receiver's state exists from construction so mode, passband and AGC
    // pushed before connect are kept; DSP chains are built in connectRadio()
    // once the receiver count is known. buildReceivers() keeps the state.
    m_ids.reset(1);
    m_rx.assign(1, Receiver{});

    // Transmit availability, decided once so it cannot change under a key.
    // Interactive runs can transmit; automation runs defer to the bridge's TX
    // gate (AETHER_AUTOMATION_ALLOW_TX) rather than an HL2-specific variable.
    const bool automation = qEnvironmentVariableIsSet("AETHER_AUTOMATION");
    // The bridge's TX gate has TWO sources and both must be honoured, or this
    // backend disagrees with the layer the operator actually configured:
    // AutomationServer::start() reads the env var, and MainWindow applies the
    // persisted GUI toggle through setTxAllowed(). Checking only the env var
    // meant the bridge would accept a key, log "key ptt ON" and return ok:true
    // while nothing keyed — a silent disagreement, which is worse than either
    // answer on its own.
    const bool automationAllowsTx =
        qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX")
        || AutomationBridgeSettings::txAllowed();
    m_txAllowed = !automation || automationAllowsTx;
    if (m_txAllowed) {
        m_metis->enableTransmit(true);
        qInfo() << "Hl2Backend: transmit available"
                << (automation ? "(automation, ALLOW_TX)" : "(interactive)");
    } else {
        qInfo() << "Hl2Backend: transmit BLOCKED — automation bridge active "
                   "without AETHER_AUTOMATION_ALLOW_TX";
    }

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("hl2-io"));
    m_metis->moveToThread(m_ioThread);
    m_txDsp->moveToThread(m_ioThread);
    m_ioThread->start();

    // The rate-change build thread — see the member declarations. Started here
    // and never restarted: it is idle except during a pan-bandwidth crossing,
    // and creating it lazily would put a thread start inside the one path whose
    // whole purpose is to not make the operator wait.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("hl2-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    m_dspBuildThread->start();

    // THE UNKEY HOLD. Single-shot and parented here, so it lives and fires on
    // this object's thread — the same thread applyKeying() runs on, which is
    // what lets a re-key stop it with a plain call and no lock.
    m_unkeyUnmuteTimer = new QTimer(this);
    m_unkeyUnmuteTimer->setSingleShot(true);
    connect(m_unkeyUnmuteTimer, &QTimer::timeout, this, [this] {
        // BELT AND BRACES WITH THE STOP IN applyKeying(). A re-key inside the
        // window stops this timer, so reaching here while keyed should be
        // impossible — but "should be impossible" is how a hold turns into an
        // unmute in the middle of a transmission, and this costs one test.
        if (m_keyed && !m_txMonitor) {
            return;
        }
        applyRxAudioMute(false);
    });

    m_cwHangTimer = new QTimer(this);
    m_cwHangTimer->setSingleShot(true);
    connect(m_cwHangTimer, &QTimer::timeout, this, [this] {
        if (!m_cwAutoKeyed) {
            return;
        }
        m_cwAutoKeyed = false;
        const TxCoordinator::Completion completion = std::exchange(m_cwHangCompletion, {});
        // applyKeying() directly with cwBreakIn true (setKeying() hard-codes
        // false), so the QSK branch in applyKeying() sees this unkey. On the
        // wire an unkey is identical either way: MetisClient::setMoxImpl()
        // reads cwBreakIn only when keying.
        applyKeying(false, m_cwHangOperation, completion, /*cwBreakIn=*/true);
    });

    // Raw IQ -> the per-receiver DSP chains. ONE connection for every receiver,
    // rather than one per receiver, because the demux has already happened at
    // the wire: blocks[i] is DDC i. Fanning out here keeps the sample path a
    // direct call on the I/O thread (no queue, no GUI thread) and means adding a
    // receiver does not add a connection that could be missed on a rebuild.
    connect(m_metis, &MetisClient::iqBlocksReady, this,
            [this](const std::vector<std::vector<std::complex<float>>>& blocks) {
        // m_ioDsps, NOT m_rx. This lambda is a DirectConnection from a signal
        // emitted by m_metis, which lives on the I/O thread — so this body runs
        // THERE, and m_rx belongs to the GUI thread. See publishIoDsps().
        const std::size_t n = std::min(blocks.size(), m_ioDsps.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (m_ioDsps[i])
                m_ioDsps[i]->processIqBlock(blocks[i]);
        }
    }, Qt::DirectConnection);

    // EP6 interleaves every active DDC. Invalidate each partial FFT before
    // the discontinuous datagram's samples arrive, including rewinds and
    // duplicates. Like iqBlocksReady above, this runs on the I/O thread and
    // uses its published DSP list (not the GUI thread's m_rx).
    connect(m_metis, &MetisClient::rxSequenceGap, this, [this](quint32) {
        for (auto* dsp : m_ioDsps) {
            if (dsp) {
                dsp->onSequenceGap();
            }
        }
    }, Qt::DirectConnection);

    // Link lifecycle: first EP6 -> connected; stop -> disconnected.
    connect(m_metis, &MetisClient::linkUp, this, [this] {
        m_connected = true;
        // #5594 (M1): seed the announcement baseline at the connect edge. The
        // connect itself republishes capabilities through connectionStateChanged,
        // so this value is already described — recording it here is what stops
        // the first zoom that does NOT move the ceiling from announcing anyway.
        m_ceilingAnnouncer.seed(receiverCeiling());
        // Started here rather than in connectRadio(): before the first EP6 there
        // is no link to describe, and ticking through the connect attempt would
        // publish a "reported" snapshot of zeros that reads as a dead link
        // rather than as one that has not come up yet.
        m_link = LinkStats{};
        m_linkRxPacketsAtLastTick = 0;
        // Fresh session: the stall clock must start now. Left over from a
        // previous connection it would read minutes, and the first poll-state
        // tick of a healthy new stream would declare it stalled.
        m_rxPacketsAtLastAdvance = 0;
        m_rxAdvanceClock.restart();
        // The bandscope belongs to the session: start() comes up with
        // wide_spectrum clear and ep4_seq_no restarted. A linkUp without
        // start() (EP6 resumed after a watchdog linkDown) also holds, because
        // the silence watchdog ends the gate's intent too.
        resetBandscopeMirrors();
        m_linkStatsTimer->start();
        emit connected();
        // Publish slice/pan state after connected(): RadioModel::onConnected()
        // wipes earlier emissions. Order: pans first, since RadioModel drops
        // pushInitialState()'s zoom limits for a pan that does not exist yet
        // (only panCenterBandwidthChanged materialises it); then
        // pushInitialState() derives passbands (#4484); then slice state.
        // emitPanState() reads nothing pushInitialState() changes.
        emitAllPanState();
        pushInitialState();
        emitAllSliceState();
        defineMeters();
        // Tell the IO board where we came up. applyBandFilter() is NOT called on
        // this path — the connect-time filter byte is primed straight into
        // MetisClient::Params instead — so without this the board would hold
        // whatever the last session left it, and an amplifier would stay on that
        // band until the operator's first retune. Placed after pushInitialState()
        // so the receiver frequencies it reads are the restored ones.
        applyIoBoardFrequency();
        // At connect there is one receiver, so this is always "not wide" — but
        // it is published rather than assumed, so the indicator starts from a
        // stated value instead of whatever the widget happened to hold.
        publishWideState();
        // Restore the auto-gain switch last: setAutoRfGain() checks the
        // baseline against kAutoRfGainMaxBaselineDb, and the restored baseline
        // reaches m_lnaGainDb only in pushInitialState(). A restored baseline is
        // clamped to the native range, so it arms; a refusal would be logged.
        if (m_autoRfGainWanted && !m_autoRfGainEnabled) {
            setAutoRfGain(true);
        } else {
            // The loop can already be running: MetisClient re-emits linkUp when
            // EP6 resumes after a silence, with no connect in between. The link
            // edge ended the bandscope gate and this object's claim on it, so
            // ask again. A no-op for a loop that is off and for a law that does
            // not read the bandscope.
            applyBandscopeForAutoGain();
        }
    });
    connect(m_metis, &MetisClient::linkDown, this, [this] {
        // The PA temperature pole belongs to the SESSION. Left standing, the
        // first sample of the next stream would be averaged against a reading
        // from before the drop — kPaTempAlpha would drag a cold radio toward a
        // temperature it had an hour ago, and the row would take several
        // samples to tell the truth. Clearing it makes the next reading seed
        // the filter instead of blending with history.
        m_havePaTemp = false;
        m_paTempC = 0.0;
        if (m_connected) {
            m_connected = false;
            m_ceilingAnnouncer.reset();   // #5594 (M1): re-seeded on the next connect
            m_linkStatsTimer->stop();
            resetIoBoardSchedule();
            resetBandscopeMirrors();
            emit disconnected();
        }
    });
    // F4 (#4448): the radio never sent EP6 within the connect deadline — off,
    // unreachable, or already streaming to another client. Surface it as a
    // connection error and stop the Metis client so it does not sit half-open
    // paying out C&C at a radio that will never answer.
    connect(m_metis, &MetisClient::connectFailed, this, [this](const QString& reason) {
        invalidateTxDspConfiguration();
        // This handler runs on the MAIN thread (queued from the io thread), but
        // m_metis lives on the io thread — stop() touches its socket and timers,
        // so it must run THERE, not here. A direct call is the affinity bug the
        // destructor also guards against.
        QMetaObject::invokeMethod(m_metis, "stop", Qt::QueuedConnection);
        m_connected = false;
        m_ceilingAnnouncer.reset();   // #5594 (M1): re-seeded on the next connect
        m_linkStatsTimer->stop();
        resetIoBoardSchedule();
        resetBandscopeMirrors();
        emit connectionError(QStringLiteral("Hermes-Lite 2: %1").arg(reason));
    });

    // Per-receiver DSP outputs are wired in buildReceivers(), because the
    // receivers do not exist yet. Everything below is radio-wide.
    //
    // Modulated IQ -> the wire. Both live on the I/O thread, so this is a direct
    // call and the transmit path never touches the GUI thread.
    connect(m_txDsp, &Hl2TxDsp::iqReady, m_metis,
            [this](const std::vector<std::complex<float>>& iq, const TxCoordinator::Context& context) {
        m_metis->queueTxIq(iq, context);
    });
    connect(m_txDsp, &Hl2TxDsp::micPeak, this,
            [this](float dbfs) {
        emit meterUpdate(QStringLiteral("TX:MICPEAK"), dbfs);
        // Loudest thing the operator said this transmission. Evaluated at unkey
        // (setKeying) against the ALC's target peak — a PER-BLOCK test would
        // fire on every normal transmission, because the pauses between words
        // sit far below the target by definition. The MAXIMUM across the over
        // is the only reading that answers "did any of this modulate".
        if (m_keyed)
            m_txMicPeakMaxDbfs = std::max(m_txMicPeakMaxDbfs, dbfs);
    });
    // The post-ALC transmit peak — the level the modulator is actually handing
    // to the wire.
    //
    // MeterModel's TX:ALC is a LEVEL in dBFS, not a gain, which is why this is
    // fed from alcPeak() and not from the alcGain() signal sitting next to it.
    // alcGain answers "how hard is the ALC working"; the gauge asks "how close
    // to full modulation am I", and on a chain whose ALC targets 0.85 those two
    // move in opposite directions.
    connect(m_txDsp, &Hl2TxDsp::alcPeak, this, [this](float dbfs) {
        m_alcPeakDbfs = dbfs;
        emit meterUpdate(QStringLiteral("TX:ALC"), dbfs);
    });
        // The gain the ALC applies, a separate meter from TX:ALC (a level):
        // they move in opposite directions. Mirrored into m_alcGainDb for
        // healthSnapshot() and published as TX:ALCGAIN for MeterModel.
    connect(m_txDsp, &Hl2TxDsp::alcGain, this, [this](float db) {
        m_alcGainDb = db;
        emit meterUpdate(QStringLiteral("TX:ALCGAIN"), db);
    });
    // The modulator's own copy of the mic gain, for healthSnapshot(). Reported
    // ALONGSIDE m_micLevel rather than instead of it: the operator's request and
    // the modulator's state are different facts, and a diagnosis needs to see
    // them disagree. See Hl2TxDsp::micGainChanged.
    connect(m_txDsp, &Hl2TxDsp::micGainChanged, this,
            [this](double linear) { m_appliedMicGainLinear = linear; });

    connect(m_metis, &MetisClient::telemetryUpdated, this,
            [this](const Hl2Telemetry& t) { publishTelemetry(t); });

    // Stream-free telemetry is owned by RadioModel (setTelemetryService()) so
    // it works with no backend; this timer tells it what the IQ path is doing.
    // Never stopped, unlike the link-stats timer, which stops in exactly the
    // cases the poller exists for. hl2_telemetry_wire_test pins its effect.
    auto* pollStateTimer = new QTimer(this);
    pollStateTimer->setInterval(kTelemetryPollStateIntervalMs);
    connect(pollStateTimer, &QTimer::timeout, this, &Hl2Backend::updateTelemetryPollState);
    pollStateTimer->start();
    // Mirror the drop counter onto this thread so healthSnapshot() can read it
    // without touching an object that lives on the I/O thread.
    connect(m_metis, &MetisClient::dropsUpdated, this,
            [this](quint64 drops) { m_drops = drops; });
    // Same mirror, for the transport counters linkStats() reports. The wire
    // shape is translated to the seam shape HERE, so linkStats() is a plain
    // read of a value that already lives on the reader's thread.
    connect(m_metis, &MetisClient::linkCountersUpdated, this,
            [this](const MetisClient::LinkCounters& c) {
        // `reported` and `alive` are deliberately NOT set here — they are
        // statements about the connection and the tick, not about the counters,
        // and both publishers below own them.
        m_link.rxBytes = static_cast<qint64>(c.rxBytes);
        m_link.txBytes = static_cast<qint64>(c.txBytes);
        // The stall clock is restarted HERE, in the mirror, and only on an
        // actual advance -- linkCountersUpdated fires on its own cadence whether
        // or not EP6 moved, so "a publish arrived" is not "packets arrived".
        if (c.rxPackets != m_rxPacketsAtLastAdvance) {
            m_rxPacketsAtLastAdvance = c.rxPackets;
            m_rxAdvanceClock.restart();
        }
        m_link.rxPackets = c.rxPackets;
        m_link.rxPacketsLost = c.drops;
        // Protocol 1 is a one-way stream with no request/response exchange: EP2
        // goes out on a wall clock and EP6 comes back free-running, and no frame
        // in either direction answers a specific frame in the other. There is
        // therefore no round trip to time, and -1 says exactly that rather than
        // presenting a 0 that every readout formats as "< 1 ms".
        m_link.rttMs = -1;
        m_link.gapMs = c.meanGapMs;
        m_link.gapMaxMs = c.maxGapMs;
        // Jitter as the SPREAD of delivery within the window. On a stream with
        // no round trip this is the honest latency-variation figure: a healthy
        // link delivers on a metronome and reads a fraction of a millisecond,
        // while a congested one stalls and resumes — which is precisely what the
        // operator hears. Undefined until a window has closed with samples in it.
        m_link.jitterMs = (c.maxGapMs >= 0 && c.meanGapMs >= 0)
                              ? c.maxGapMs - c.meanGapMs
                              : -1;
        m_link.localEndpoint = c.localEndpoint;
        // The bandscope's counters ride this same publish rather than a signal
        // or a timer of their own — the point of putting them on LinkCounters.
        // They stay off LinkStats: see the members' comment in the header.
        // The GATE's state as the client has it, not the request this backend
        // made. See LinkCounters::bandscopeEnabled.
        m_bandscopeEnabled = c.bandscopeEnabled;
        m_ep4Packets = c.ep4Packets;
        m_ep4Drops = c.ep4Drops;
        m_ep4Rewinds = c.ep4Rewinds;
        m_ep4Blocks = c.bandscopeBlocks;
        m_ep4Timeouts = c.bandscopeTimeouts;
        // The silence watchdog's recovery record. It rides this publish for the
        // reason the bandscope's counters do -- no signal and no timer of its
        // own -- and it has to ride SOMETHING, because a recovery that works is
        // invisible by construction: m_linkUp never drops, so no linkDown and
        // no linkUp fires and nothing republishes. These two numbers are the
        // whole trace it leaves.
        m_silenceRecoveryAttempts = c.silenceRecoveryAttempts;
        m_silenceRecoveriesCompleted = c.silenceRecoveriesCompleted;
    });

    // ONE ACCEPTED BANDSCOPE BLOCK, mirrored onto the GUI thread. The same
    // pattern as m_drops and m_link above and for the same reason: MetisClient
    // lives on the hl2-io thread.
    //
    // It arrives about once a second — the gate's duty cycle, not the packet
    // rate — so it needs no throttle of its own, and it drives NOTHING. There
    // is no consumer but healthSnapshot()'s rows, which IRadioBackend.h binds
    // to display.
    connect(m_metis, &MetisClient::bandscopeBlockReady, this,
            [this](const AetherSDR::hl2::Ep4Stats& block) {
        m_bandscopeBlock = block;
        m_bandscopeBlockClock.restart();
    });

    // The on-demand frame's two answers. Both consume the outstanding request
    // id, so exactly one of extensionResult / extensionError goes out per
    // bandscope.frame call and the next caller is not blocked by a request
    // that was already answered.
    connect(m_metis, &MetisClient::bandscopeFrameReady, this,
            [this](const QList<float>& samples) {
        const quint64 id = m_bandscopeFrameRequest;
        m_bandscopeFrameRequest = 0;
        if (id == 0)
            return;   // the request was already failed out from under us
        emit extensionResult(id, QVariantMap{
            {QStringLiteral("samples"), QVariant::fromValue(samples)},
            // The converter's sample rate, so the consumer does not have to
            // know the gateware's clock to label an axis. First Nyquist zone:
            // the 2048-point transform of this spans DC..38.4 MHz.
            {QStringLiteral("sampleRateHz"), AetherSDR::hl2::kAdcSampleRateHz},
            // UNCALIBRATED AND PRE-DDC — carried in the reply so a consumer
            // cannot claim otherwise by omission. Nothing has compared these
            // levels against a real band (the study's Procedure C, which needs
            // a live antenna and is not scheduled).
            {QStringLiteral("calibrated"), false},
        });
    });
    connect(m_metis, &MetisClient::bandscopeFrameFailed, this,
            [this](const QString& reason) {
        const quint64 id = m_bandscopeFrameRequest;
        m_bandscopeFrameRequest = 0;
        if (id != 0)
            emit extensionError(id, reason);
    });

    m_linkStatsTimer = new QTimer(this);
    m_linkStatsTimer->setInterval(kLinkStatsIntervalMs);
    connect(m_linkStatsTimer, &QTimer::timeout, this, &Hl2Backend::publishLinkStats);
}

void Hl2Backend::publishLinkStats()
{
    // Fresh packets since the last tick: the heartbeat's proof of life.
    // Computed here because it consumes the previous value, and stored on
    // m_link so the linkStats() getter (the `liveness` verb) sees it too.
    m_link.alive = m_connected && m_link.rxPackets != m_linkRxPacketsAtLastTick;
    m_linkRxPacketsAtLastTick = m_link.rxPackets;
    LinkStats s = m_link;
    // The link is REPORTED from the moment we are connected, even before the
    // first counter snapshot has crossed from the I/O thread. Otherwise the
    // consumer's first tick sees reported=false, keeps its Flex sources, and
    // renders the blank readout this whole path exists to fix.
    s.reported = true;
    emit linkStatsUpdated(s);
}

Hl2LinkState Hl2Backend::telemetryLinkState() const
{
    // How long the EP6 counter has sat still. An invalid clock means nothing has
    // ever advanced it; while connected that is a stream which never came up,
    // which is a stall by any reading, so it is reported as one rather than
    // being excused as "no data yet".
    const long long sinceAdvance =
        m_rxAdvanceClock.isValid() ? m_rxAdvanceClock.elapsed() : kStreamStallDeclareMs;

    // Held by another client, from the latest discovery reply's streaming bit
    // (status byte 0x03). Only meaningful while we are not streaming, since
    // our own session sets it too.
    std::optional<DiscoveryReply> reply;
    if (m_telemetryService)
        reply = m_telemetryService->lastReply();
    const bool heldByOther =
        m_pollTargetHeldByOther || (!m_connected && reply && reply->streaming);

    return hl2LinkStateFor(m_connected, heldByOther, sinceAdvance);
}

void Hl2Backend::updateTelemetryPollState()
{
    if (!m_telemetryService)
        return;

    // The link state. Demand belongs to the service: it is recorded by the
    // health read itself, which every consumer performs and none can forget.
    const Hl2LinkState state = telemetryLinkState();
    m_telemetryService->setLinkState(state);

    // Publish RAD:PATEMP from the poller when in-band telemetry is not live,
    // so the needle does not freeze. Raw, not fed into m_paTempC, whose filter
    // belongs to the in-band session. With no reading at all the needle still
    // holds (the meter seam has no "unknown"); the health row shows the gap.
    if (state == Hl2LinkState::Streaming)
        return;   // in-band owns the meter whenever it is live
    const std::optional<DiscoveryReply> reply = m_telemetryService->lastReply();
    if (reply && reply->temperatureRaw)
        emit meterUpdate(QStringLiteral("RAD:PATEMP"),
                         hl2TemperatureCelsius(*reply->temperatureRaw));
}

void Hl2Backend::setTelemetryPollTarget(const QHostAddress& addr, bool heldByOther)
{
    m_pollTargetHeldByOther = heldByOther;
    if (m_telemetryService)
        m_telemetryService->setTarget(addr);
    updateTelemetryPollState();
}

IRadioBackend::LinkStats Hl2Backend::linkStats() const
{
    LinkStats s = m_link;
    s.reported = m_connected;
    return s;
}

Hl2Backend::Receiver* Hl2Backend::rx(int ddc)
{
    if (ddc < 0 || ddc >= static_cast<int>(m_rx.size()))
        return nullptr;
    return &m_rx[static_cast<std::size_t>(ddc)];
}

const Hl2Backend::Receiver* Hl2Backend::rx(int ddc) const
{
    if (ddc < 0 || ddc >= static_cast<int>(m_rx.size()))
        return nullptr;
    return &m_rx[static_cast<std::size_t>(ddc)];
}

int Hl2Backend::ddcForSlice(int sliceId) const
{
    const auto* ids = m_ids.byUi(sliceId);
    return ids ? ids->ddcIndex : -1;
}

int Hl2Backend::ddcForPan(const QString& panId) const
{
    // An EMPTY pan id addresses the first receiver. Some seam callers omit it
    // for a single-pan radio, and refusing those would break controls that
    // worked before this became a multi-pan backend.
    if (panId.isEmpty())
        return m_rx.empty() ? -1 : 0;
    const auto* ids = m_ids.byPanId(panId);
    return ids ? ids->ddcIndex : -1;
}

void Hl2Backend::buildReceivers(int count)
{
    if (count < 1)
        count = 1;

    // STATE SURVIVES, DSP CHAINS DO NOT.
    //
    // The two have different lifetimes and conflating them is a bug in both
    // directions. A receiver's mode, passband, AGC and frequency are set by the
    // operator and by RadioModel's initial push, and some of that arrives BEFORE
    // a radio is connected — so wiping it here would silently discard, for
    // example, the mode the session is meant to come up in. The DSP chain, in
    // contrast, owns a WDSP channel from a shared pool and must be torn down and
    // rebuilt, or a reconnect leaks channel ids until the pool is exhausted.
    releaseReceiverDsps();
    const auto previous = m_rx;   // state only; every .dsp in here is now null
    // Whether there WAS state to carry, for callers that have to tell a rebuild
    // apart from a build. connectRadio()'s AGC seeding is the one that needs it:
    // "same radio reconnecting" and "same radio after tearDownReceivers()" are
    // indistinguishable by serial, and only the second must be re-seeded.
    m_rxCarriedState = !previous.empty();

    m_ids.reset(count);
    m_rx.assign(static_cast<std::size_t>(count), Receiver{});

    for (int i = 0; i < count; ++i) {
        Receiver& r = m_rx[static_cast<std::size_t>(i)];
        const std::size_t ui = static_cast<std::size_t>(i);
        if (ui < previous.size()) {
            r = previous[ui];        // this receiver existed; keep what it held
        } else if (!previous.empty()) {
            // A receiver that did not exist before inherits the FIRST one's
            // settings rather than construction defaults. Starting them on the
            // same frequency is deliberate: parked at 0 Hz they would draw
            // panadapters of DC and read as a hardware fault on first connect.
            r = previous.front();
        }
        r.dsp = nullptr;             // never inherited; recreated below
        r.audioMuted = false;
        r.sMeter.reset();

        std::string err;
        if (!openReceiverDsp(i, &err)) {
            qCWarning(lcHl2) << "HL2: could not create receiver" << i << "—"
                             << QString::fromStdString(err);
        }
    }
    // The set is final; hand the sample path its copy. Once per rebuild rather
    // than once per receiver: an intermediate list would describe a set that
    // never actually ran.
    publishIoDsps();
    qCInfo(lcHl2) << "HL2: running" << count << "receiver(s)";
}

bool Hl2Backend::openReceiverDsp(int ddc, std::string* error)
{
    Receiver* r = rx(ddc);
    const auto* ids = m_ids.byDdc(ddc);
    if (!r || !ids) {
        if (error) *error = "no such receiver";
        return false;
    }

    // Created and wired HERE, recorded in m_rx at the END of this function, and
    // not handed to the sample path at all — the CALLER does that with
    // publishIoDsps(), once it has configured the chain. So a receiver is never
    // fed before its WDSP channel exists.
    auto* dsp = new Hl2RxDsp(nullptr);   // no parent: moveToThread refuses one
    dsp->moveToThread(m_ioThread);

    // Capture the UI number, not the DDC index: closing a receiver renumbers
    // the DDCs after it (the gateware streams numRx contiguous slots), while
    // the UI number never changes (Hl2ReceiverMap::remove).
    const int ui = ids->uiNumber;

    connect(dsp, &Hl2RxDsp::spectrumReady, this,
            [this, ui](const std::vector<float>& bins) {
        if (!m_ids.byUi(ui))
            return;
        // dBFS -> dBm through the shared reference (one AD9866 behind every
        // DDC): derived full scale (+3 dB) minus LNA gain, so the trace holds
        // still across gain changes. off == 0.0 fires only when the LNA gain
        // equals the full-scale figure.
        const double off = m_dbRef.offsetDb();
        if (off == 0.0) {
            emit spectrumFrameReady(ui, floatBytes(bins));
            return;
        }
        std::vector<float> dbm(bins.size());
        for (std::size_t i = 0; i < bins.size(); ++i)
            dbm[i] = static_cast<float>(bins[i] + off);
        emit spectrumFrameReady(ui, floatBytes(dbm));
    });

    connect(dsp, &Hl2RxDsp::audioReady, this,
            [this, ui, producer = QPointer<Hl2RxDsp>(dsp)](const std::vector<float>& pcm) {
        const auto* ids = m_ids.byUi(ui);
        const Receiver* receiver = ids ? rx(ids->ddcIndex) : nullptr;
        if (!producer || !receiver || receiver->dsp != producer.data()) {
            return;
        }
        // THIS SLICE's audio, before the mixer touches it. Per-slice consumers
        // (a TCI receiver channel, a decoder) need one slice's audio and cannot
        // un-mix the speaker sum. Pre-mute and pre-gain on purpose — see the
        // signal's comment: muting a slice must not stop WSJT-X decoding on it.
        //
        // Emitted even while keyed. The mixer drops keyed audio for the speaker
        // (we hear our own transmitter), but a per-slice consumer decides that
        // for itself, and the TX path already mutes the demodulator.
        if (!publishLegacySliceAudio(ids->uiNumber, floatBytes(pcm))) {
            return; // malformed PCM must not enter the stateful speaker mixer
        }

        mixReceiverAudio(ids->ddcIndex, pcm);
    });

    connect(dsp, &Hl2RxDsp::meterUpdate, this,
            [this, ui](float dbfs) {
        const auto* ids = m_ids.byUi(ui);
        Receiver* r = ids ? rx(ids->ddcIndex) : nullptr;
        if (!r)
            return;
        // Same reference as the spectrum -- a meter that moved on a gain
        // change while the trace stayed put would be its own kind of lie.
        const double dbm = m_dbRef.toDbm(dbfs);

        // Smooth EVERY sample, publish only on the tick -- SMeterSmoother,
        // per receiver so a strong signal on one does not drive another's
        // needle.
        if (const auto out = r->sMeter.feed(dbm))
            emit meterUpdate(sliceMeterName(ui), *out);
    });

    // The name the lambda above publishes under has to EXIST as a definition or
    // MeterModel has nowhere to put the value. Declared here rather than in
    // defineMeters() because this runs once per receiver, at both connect and
    // add, so the catalogue describes the receivers that are actually running.
    // A no-op for receiver 0, whose meter defineMeters() owns.
    defineSliceLevelMeter(ui);

    // Recorded, not published. m_rx is this thread's, so this is a plain store;
    // the sample path sees nothing until the caller calls publishIoDsps().
    r->dsp = dsp;
    return true;
}

int Hl2Backend::receiverCeiling() const
{
    // Two independent limits and the smaller wins. Neither may be assumed: the
    // shipping gateware reports 4 at discovery byte 0x13 and the skimmer builds
    // report 9-12, while the link budget depends on the span the operator is
    // currently running.
    MetisClient::Params p;
    p.numRx = kMaxReceivers;
    p.boardMaxRx = m_boardMaxRx;
    const int board = MetisClient::effectiveNumRx(p);
    return std::min(board, maxReceiversAtRate(m_sampleRateHz, board));
}

void Hl2Backend::announceReceiverCeilingRevision()
{
    // Disconnected, the ceiling reported by capabilities() is not receiverCeiling()
    // at all (it falls back to the receiver count), and the connect/disconnect
    // edges already republish capabilities on their own. Nothing to announce.
    if (!m_connected)
        return;
    if (m_ceilingAnnouncer.shouldAnnounce(receiverCeiling()))
        emit capabilitiesChanged();
}

bool Hl2Backend::createPanadapter()
{
    if (!m_connected) {
        qCWarning(lcHl2) << "HL2: cannot add a receiver before the radio is connected";
        return false;
    }
    const int running = m_ids.size();
    const int ceiling = receiverCeiling();
    if (running >= ceiling) {
        // Say WHICH limit was hit. "Limit reached" on a 4-receiver board that is
        // only allowed 3 because the operator zoomed out to 384 kHz is the kind
        // of message that sends someone hunting for a hardware fault.
        qCWarning(lcHl2).nospace()
            << "HL2: cannot add a receiver — running " << running << " of " << ceiling
            << " (board reports " << (m_boardMaxRx > 0 ? QString::number(m_boardMaxRx)
                                                       : QStringLiteral("unknown"))
            << ", link budget allows " << maxReceiversAtRate(m_sampleRateHz, kMaxReceivers)
            << " at " << m_sampleRateHz / 1000 << " kHz)";
        return false;
    }

    const int ddc = m_ids.append();

    Receiver seed;
    if (!m_rx.empty()) {
        const Receiver& first = m_rx.front();
        seed.sliceFreqHz = first.sliceFreqHz;
        seed.ncoHz = first.ncoHz;
        seed.mode = first.mode;
        seed.filterLowHz = first.filterLowHz;
        seed.filterHighHz = first.filterHighHz;
        seed.agcMode = first.agcMode;
        seed.agcThresholdDb = first.agcThresholdDb;
    }
    m_rx.push_back(seed);

    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::QueuedConnection,
            Q_ARG(int, static_cast<int>(m_rx.size())));
    }

    std::string err;
    if (!openReceiverDsp(ddc, &err)) {
        qCWarning(lcHl2) << "HL2: receiver" << ddc << "could not be created —"
                         << QString::fromStdString(err);
        m_rx.pop_back();   // never published, so nothing to withdraw
        m_ids.remove(ddc);
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::QueuedConnection,
                Q_ARG(int, static_cast<int>(m_rx.size())));
        }
        return false;
    }

    const Hl2ReceiverIds* const opened = m_ids.byDdc(ddc);

    // TCI and layout callers discover the new receiver before this call returns.
    emitPanState(ddc);
    emitSliceState(ddc);
    if (opened) {
        emit panBandwidthLimitsChanged(
            opened->panId,
            static_cast<double>(kIqSampleRatesHz[0]) / 1.0e6,
            static_cast<double>(m_sampleRateHz) / 1.0e6);
        emit panRfGainInfoChanged(opened->panId, kLnaGainMinDb, kLnaGainMaxDb,
                                  kLnaGainStepDb);
        // EFFECTIVE, not baseline: a pan created while an automatic control is
        // holding gain down must come up showing the same number as its
        // siblings, not the operator's untouched baseline (Hl2GainSplit.h).
        emit panRfGainChanged(opened->panId, lnaEffectiveDb());
    }
    // A new receiver can change whether the set spans bands.
    applyBandFilter("add receiver");
    publishWideState();

    startReceiverDspBuild(opened ? opened->uiNumber : -1);

    return true;
}

void Hl2Backend::startReceiverDspBuild(int uiNumber)
{
    const Hl2ReceiverIds* const ids = m_ids.byUi(uiNumber);
    Receiver* const r = ids ? rx(ids->ddcIndex) : nullptr;
    if (!r || !r->dsp) {
        qCWarning(lcHl2) << "HL2: no receiver behind UI" << uiNumber
                         << "to build a DSP chain for";
        return;
    }

    Hl2RxDsp::Config config;
    config.inputSampleRateHz = m_rateLedger.committed();
    config.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
    config.mode = modeFromString(r->mode);
    std::tie(config.filterLowHz, config.filterHighHz) = dspFilterHz(*r);
    config.agcMode = wdspAgcMode(r->agcMode);
    config.maximumAgcGainDb = m_dbRef.agcCeilingDb(r->agcThresholdDb);

    // Rate reconciliation must leave this DSP alone until its own build completes.
    r->dspBuildInFlight = true;

    // A UI number can be recycled during any of the queued hops.
    const quint64 generation = ++m_nextDspBuildGeneration;
    r->dspBuildGeneration = generation;

    Hl2RxDsp* const dsp = r->dsp;
    MetisClient* const metis = m_metis;

    if (!metis || !m_dspBuildContext || !m_ioThread || !m_ioThread->isRunning()
        || QThread::currentThread() == m_ioThread) {
        // NO THREADS TO SPLIT ACROSS. Before the I/O thread starts, after it is
        // joined, or called from it — the same four conditions publishIoDspList()
        // tests, and for the same reason: a queued hop into a dead event loop
        // never arrives and one into your own deadlocks. Build inline instead of
        // leaving a receiver with no chain. Nothing is streaming in any of these.
        std::string error;
        const bool ok = dsp->configure(config, &error);
        finishReceiverDspBuild(uiNumber, generation, ok,
                               ok ? dsp->wdspChannelId() : -1,
                               config.inputSampleRateHz, error);
        return;
    }

    QMetaObject::invokeMethod(dsp, [this, metis, dsp, config, uiNumber, generation] {
        // Mirror control changes during the build; installation replays them.
        dsp->beginRebuild(config);
        const bool nbOn = dsp->noiseBlankerEnabled();
        const int nbLevel = dsp->noiseBlankerLevel();
        QPointer<Hl2RxDsp> guard(dsp);

        QMetaObject::invokeMethod(m_dspBuildContext, [this, metis, guard, config,
                                                      uiNumber, generation,
                                                      nbOn, nbLevel] {
            Hl2RxDsp::RebuildResult built =
                Hl2RxDsp::buildChannel(config, nbOn, nbLevel);

            QMetaObject::invokeMethod(metis, [this, guard, config, uiNumber,
                                              generation,
                                              b = std::move(built)]() mutable {
                bool ok = false;
                int channelId = -1;
                std::string error = b.error;
                // Check QPointer on the I/O thread that owns and deletes the DSP.
                if (!guard) {
                    error = "the receiver was closed while its chain was building";
                } else {
                    ok = guard->installRebuiltChannel(std::move(b));
                    channelId = guard->wdspChannelId();
                }

                QMetaObject::invokeMethod(this, [this, uiNumber, generation, ok,
                                                 channelId,
                                                 rate = config.inputSampleRateHz,
                                                 error] {
                    finishReceiverDspBuild(uiNumber, generation, ok, channelId,
                                           rate, error);
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Hl2Backend::finishReceiverDspBuild(int uiNumber, quint64 generation, bool ok,
                                        int channelId, int builtRateHz,
                                        const std::string& error)
{
    const Hl2ReceiverIds* ids = m_ids.byUi(uiNumber);
    Receiver* r = ids ? rx(ids->ddcIndex) : nullptr;
    if (!r) {
        // Closed while it built, and nothing has taken its number yet.
        // installRebuiltChannel() was never reached (the QPointer was already
        // null) or the chain it installed is already queued for deletion on the
        // I/O thread — either way there is nothing here to finish and nothing to
        // withdraw.
        qCInfo(lcHl2) << "HL2: receiver UI" << uiNumber
                      << "was closed before its DSP chain finished building";
        return;
    }
    if (r->dspBuildGeneration != generation) {
        qCInfo(lcHl2) << "HL2: discarding a DSP build completion for UI" << uiNumber
                      << "generation" << generation
                      << "— the DSP owning that build has retired"
                      << "(the receiver holding it now is on generation"
                      << r->dspBuildGeneration << ")";
        return;
    }
    const int ddc = ids->ddcIndex;
    r->dspBuildInFlight = false;
    r->dspBuildGeneration = 0;

    if (!ok) {
        qCWarning(lcHl2) << "HL2: receiver" << ddc << "DSP failed —"
                         << QString::fromStdString(error);
        const QString removedPanId = ids->panId;
        const int removedUi = ids->uiNumber;
        // Retire the published meter before the DSP and UI identities disappear.
        withdrawSliceLevelMeter(removedUi);
        Hl2RxDsp* const doomed = r->dsp;
        r->dsp = nullptr;
        // WITHDRAW, THEN DESTROY. publishIoDspList() blocks until the I/O thread
        // has taken the list, so by the time the deletion below is posted the
        // fan-out has already stopped feeding this chain. This is the one
        // blocking hop left on this path, it is on the failure path only, and it
        // is the same ordering removePanadapter() documents at length.
        withdrawIoDsps();
        if (doomed) {
            doomed->disconnect(this);
            doomed->deleteLater();
        }
        // The announced slice can be selected before its build fails.
        const bool txWasHere = (m_txDdc == ddc);
        const bool activeWasHere = (m_activeDdc == ddc);
        if (txWasHere) {
            qCInfo(lcHl2) << "HL2: transmit moves from DDC" << ddc
                          << "to 0 — its receiver's DSP chain failed to build";
        }
        m_txDdc = txWasHere ? 0 : hl2RoleAfterRemove(m_txDdc, ddc);
        m_activeDdc = activeWasHere ? 0 : hl2RoleAfterRemove(m_activeDdc, ddc);
        m_rx.erase(m_rx.begin() + ddc);
        m_ids.remove(ddc);        // renumbers DDC indices; UI numbers are untouched
        m_mixPending.clear();     // the per-receiver queues describe the old set
        m_mixAccum.clear();
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::QueuedConnection,
                Q_ARG(int, static_cast<int>(m_rx.size())));
        }
        publishIoDsps();
        if (txWasHere) {
            retuneReceiver(m_txDdc);
        }
        emit sliceLifecycleFailed(QStringLiteral("create"), removedUi,
                                  QString::fromStdString(error));
        emit sliceRemoved(removedUi);
        emit panRemoved(removedPanId);
        applyBandFilter("receiver DSP build failed");
        publishWideState();
        if (txWasHere || activeWasHere) {
            emitAllSliceState();
        }
        return;
    }

    // What this chain was actually built for, so a rate change that commits
    // while it was being opened can tell that it still needs rebuilding.
    r->configuredRateHz = builtRateHz;

    if (auto* mutableIds = m_ids.mutableByDdc(ddc)) {
        mutableIds->dspChannel = channelId;
        mutableIds->analyzerId = mutableIds->uiNumber;
    }

    // Put the NCO where this receiver's state says it should be. setReceiverCount
    // starts a new receiver on RX1's frequency, which is only right if nothing
    // moved it since.
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
            Q_ARG(int, ddc),
            Q_ARG(std::uint32_t, ncoCommandHz(r->ncoHz)));
    }
    QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
        Q_ARG(double, rxShiftHz(*r)));
    // Notches are radio-wide, so a receiver that appears after them has to be
    // brought up to date. Otherwise adding a second panadapter gives you one
    // receiver with the interferer notched out and one without.
    seedNotches(*r);
    // Per-receiver, so a new panadapter starts from ITS OWN state rather than
    // inheriting RX1's — which for a receiver that has never been configured is
    // the default of off.
    pushNoiseBlanker(*r);
    pushPanAveraging(*r);
    pushSquelch(*r);

    // AND ONLY NOW does the sample path learn about it. Last, after the chain is
    // configured, tuned and shifted — so the first block it is ever handed lands
    // in a receiver that is fully set up, rather than one still being assembled.
    publishIoDsps();

    qCInfo(lcHl2) << "HL2: added receiver — DDC" << ddc << "pan" << ids->panId
                  << "WDSP channel" << channelId
                  << "; running" << m_rx.size() << "of" << receiverCeiling();


    // A rate crossing may have committed while this build ran.
    if (m_rateLedger.committed() != builtRateHz) {
        qCInfo(lcHl2) << "HL2: receiver" << ddc << "opened at" << builtRateHz
                      << "Hz but the radio committed to" << m_rateLedger.committed()
                      << "Hz while it built — rebuilding";
        startReceiverDspBuild(uiNumber);
    }
}

bool Hl2Backend::removePanadapter(const QString& panId)
{
    const int ddc = ddcForPan(panId);
    const auto* ids = m_ids.byDdc(ddc);
    if (ddc < 0 || !ids) {
        qCWarning(lcHl2) << "HL2: no receiver behind pan" << panId;
        return false;
    }
    if (m_ids.size() <= 1) {
        // A radio with no receivers is not a state worth being able to reach:
        // there would be nothing to hear, nothing to display, and no pane left
        // to reopen one from. Closing the last pan is refused, not obeyed.
        qCWarning(lcHl2) << "HL2: refusing to close the last receiver";
        return false;
    }
    // m_txDdc and the active DDC are indices, and removal renumbers every
    // index after the closed one. A role on the closing receiver must move; a
    // role after it must shift down, or it names the wrong receiver.
    const bool txMoved = (ddc == m_txDdc);
    if (txMoved) {
        // Move transmit to DDC 0 in post-removal numbering (always exists,
        // since closing the last receiver is refused), or txSlice() goes null.
        qCInfo(lcHl2) << "HL2: transmit moves from DDC" << ddc
                      << "to 0 — its receiver is closing";
        m_txDdc = 0;
    } else {
        m_txDdc = hl2RoleAfterRemove(m_txDdc, ddc);
    }

    if (ddc == m_activeDdc) {
        // Same for the active slice, and for the same reason: the client's
        // shared controls act on it, so it cannot point at a closed receiver.
        m_activeDdc = 0;
    } else {
        m_activeDdc = hl2RoleAfterRemove(m_activeDdc, ddc);
    }

    const QString removedPanId = ids->panId;
    const int removedUi = ids->uiNumber;

    // Withdraw this receiver's S-meter before its DSP goes; UI numbers are not
    // renumbered, so surviving meters keep their identity. Except receiver 0:
    // its meter is defineMeters()' def(1), declared once per session, and
    // withdrawSliceLevelMeter(0) is a no-op so a reopened UI 0 still has one.
    // Making def(1) per-receiver would also move the TX waveform meters,
    // which derive their slice context from it.
    withdrawSliceLevelMeter(removedUi);

    // Withdraw every chain (empty list) before the wire's receiver count
    // changes, then destroy. publishIoDsps() blocks until the I/O thread has
    // the list, so the fan-out has stopped feeding the doomed chain. A
    // shortened list is unsafe here: erase() shifts survivors while the wire
    // still sends the old slot count, misfeeding every receiver above.
    Hl2RxDsp* doomed = m_rx[static_cast<std::size_t>(ddc)].dsp;
    m_rx[static_cast<std::size_t>(ddc)].dsp = nullptr;
    withdrawIoDsps();
    if (doomed) {
        // deleteLater() posts the destruction to the I/O thread's event loop,
        // which is the only thread allowed to close the WDSP channel this owns.
        doomed->disconnect(this);
        doomed->deleteLater();
    }
    m_rx.erase(m_rx.begin() + ddc);
    m_ids.remove(ddc);        // renumbers DDC indices; UI numbers are untouched
    m_mixPending.clear();     // the per-receiver queues describe the old set
    m_mixAccum.clear();

    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::BlockingQueuedConnection,
            Q_ARG(int, static_cast<int>(m_rx.size())));
        // Every SURVIVING receiver may have moved down a hardware slot, and the
        // NCO registers are addressed by that slot. Re-assert all of them, or
        // the receivers after the closed one keep tuning the register that used
        // to be theirs — silently, because nothing reads a NCO back.
        for (const auto& s : m_ids.all()) {
            if (const Receiver* sr = rx(s.ddcIndex)) {
                QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
                    Q_ARG(int, s.ddcIndex),
                    Q_ARG(std::uint32_t, ncoCommandHz(sr->ncoHz)));
            }
        }
    }

    // Feed the survivors again. Unconditional and AFTER the block above: only
    // now does a shortened list describe what the wire is sending. Outside the
    // if(m_metis), because a withdrawal that is never undone is permanent
    // silence, and "no wire" must not be the one path that never recovers.
    publishIoDsps();

    qCInfo(lcHl2) << "HL2: closed receiver — pan" << removedPanId << "(UI" << removedUi
                  << "); running" << m_rx.size() << "receiver(s)";

    // BOTH, and in this order. On this backend a slice IS a receiver, so
    // closing one retires the pan and the slice together. Emitting only
    // panRemoved left the SliceModel behind naming a dead pan id, and the next
    // create then failed the capacity guard against a slice count that never
    // fell — "Slice capacity is full" with one receiver running.
    emit sliceRemoved(removedUi);
    emit panRemoved(removedPanId);
    // Closing one can take the set back onto a single band.
    applyBandFilter("close receiver");
    publishWideState();
    // Transmit moved: re-run the new owner's tune, as setTxSlice() does, so the
    // TX register and RIT follow it instead of staying on the closed receiver.
    if (txMoved) {
        retuneReceiver(m_txDdc);
    }
    // The TX slice may have moved; republish so the indicator follows.
    emitAllSliceState();
    return true;
}

void Hl2Backend::publishWideState()
{
    // WIDE means the shared band filter could not serve every active receiver,
    // so it was bypassed. Computed from the same rule applyBandFilter() applies,
    // rather than from a flag it sets, so the indicator cannot drift out of step
    // with the relays.
    //
    // THE RECEIVE BYTE, because "wide" is a statement about what the RECEIVERS
    // can hear. Asking the transmit rule here would report every receiver as
    // narrow on a transmit-only filter board, where receive is in fact wide
    // open — the exact opposite of what this indicator exists to say.
    int want = -1;
    bool spanned = false;
    for (const Receiver& r : m_rx) {
        const int w = static_cast<int>(m_hw.ocReceiveByteForHz(r.sliceFreqHz));
        if (want < 0)
            want = w;
        else if (w != want)
            spanned = true;
    }
    for (const auto& ids : m_ids.all())
        emit panWideChanged(ids.panId, spanned);
}

// Thread-affinity check for the RX audio mute helpers below: m_rxAudioMuted
// is read unsynchronised by mixReceiverAudio() on the backend's thread. A
// warning in every build (Q_ASSERT vanishes under QT_NO_DEBUG, i.e.
// RelWithDebInfo), plus Q_ASSERT_X to stop debug runs at the call.
static void hl2RequireBackendThread(const QObject* owner, const char* what)
{
    if (owner->thread() == QThread::currentThread()) {
        return;
    }
    qCWarning(lcHl2) << "HL2:" << what
                     << "was called from a thread that does not own the "
                        "backend. m_rxAudioMuted is read without "
                        "synchronisation by mixReceiverAudio(), so this is a "
                        "torn receive-audio gate, not a style point.";
    Q_ASSERT_X(false, what, "called off the Hl2Backend's own thread");
}

void Hl2Backend::applyRxAudioMute(bool muted)
{
    hl2RequireBackendThread(this, "applyRxAudioMute");
    m_rxAudioMuted = muted;
    // Record the moment sampling is asked to RESUME at the site that actually
    // asks for it, which since #5497 is HERE and not at the top of
    // applyKeying(): on the unkey edge the request is now made a hold later
    // than the key edge, and a stamp taken at the key edge would have told
    // SliceSamplingGate the chain was sampling for the whole of the hold. The
    // queued delivery to the DSP thread still leaves one block of skew, which
    // is the skew the gate was built to answer — it reads the stamp Hl2RxDsp
    // writes while unmuted rather than trusting this request.
    m_sliceSampling.setRequested(!muted, hl2::steadyNowNs());
    for (Receiver& r : m_rx) {
        if (r.dsp) {
            QMetaObject::invokeMethod(r.dsp, "setAudioMuted", Qt::QueuedConnection,
                Q_ARG(bool, muted));
        }
    }
}

void Hl2Backend::releaseRxAudioMuteAfterHold()
{
    hl2RequireBackendThread(this, "releaseRxAudioMuteAfterHold");
    if (!m_rxAudioMuted) {
        // Nothing is being held, so there is nothing to defer. This is the
        // ordinary path when the TX audio monitor is on: the key edge never
        // muted anything, and starting a 70 ms timer to un-mute an unmuted
        // chain would be a lie in the trace and a spurious wake-up.
        if (m_unkeyUnmuteTimer) {
            m_unkeyUnmuteTimer->stop();
        }
        return;
    }
    if (m_unkeyUnmuteHoldMs <= 0 || !m_unkeyUnmuteTimer) {
        applyRxAudioMute(false);
        return;
    }
    // start() on a running single-shot timer RESTARTS it, which is the
    // behaviour a second unkey inside the window wants: that unkey has its own
    // T/R turnaround to cover and inherits none of the elapsed time of the
    // first. The bench build this fix comes from did not coalesce them.
    m_unkeyUnmuteTimer->start(m_unkeyUnmuteHoldMs);
}

void Hl2Backend::mixReceiverAudio(int ddc, const std::vector<float>& pcm)
{
    // Gated on m_rxAudioMuted, the same flag the demodulator mute uses (#5497),
    // so the post-unkey hold is a gap in audioFrameReady() rather than zeros,
    // and blocks in flight at key-down are dropped. The TX audio monitor
    // exception is carried by the flag (applyKeying() never sets it with the
    // monitor on); this is the only emitter of audioFrameReady(), which feeds
    // the engine's output capture. At most one zero-filled block may pass at
    // the end of the hold (flag clears here, unmute is queued to the DSP).
    if (m_rxAudioMuted) {
        return;
    }
    const Receiver* r = rx(ddc);
    if (!r || r->audioMuted)
        return;   // not queued at all: a muted receiver must not accumulate

    if (m_mixPending.size() != m_rx.size())
        m_mixPending.resize(m_rx.size());
    auto& q = m_mixPending[static_cast<std::size_t>(ddc)];
    q.insert(q.end(), pcm.begin(), pcm.end());

    // FAST PATH. One unmuted receiver at unity gain and centre balance is not a
    // mix, and making it walk the summing code below would add a copy and a
    // clamp to the single-slice case that has been on the air for weeks.
    //
    // The gain/pan test is part of the condition, not an afterthought: taking
    // this path with a non-unity level would silently ignore the operator's
    // fader whenever only one slice happened to be open.
    int contributors = 0;
    for (const Receiver& other : m_rx)
        if (other.dsp && !other.audioMuted)
            ++contributors;
    if (contributors <= 1 && r->audioGain == 1.0f
        && r->audioPanPercent == kAudioPanCentre) {
        // The queue was empty before the insert above, so it holds exactly the
        // block we were handed: emit that directly. This is the steady state and
        // the reason the fast path exists — no remix or clamp. The PCM
        // adapter takes an owning copy for queued consumers.
        if (q.size() == pcm.size()) {
            publishLegacyAudio(floatBytes(pcm));
            forwardSpeakerAudioToCodec(pcm);
            q.clear();
            return;
        }
        // Entering the single-receiver path with residue left by the
        // min()-aligned drain: flush the whole queue rather than dropping it,
        // or muting a slice glitches the other. Residue is always whole stereo
        // frames.
        m_mixAccum.assign(q.cbegin(), q.cend());
        q.clear();
        publishLegacyAudio(floatBytes(m_mixAccum));
        forwardSpeakerAudioToCodec(m_mixAccum);
        return;
    }

    // Drain as much as EVERY contributor can supply. The receivers share an
    // input clock (one EP6 packet feeds them all) so they run in near-lockstep,
    // but WDSP's worker is asynchronous and their blocks do not arrive together.
    // Mixing min() keeps the sum sample-aligned rather than smearing one
    // receiver's block across another's.
    std::size_t n = std::numeric_limits<std::size_t>::max();
    std::size_t deepest = 0;
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        if (!m_rx[i].dsp || m_rx[i].audioMuted)
            continue;
        n = std::min(n, m_mixPending[i].size());
        deepest = std::max(deepest, m_mixPending[i].size());
    }
    if (n == std::numeric_limits<std::size_t>::max())
        return;

    // STARVATION GUARD. Without this, one receiver whose DSP stalls holds min()
    // at zero and the radio goes SILENT — every other receiver included. That is
    // strictly worse than the fault it is reacting to, so past the cap the
    // laggard is mixed as silence and the rest stay audible.
    if (n == 0) {
        if (deepest < kMixStarvationSamples)
            return;               // still within normal jitter; wait for it
        n = deepest - kMixStarvationSamples;
        if (n == 0)
            return;
    }

    // Drain a whole number of STEREO FRAMES. The stream is interleaved L,R, so
    // an odd sample count would swap the channels of everything after it — and
    // it stays swapped, because the offset carries into the next block.
    n &= ~std::size_t{1};
    if (n == 0)
        return;

    m_mixAccum.assign(n, 0.0f);
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        const Receiver& src_rx = m_rx[i];
        if (!src_rx.dsp || src_rx.audioMuted)
            continue;
        auto& src = m_mixPending[i];
        const std::size_t take = std::min(n, src.size()) & ~std::size_t{1};

        // Balance is a BALANCE, not a constant-power pan: centre leaves both
        // channels at unity rather than dipping them by 3 dB, so moving the
        // control off centre only ever attenuates the side you moved away from.
        // That matches what the operator expects of the Flex control this
        // mirrors, and keeps a centred slice bit-identical to no processing.
        const float g = src_rx.audioGain;
        const int p = src_rx.audioPanPercent;
        const float lw = g * (p <= kAudioPanCentre
                                  ? 1.0f
                                  : static_cast<float>(100 - p) / kAudioPanCentre);
        const float rw = g * (p >= kAudioPanCentre
                                  ? 1.0f
                                  : static_cast<float>(p) / kAudioPanCentre);

        for (std::size_t k = 0; k + 1 < take; k += 2) {
            m_mixAccum[k]     += src[k]     * lw;
            m_mixAccum[k + 1] += src[k + 1] * rw;
        }
        src.erase(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(take));
    }

    // Clamp rather than scale by 1/N. Dividing would make every slice quieter
    // the moment a second one is opened, which an operator reads as the radio
    // going deaf; clamping leaves a single slice at exactly the level it had
    // and only costs headroom when several loud slices genuinely coincide.
    for (float& s : m_mixAccum)
        s = std::clamp(s, -kMixCeiling, kMixCeiling);

    publishLegacyAudio(floatBytes(m_mixAccum));
    forwardSpeakerAudioToCodec(m_mixAccum);
}

void Hl2Backend::releaseReceiverDsps()
{
    // WITHDRAW EVERY CHAIN FIRST, then destroy them — same order, and same reason,
    // as removePanadapter(). Collect and null the pointers, publish the now-empty
    // list (blocking, so the I/O thread has stopped feeding them when it returns),
    // and only then post the destruction.
    std::vector<Hl2RxDsp*> doomed;
    doomed.reserve(m_rx.size());
    // Indexed rather than ranged, because the S-meter withdrawal below needs the
    // DDC index to find the receiver's UI NUMBER, and m_rx is indexed by DDC.
    for (std::size_t k = 0; k < m_rx.size(); ++k) {
        Receiver& r = m_rx[k];
        // Build identity belongs to the retiring DSP, not the state copied on reconnect.
        r.dspBuildInFlight = false;
        r.dspBuildGeneration = 0;
        if (!r.dsp)
            continue;
        // Withdraw its S-meter: a non-null dsp means openReceiverDsp() declared
        // one. Not redundant with MeterModel::clear(), which only runs on
        // disconnected(); superseded and failed-socket teardowns never emit it.
        // By UI number from the map (the ddc<->ui identity is not an invariant).
        if (const Hl2ReceiverIds* ids = m_ids.byDdc(static_cast<int>(k))) {
            withdrawSliceLevelMeter(ids->uiNumber);
        }
        doomed.push_back(r.dsp);
        r.dsp = nullptr;
    }
    publishIoDsps();
    for (Hl2RxDsp* d : doomed) {
        // The DSP lives on the I/O thread and owns a WDSP channel plus an FFTW
        // plan. deleteLater() posts the destruction to that thread's event loop,
        // which is the only thread allowed to close the channel -- destroying it
        // from here would release a WDSP channel id from the wrong thread while
        // processIqBlock could still be running.
        //
        // Disconnected as well, so its own outputs stop arriving; the withdrawal
        // above is what stops the fan-out reaching it.
        d->disconnect(this);
        d->deleteLater();
    }
    // The mix buffers describe the set that just went away.
    m_mixAccum.clear();
    m_mixPending.clear();
    // The DSP-dependent half of the index map is no longer true. The DDC and UI
    // numbers stay, because those are ours and outlive any DSP.
    for (const auto& ids : m_ids.all()) {
        if (auto* m = m_ids.mutableByDdc(ids.ddcIndex)) {
            m->dspChannel = -1;
            m->analyzerId = -1;
        }
    }
}

void Hl2Backend::tearDownReceivers()
{
    invalidateTxDspConfiguration();
    releaseReceiverDsps();   // already withdrew every chain from the sample path
    m_rx.clear();
    publishIoDsps();         // and now the list is empty, not merely all-null
    m_ids.clear();
}

void Hl2Backend::withdrawIoDsps()
{
    publishIoDspList({});
}

void Hl2Backend::publishIoDsps()
{
    std::vector<Hl2RxDsp*> next;
    next.reserve(m_rx.size());
    for (const Receiver& r : m_rx)
        next.push_back(r.dsp);
    publishIoDspList(std::move(next));
}

void Hl2Backend::publishIoDspList(std::vector<Hl2RxDsp*> next)
{
    // m_metis is the handle onto the I/O thread's event loop — it is the object
    // that lives there (m_ioThread itself does not; a QThread has the affinity of
    // the thread that CREATED it, which is this one).
    if (m_metis && m_ioThread && m_ioThread->isRunning()
        && QThread::currentThread() != m_ioThread) {
        QMetaObject::invokeMethod(m_metis, [this, next] { m_ioDsps = next; },
                                  Qt::BlockingQueuedConnection);
        return;
    }
    // No I/O thread to hand it to: before it starts, after it is joined, or when
    // already on it. Nothing is reading m_ioDsps in any of those, and a blocking
    // invoke into a dead event loop — or into one's own — hangs forever.
    m_ioDsps = next;
}

Hl2Backend::~Hl2Backend()
{
    // Join the build thread before the I/O thread it posts to, so finished
    // channels are installed or destroyed by their owning thread. Blocks for
    // an in-flight OpenChannel (WDSP opens cannot be cancelled).
    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }
    if (m_ioThread) {
        // Stop the wire on its own thread and wait: a queued stop() would never
        // run after quit(). Blocks on the GUI thread for an in-flight DSP build
        // (docs/HERMES.md §22.4); beginDspSetup()'s QPointer relies on this.
        if (m_metis)
            QMetaObject::invokeMethod(m_metis, "stop", Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    } else if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "stop");
    }
    // Safe now: the thread is joined, so nothing can be running in any of them.
    // The receivers are deleted OUTRIGHT rather than through tearDownReceivers():
    // that posts deleteLater() to the I/O thread's event loop, which has already
    // ended here, so the DSP chains would leak along with their WDSP channel ids.
    for (Receiver& r : m_rx)
        delete r.dsp;
    m_rx.clear();
    m_ioDsps.clear();   // the thread that read this is joined; plain clear is safe
    m_ids.clear();
    delete m_txDsp;
    delete m_metis;
    // After both joins, so nothing can be posting to it.
    delete m_dspBuildContext;
}

AetherSDR::WidebandConverterView widebandConverterViewRecord() noexcept
{
    AetherSDR::WidebandConverterView wide;
    // The converter's numbers and not a display choice: 76.8 MHz is
    // hermeslite_core.v's CLK_FREQ, so the view spans DC..38.4 MHz, and 2048 is
    // the depth of the capture FIFO the gateware drains four datagrams at a
    // time.
    wide.sampleRateHz = kAdcSampleRateHz;
    wide.blockSamples = kEp4BlockSamples;
    // MUST match the namespace in extensionNamespaces and the verb string
    // invokeExtension() compares against. wideband_converter_view_test asserts
    // exactly that, because nothing else would notice until an operator did.
    wide.frameNamespace = QStringLiteral("hl2");
    wide.frameVerb = QStringLiteral("bandscope.frame");
    return wide;
}

RadioCapabilities Hl2Backend::capabilities() const
{
    RadioCapabilities c;
    c.canReboot = false;
    c.hasRemoteOnControl = false;
    c.canUpgradeFirmware = false;
    c.hasSmartLink = false;
    c.hasLicenseInfo = false;
    c.hasClientNetworkConfig = false;
    c.hasFlexControlIntegration = false;
    c.hasAudioCompression = false;
    c.hasSharpFilters = false;
    c.usesVita49Transport = false;
    c.hasNetworkConfigurationReadback = false;
    c.hasPrivateIpConnectionPolicy = false;
    c.txPowerBands = {};
    c.declaredBandRanges = {};
    c.family = QStringLiteral("hl2");
    // setTune() is the built-in test tone at ZERO offset — a single carrier
    // exactly on the TX NCO. Nothing here can produce a second tone.
    c.twoToneGenerator = std::nullopt;
    c.manufacturer = QStringLiteral("Hermes-Lite");
    c.model = QStringLiteral("Hermes-Lite 2");
    // No repeater duplex and no tone encode: FM/NFM are receive-only below on
    // every build, so these would imply FM transmit. hasFmRepeaterOffset
    // defaults true and this backend overrides no repeater-offset setter;
    // Hidden tone presentation since there is no CTCSS/DCS encoder.
    c.hasFmRepeaterOffset = false;
    c.fmTonePresentation = FmTonePresentation::Hidden;
    c.fmDtcsCodes = {};
    // The ceiling, not the running count, so "Add Panadapter" can be offered:
    // the board's discovery byte 0x13 capped by the link budget at the current
    // span (it falls when zooming out).
    const int ceiling = m_connected ? receiverCeiling()
                                    : std::max(1, m_ids.size());
    c.canCreateSlices = false;
    c.maxSlices = ceiling;
    c.maxPanadapters = ceiling;
    for (const int rate : kIqSampleRatesHz)
        c.sampleRatesHz.append(rate);
    PanSpanModel span;
    // The span is the sample rate: spectrum is computed from raw IQ, so no pan
    // narrower than 48 kHz exists. Zoom requests snap to these rates
    // (nearestIqSampleRateHz). #5223 is the RFC for sub-window display.
    span.followsSampleRate = true;
    // One span for the whole radio: ccConfig carries a single sample-rate field
    // (C1[1:0]) beside the receiver count (C4), so changing one pan's span
    // rebuilds every receiver (applyPanBandwidth). Hence
    // receivePanBandwidthControl is nullopt, and receiverCeiling() falls as the
    // span widens (shared 100BASE-T budget).
    span.radioWide = true;
    c.panSpanModel = span;

    PanAmplitudeModel amplitude;
    // The Y axis is dBFS under a dBm label. Hl2DbReference::fullScaleDbm (+3
    // dBm) is derived from the AD9866 datasheet and input network, not measured
    // per unit, and isCalibrated() stays false until setFullScaleDbm() is
    // called (nothing in src/ does). Relative levels are consistent; absolute
    // levels must not be compared, spotted or thresholded.
    amplitude.calibratedDbm = m_dbRef.isCalibrated();
    // Bins are absolute dBFS computed on this host, shifted only by
    // m_dbRef.offsetDb() (the LNA reference, which never follows m_refLevel),
    // and passed through RadioModel::onBackendSpectrumFrame untouched, so
    // SpectrumWidget's noise-floor auto-adjust converges without a dBm range
    // echo. See PanAmplitudeModel::binsAbsolute.
    amplitude.binsAbsolute = true;
    c.panAmplitude = amplitude;

    // radioOwnsDbmScale is not declared: this radio cannot be commanded a dBm
    // range, so it would be false, but declaring that is its own change.
    // binsAbsolute already keeps noiseFloorAutoAdjustAllowed() open (bench run
    // d101: the loop settles, 0.307 dB in 74 s quiescent).
    // Tuning range: the AD9866 samples at 76.8 MHz, so the first Nyquist zone is
    // DC to 38.4 MHz; below 100 kHz the input transformer rolls off.
    c.tuningMinHz = 100'000.0;
    c.tuningMaxHz = 38'400'000.0;
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine,
                               100'000, 38'400'000};
    // Modes the headless receive path accepts. ModelReceiveControlTarget checks
    // both the requested mode and the slice's observed mode against this list,
    // so every mode publishedModeStrings() offers must be here (including DSB,
    // CWL, and FM, which demodulates but with default 5 kHz deviation and AGC
    // cleared). No alias spellings: slices hold canonical modes
    // and an alias request would wedge "request.conflict" (control_receive_test
    // asserts canonicalOfferedMode(m) == m). WBFM/WFM and DRM stay off.
    c.receiveModeControl = ReceiveModeControl{SliceFrequencyControl::Authority::Engine,
        {QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("DSB"),
         QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("AM"),
         QStringLiteral("SAM"), QStringLiteral("CW"), QStringLiteral("CWL"),
         QStringLiteral("FM")}};
    // Conservative carrier-relative subdomains of the existing WDSP passband.
    c.receiveFilterControl = ReceiveFilterControl{SliceFrequencyControl::Authority::Engine, {
        {QStringLiteral("USB"), 0, 11990, 10, 12000, 10, 12000},
        {QStringLiteral("DIGU"), 0, 11990, 10, 12000, 10, 12000},
        {QStringLiteral("LSB"), -12000, -10, -11990, 0, 10, 12000},
        {QStringLiteral("DIGL"), -12000, -10, -11990, 0, 10, 12000},
        {QStringLiteral("AM"), -12000, -10, 10, 12000, 20, 24000},
        {QStringLiteral("SAM"), -12000, -10, 10, 12000, 20, 24000}}};
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receivePanCenterControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                      100'000, 38'400'000};
    c.receivePanBandwidthControl = std::nullopt; // radio-wide rate can retire other receivers
    // Power class for every forward-power gauge, via the band-table seam
    // (RadioModel::refreshTxPowerLimit; TxApplet scales from a non-empty
    // table). One band: the HL2 is rated 5 W on all HF bands (HL2 wiki FAQ).
    // The +17 dBm RF1 instrumentation output is hardware-selected with no
    // readback, so it is not described.
    c.txPowerBands = {TxPowerBand{c.tuningMinHz, c.tuningMaxHz,
                                  static_cast<double>(kHl2RatedOutputWatts)}};
    // Reported from the gate, not hardcoded: the engine's TX guard keys off this,
    // so a build with transmit disabled must look RX-only from above the seam.
    c.canTransmit = m_txAllowed;
    // Modes this radio demodulates but cannot modulate: Hl2TxDsp only
    // distinguishes sidebands, so these would go out as USB. SSB family and CW
    // (gateware-keyed at the TX NCO) are not listed. Both spellings are listed
    // because refuseKeyInReceiveOnlyMode() compares the slice's string. This
    // also refuses TUNE in these modes, since the list has no per-activity
    // granularity (same on IC-705, #5040).
    c.receiveOnlyModes = {QStringLiteral("AM"),   QStringLiteral("SAM"),
                          QStringLiteral("DSB"),  QStringLiteral("FM"),
                          QStringLiteral("NFM"),  QStringLiteral("WBFM"),
                          QStringLiteral("WFM"),  QStringLiteral("DRM")};
    c.hostModulates = true;
    // Same tap, same seam — see RadioCapabilities::takesTxAudioOverSeam.
    c.takesTxAudioOverSeam = true;             // PC runs the modulator; no on-radio mic jacks
    // No PTT status plane: the command edge is the only keyed edge there is.
    c.hasRadioPttReadback = false;
    c.txPowerMaxWatts = 0.0;            // uncalibrated; see the oracle on power counts
    // HL2 publishes an instantaneous directional estimate; preserve the
    // established client-side PEP response above the backend seam.
    c.forwardPowerRequiresSmoothing = true;
    // Drive here is OPERATOR INTENT, not a readback (#5518). setTxPower() records
    // the requested percent before the transmit gate and applyDrive() holds the
    // drive register at 0 while !m_txAllowed, so TransmitModel::rfPower() can read
    // 100 with no RF leaving the radio. Consumers that act on drive must see that
    // distinction rather than infer applied power from a request.
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine};
    c.hasRadioDialLock = false;
    c.hasTuner = false;
    c.hasTunerMemories = false;
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
    // Both moot while hasRadioSideDsp is false — the host runs every filter
    // this radio has — but stated rather than defaulted, per the struct's
    // "a backend that omits one silently declares it absent" rule.
    c.hasLmsNoiseFilters = false;
    c.hasAudioPeakingFilter = false;
    c.hasManualNotch = false;
    c.hasTransmitFrequencyCheck = false;
    c.hasDdcPanEdgeRolloff = false;
    // The panadapter is averaged here, in Hl2Spectrum, per the operator's FFT
    // AVG (setPanAverage). SpectrumWidget skips its own fixed SMOOTH_ALPHA EMA
    // while this is set, so the two never stack (RFC #5782).
    c.backendPanAveraging = BackendPanAveraging{kMsPerAverageStep};
    // No band/segment zoom: this backend vends no command plane at all, so
    // `display pan set ... band_zoom=` is dropped inside RadioModel::sendCmd.
    // Declaring absence is what makes the control refuse rather than lie.
    c.panZoomModes = std::nullopt;
    // The one member of the noise family that is NOT moot here. WDSP's ANB runs
    // on this host, on the raw IQ, ahead of the demodulator — the same
    // arrangement as the manual notch and for the same reason (oracle addendum
    // 3 §B4: the HL2 carries no DSP). NR and ANF are left off because they are
    // not implemented, not because they could not be.
    c.hasHostNoiseBlanker = true;
    // Squelch is host-side too, per mode family: FM on WDSP's fmsq, AM/SAM/DSB/
    // LSB/USB on the level squelch amsq (WdspChannel::setSquelch()). CW and the
    // data modes have no stage, so this is false, stated explicitly: the client
    // then disables SQL there instead of showing a button wired to nothing.
    c.hasModeIndependentSquelch = false;
    // The 76.8 MHz NCO scale is a localparam in the bitstream and nothing in the
    // HPSDR map can be told the crystal's real error — so the correction is ours
    // or it does not happen. See Hl2FreqCal for the derivation.
    c.hostFrequencyCalibration = true;
    // Not yet measured/calibrated for this radio -- see
    // RadioCapabilities::hostDroopCalibration's own comment on why "false"
    // here is not a claim the HL2's DDC has no droop, only that nothing has
    // characterised or corrected one.
    c.hostDroopCalibration = false;
    // Declared because invokeExtension() now implements it (freqcal.get / .set /
    // .set_live). This field is the handshake a client pre-checks before issuing
    // an extension call, so leaving it empty while the verbs work would report
    // the opposite of the truth.
    c.extensionNamespaces << QStringLiteral("hl2");
    // Wideband converter view (docs/HERMES.md §13 item 18), declared rather than
    // inferred from the family string. Only while connected: the verb raises
    // wide_spectrum on a streaming radio and MetisClient refuses otherwise.
    // 76.8 MHz is hermeslite_core.v's CLK_FREQ (span DC..38.4 MHz); 2048 is the
    // capture FIFO depth, drained four datagrams at a time.
    if (m_connected)
        c.widebandConverterView = widebandConverterViewRecord();
    // No on-radio configuration store. The HL2 holds no state across a
    // connection beyond its registers — everything the operator can change
    // lives in this application, so there is nothing for a profile to name.
    c.hasProfiles = false;
    c.hasSelectableMicInputs = false;
    c.hasDownwardExpander = false;
    c.hasAgcThreshold = true; // Host receiver DSP implements threshold/off gain.

    // EMPTY: the HL2's receive filters are the host DSP's, and continuous.
    c.rxFilterWidthsHz = {};
    // The host modulator implements a continuous transmit passband.
    c.hasTxFilterControls = true;
    // No per-slice audio or per-pan IQ stream plane: the HL2 sends one raw IQ
    // feed and this host demodulates it.
    c.hasDaxStreams = false;
    // Every noise module for this radio runs on THIS host — the HL2 sends raw
    // IQ and has no firmware DSP to switch on. Gating the radio-side toggles
    // off is what stops them from looking operable; the client-side modules
    // (NR2/NR4/MNR/BNR/DFNR/RN2) are unaffected and remain available.
    c.hasRadioSideDsp = false;
    // Same reason, on the display plane: nothing in the HL2 computes a
    // waterfall black level, so the Black Level button's HW position would be
    // a mode that never produces one. Off <-> SW only. (#4606)
    c.hasRadioSideWaterfallAutoBlack = false;
    // No command plane to carry any of these. The HL2 has no text buffer for a
    // CW keyer, no voice recorder and no full-duplex setting — so the three
    // status-bar toggles that drive them go away rather than sitting greyed
    // out. The host-side equivalents are untouched: this radio still transmits
    // CW, and this client's own noise modules are the only DSP it has. TNF is
    // deliberately NOT gated — see the note in RadioCapabilities.h.
    c.hasRadioSideCwKeyer = false;
    c.hasVoiceKeyer = false;
    c.hasFullDuplex = false;
    c.hasWaveforms = false;             // no installable plugin surface
    c.hasMultiClientSessions = false;   // one client owns the radio
    // Spots live in this client or nowhere. The Flex path publishes every
    // DX-cluster, RBN, WSJT-X, POTA and manual spot as `spot add` wire text and
    // waits for the radio's `spot <id>` status to put it on the panadapter. The
    // HL2 has no command plane, so RadioModel::sendCmd drops that text and no
    // status ever comes back: with this false, spots were fetched and never
    // drawn. True routes them into the passive-local SpotModel, the same
    // fallback Icom declares for the same reason.
    c.alwaysUseClientSideSpots = true;
    // Manual notches, and the one piece of DSP on this radio that is NOT absent
    // just because hasRadioSideDsp is false. The notch runs in WDSP on this
    // host, which is the whole point: the HL2 sends raw IQ, so a notch either
    // happens here or nowhere (oracle addendum 3 §B4).
    //
    // The ceiling is WDSP's own notch database size (RXA.c creates it with room
    // for 1024), not a guess. Each active notch inside the passband adds a
    // sub-band to the multi-bandpass mask, so the practical limit is taste
    // rather than capacity.
    c.maxNotchFilters = 1024;
    // A WDSP notched bandpass is a full null. There is no depth parameter to
    // map three Flex depths onto, so the depth submenu is hidden rather than
    // offering three settings that behave identically.
    c.notchHasDepth = false;
    // Set by the RX filter length and enforced silently — see Hl2RxDsp's
    // kMinNotchWidthHz. Reported so the UI's width choices match what the
    // operator will actually hear.
    c.notchMinWidthHz = Hl2RxDsp::kMinNotchWidthHz;
    c.notchMaxWidthHz = 6000.0;
    c.hasGpsLocation = false;           // no GNSS receiver on the board
    c.hasGpsSatelliteTelemetry = false;
    c.hasGpsFrequencyReference = false;
    c.hasGpsTimeConfiguration = false;
    c.hasGpsHardware = false;
    c.gpsHardwareRequiresPresence = false;
    // The HL2 declares PATEMP but no "+13.8A": PA temperature is a real reading
    // from this radio, the supply rail is not reported at all. Only the volts
    // readout goes away — the temperature above it keeps working.
    c.hasSupplyVoltageTelemetry = false;
    c.hasPaTemperatureTelemetry = true;
    c.hasPaCurrentTelemetry = false;
    c.speechProcessorLevelMaximum = 2;
    c.speechProcessorLabel = QStringLiteral("PROC");
    c.hasMainFanTelemetry = false;
    // The HL2 persists NOTHING across power cycles — "the radio reports no
    // VFO, so the app is authoritative and must push" (pushInitialState).
    // These are the domains the client owns as the radio's memory
    // (RFC #4603): consumed by RadioStateMemory, wired per-domain in the
    // RFC's PR 3 (per-band drive/LNA maps per nigelfenton's review).
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::Tuning
                            | RadioCapabilities::ClientSettingsDomain::Passband
                            | RadioCapabilities::ClientSettingsDomain::SpanRate
                            | RadioCapabilities::ClientSettingsDomain::RfGain
                            | RadioCapabilities::ClientSettingsDomain::TxSetpoints
                            // The AGC runs in WDSP on THIS HOST — there is no
                            // AGC register in the HPSDR map to read back, so
                            // the client is the only place the operator's mode
                            // and threshold can live. Without this the channel
                            // reopened on Config's defaults every launch and
                            // the setting reverted to med/65 (#4909).
                            | RadioCapabilities::ClientSettingsDomain::Agc
                            // CW timing and sidetone are also host-owned. A
                            // Flex persists these in radio firmware; HL2 has no
                            // corresponding readable state, so RadioModel keeps
                            // the complete CW surface per radio.
                            | RadioCapabilities::ClientSettingsDomain::Cw
                            // Memories: the client owns them (persistsMemories
                            // is false above). NOTE the bank itself engages on
                            // persistsMemories and keeps its own SHARED
                            // document — this declaration is descriptive
                            // completeness, and Memories stays out of
                            // RadioStateMemory's ext gate (one domain, one
                            // document — RFC #4603 PR 6).
                            | RadioCapabilities::ClientSettingsDomain::Memories;
    // (extensionNamespaces is declared above, with the freqcal/nb verbs it
    // names — an earlier revision of this comment claimed none existed.)
    return c;
}

void Hl2Backend::connectRadio(const RadioConnectRequest& request)
{
    const QHostAddress host(request.host);
    if (host.isNull()) {
        emit connectionError(QStringLiteral("HL2: invalid host '%1'").arg(request.host));
        return;
    }

    // Point the stream-free poller at this radio now, before the stream exists.
    // Not for the connected case -- the cadence rule polls at zero while EP6 is
    // healthy -- but so the poller ALREADY knows the address when the stream
    // later stalls. Discovering the target at the moment of failure would mean
    // the one path meant to survive a broken stream depends on the machinery
    // that just broke.
    setTelemetryPollTarget(host, /*heldByOther=*/false);

    // A connect while the previous chains are still opening cannot be served
    // inline (buildReceivers() would destroy chains being opened, and
    // publishIoDsps() would block on the busy I/O thread). Supersede the build
    // and re-drive from finishDspSetup().
    if (m_pendingConnect) {
        qCInfo(lcHl2) << "HL2: connect requested while the DSP was still opening"
                      << "— queued behind it";
        ++m_connectGeneration;
        invalidateTxDspConfiguration();
        m_queuedConnect = std::make_unique<RadioConnectRequest>(request);
        return;
    }

    // A new connect re-derives the passband from the mode; a mid-session linkUp
    // (EP6 silence then resume) does not. See m_passbandDerivedThisConnect.
    m_passbandDerivedThisConnect = false;

    // Load this radio's frequency calibration first, before any frequency is
    // computed. Keyed by MAC (one crystal per radio); an empty serial gets the
    // family-wide row, empty by default, i.e. uncalibrated.
    m_radioSerial = request.serial;
    m_freqCalPpb = Hl2FreqCal::loadPpb(
        RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial));
    m_freqCalScale = Hl2FreqCal::scaleForPpb(m_freqCalPpb);
    if (m_freqCalPpb != 0)
        qCInfo(lcHl2) << "HL2: frequency calibration" << m_freqCalPpb << "ppb"
                      << "— effective clock"
                      << Hl2FreqCal::effectiveClockHz(m_freqCalPpb) << "Hz";

    // WHICH HL2 THIS IS, from the same per-radio scope and for a related
    // reason: the calibration describes one crystal, and this describes one
    // BOARD. Loaded here, before mp is filled in below, because three of these
    // fields are start() parameters — the dither bit rides the config register
    // the very first frame carries, and the codec decides whether that frame's
    // audio slot may hold anything at all.
    m_hw = Hl2HardwareOptions::load(
        RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial));
    // AND RECONCILE THE TWO DOCUMENTS BEFORE ANYTHING IS COMPUTED FROM THEM.
    // The calibration and the hardware options are separate rows, loaded by the
    // two independent calls above and written by two independent operations, so
    // "CL1 on with a manual ppb standing" is a state that can exist on disk —
    // an interruption between the two writes leaves exactly it. Nothing later in
    // connectRadio() would notice: mp is filled from m_hw a few lines down and
    // the initial NCO and DSP frequencies are computed from m_freqCalScale, so
    // the session would come up correcting a disciplined clock by the error of
    // the crystal it no longer runs on, with the UI's ppb control disabled and
    // therefore unable to show or repair it (#5923 review).
    normalizeCl1Calibration("restored settings disagree");

    // The codec's resampler carries state across blocks; a new radio is a new
    // stream and must not be interpolated out of the last one's final sample.
    m_codecHavePrev = false;
    if (m_hw != Hl2HardwareOptions{})
        qCInfo(lcHl2) << "HL2 hardware options: codec=" << static_cast<int>(m_hw.codec)
                      << "dither=" << m_hw.ditherBitOnWire()
                      << "random=" << m_hw.randomBit
                      << "filterBoard=" << static_cast<int>(m_hw.filterBoard)
                      << "n2adrHpf=" << m_hw.n2adrHpf
                      << "cl1=" << m_hw.cl1RefClock
                      << "atuGateware=" << m_hw.atuGateware;

    // Notches are SESSION state, and the same call is true on both sides of the
    // seam: RadioModel::onDisconnected() calls m_tnfModel.clear(), and a
    // same-family reconnect rebuilds no backend, so anything kept here would be
    // replayed into WDSP by seedNotches() with nothing on screen naming it —
    // audible nulls with no marker to right-click and no id to remove them by.
    // m_nextNotchId deliberately keeps counting: an id is never reused.
    m_notches.clear();
    m_notchesEnabled = true;

    // The span the operator last chose, snapped to a rate we can actually run
    // and to the current low-bandwidth ceiling. Applied BEFORE the explicit
    // param below so an automation/test caller can still pin a rate outright.
    //
    // Restoring this is what lets the default stay at the cheap 48 kHz: the
    // operator picks a wide span once and keeps it, instead of re-zooming every
    // launch, and nobody who never asked for it pays 25 Mbps.
    if (const double remembered = Hl2Settings::spanMhz(); remembered > 0.0)
        m_sampleRateHz = nearestIqSampleRateHz(remembered * 1.0e6);

    // RFC #4603 PR 3: this radio's remembered state (validated in
    // applyRestoredState) seeds the session. Ordering is deliberate — the
    // legacy family-wide span above is the fallback, the per-radio restored
    // rate beats it, and the explicit automation/test params below beat both.
    if (m_haveRestoredState && m_restoredState.sampleRateHz > 0)
        m_sampleRateHz = m_restoredState.sampleRateHz;

    // Optional overrides from the namespaced params.
    if (request.params.contains(QStringLiteral("sampleRateHz")))
        m_sampleRateHz = request.params.value(QStringLiteral("sampleRateHz")).toInt();
    if (request.params.contains(QStringLiteral("lnaGainDb")))
        m_lnaGainDb = request.params.value(QStringLiteral("lnaGainDb")).toInt();
    // m_dbRef is synced to the final m_lnaGainDb unconditionally at the seed
    // below (right before the wire command), so it cannot drift regardless of
    // which override params were supplied.
    // The frequency this session comes up on. Held locally until the receivers
    // exist, because m_rx is not built yet — how many of them there are is
    // decided a few lines below and depends on the sample rate settled above.
    double startFreqHz = 10'000'000.0;
    if (m_haveRestoredState && m_restoredState.rfFrequencyHz > 0.0)
        startFreqHz = m_restoredState.rfFrequencyHz;
    if (request.params.contains(QStringLiteral("rxFrequencyHz")))
        startFreqHz = request.params.value(QStringLiteral("rxFrequencyHz")).toDouble();

    // Per-band memory (RFC #4603 PR 3): the session comes up with the start
    // band's remembered LNA. The explicit param still wins via the guard.
    m_currentBandKey = hl2::bandKeyForHz(startFreqHz);
    {
        const bool paramPresent =
            request.params.contains(QStringLiteral("lnaGainDb"));
        const bool hasStored = m_lnaDbByBand.contains(m_currentBandKey);
        const AetherSDR::hl2::ConnectLna seed = AetherSDR::hl2::connectLna(
            m_haveRestoredState, hasStored,
            m_lnaDbByBand.value(m_currentBandKey, hl2::kLnaDefaultGainDb),
            paramPresent,
            request.params.value(QStringLiteral("lnaGainDb")).toInt(),
            hl2::kLnaDefaultGainDb, kLnaGainMinDb, kLnaGainMaxDb);
        // Only take the live value when the policy actually had something to
        // say: with no restored state and no param it returns the default,
        // which must not stamp on a value the lines above already settled.
        if (m_haveRestoredState || paramPresent) {
            m_lnaGainDb = seed.liveDb;
        }
        m_lnaSessionPin = seed.sessionPin;
        if (m_lnaSessionPin) {
            qCInfo(lcHl2) << "HL2: lnaGainDb param pins" << m_lnaGainDb
                          << "dB for this session;" << m_currentBandKey
                          << "keeps its stored"
                          << m_lnaDbByBand.value(m_currentBandKey) << "dB";
        }
    }
    // Seed the DRIVE from the start band's memory and echo it upward NOW —
    // before linkUp — so TransmitModel carries the restored value when its
    // connect-time push fires (PR #4619 bench, nigelfenton: the push arrived
    // as apparent operator intent at the model default of 100 and overwrote
    // the stored per-band drive on every reconnect; Ozy311 traced the same
    // seam). With the model seeded, that push becomes a value-identical echo
    // — which setTxPower() now recognizes and declines to record.
    if (m_haveRestoredState) {
        const int drive =
            m_driveByBand.value(m_currentBandKey, m_driveDefaultPercent);
        if (drive >= 0) {
            m_rfPowerPercent = drive;
            TransmitDelta delta;
            delta.rfPower = drive;
            emit transmitChanged(delta);
        }
    }

    // Receiver count: the smallest of what the operator asked for, what the
    // board has (discovery byte 0x13: hl2b5up_main reports 4, skimmer variants
    // 9-12 with no TX), and what the link carries at this rate (4 RX at 384
    // kHz is ~89 Mbit/s and drops packets). Connect always starts with one;
    // more come from "Add Panadapter". `numRx` is only an explicit connect
    // param, for automation.
    m_requestedNumRx = 1;
    if (request.params.contains(QStringLiteral("numRx")))
        m_requestedNumRx = request.params.value(QStringLiteral("numRx")).toInt();
    if (m_requestedNumRx < 1)
        m_requestedNumRx = 1;

    const int rateLimited = maxReceiversAtRate(m_sampleRateHz, m_requestedNumRx);
    if (rateLimited < m_requestedNumRx) {
        qCInfo(lcHl2) << "HL2: link budget at" << m_sampleRateHz
                      << "Hz allows" << rateLimited << "receiver(s), not"
                      << m_requestedNumRx
                      << QString::asprintf("(%.1f Mbit/s)",
                             ep6BitsPerSecond(m_sampleRateHz, m_requestedNumRx) / 1e6);
    }
    // The board's own limit is applied by MetisClient::effectiveNumRx(), which
    // is the only place that has seen the discovery reply. We ask for what the
    // budget allows and read back what was actually configured.

    MetisClient::Params mp;
    mp.host = host;
    mp.port = request.port ? request.port : kMetisPort;
    mp.sampleRate = sampleRateEnum(m_sampleRateHz);
    // The connect handshake IS the first commit: this rate goes into
    // MetisClient's initial command bank, so from here the radio is running at
    // it. Without seeding it here the first zoom of a session would capture the
    // 48 kHz construction default as its previousRate and a failed build would
    // "restore" a rate the operator never had.
    m_rateLedger.commit(m_sampleRateHz);
    // COMMANDED, not true-RF: this seeds MetisClient's initial RX and TX
    // command banks, which are register contents. Everything else in this
    // function keeps startFreqHz in the true-RF domain.
    mp.rxFrequencyHz = ncoCommandHz(startFreqHz);
    // EFFECTIVE. resetPersistedState() zeroes the automatic offset, so on the
    // ordinary connect path this equals the baseline; taking it through the
    // split anyway means no future caller can leave a connect seeding the
    // register with a number the rest of the session does not agree with.
    mp.lnaGainDb = lnaEffectiveDb();
    mp.numRx = rateLimited;
    m_boardMaxRx = request.params.value(QStringLiteral("boardMaxRx")).toInt();
    if (m_boardMaxRx <= 0) {
        // Connecting by IP skips the broadcast sweep, so ask with a unicast
        // discovery for byte 0x13. A count the gateware does not have makes it
        // stream all-zero IQ slots. Short timeout; no answer keeps the default.
        QMetaObject::invokeMethod(m_metis, [this, &host] {
            for (const auto& d : m_metis->discover(400, host, kMetisPort)) {
                if (d.reply.numRx > 0) {
                    m_boardMaxRx = d.reply.numRx;
                    break;
                }
            }
        }, Qt::BlockingQueuedConnection);
    }
    if (m_boardMaxRx <= 0) {
        // Still unknown — a short reply, or a radio that did not answer the
        // unicast probe. Assume the SHIPPING gateware's four (hl2b5up_main is
        // built with NR=4) rather than the register's maximum. Guessing high
        // hands out receivers that stream zeros and look like a dead antenna;
        // guessing low costs at most a receiver the operator can still not have.
        m_boardMaxRx = kAssumedBoardMaxRx;
        qCInfo(lcHl2) << "HL2: board did not report a receiver count — assuming"
                      << m_boardMaxRx << "(shipping gateware NR)";
    } else {
        qCInfo(lcHl2) << "HL2: board reports" << m_boardMaxRx << "receiver(s)";
    }
    mp.boardMaxRx = m_boardMaxRx;
    // The filter board must be right from the first config frame: the radio
    // reports no state. All receivers start on one frequency, so this is that
    // frequency's filter (see applyBandFilter()). The receive byte, since some
    // boards have the low-pass in the TX path only (Hl2HardwareOptions::
    // FilterBoard).
    mp.ocFilterByte = m_hw.ocReceiveByteForHz(startFreqHz);
    m_ocFilterByte = static_cast<int>(mp.ocFilterByte);
    // The variant fields that are start() parameters. The dither bit and the
    // codec must be right on the FIRST frame for the same reason the filter
    // byte must: nothing reads them back, so a value applied one frame later is
    // a frame of the wrong thing — a SquareSDR 2's speaker clicking on at
    // connect, or a sample written into a bare HL2's EADDR.
    mp.ditherBit   = m_hw.ditherBitOnWire();
    mp.randomBit   = m_hw.randomBit;
    mp.hasCodec    = m_hw.hasLocalCodec();
    mp.cl1RefClock = m_hw.cl1RefClock;
    // Identity for the CL1 latch, which is per-radio while MetisClient is not.
    mp.radioSerial = m_radioSerial;
    qCInfo(lcHl2).nospace()
        << "HL2 band filter: " << QString::asprintf("0x%02X", m_ocFilterByte)
        << " (" << ocFilterName(mp.ocFilterByte) << ") for "
        << QString::number(startFreqHz / 1.0e6, 'f', 6) << " MHz, trigger=connect";
    // Seed the reference from the gain we are about to command, so the very
    // first spectrum frame is already on the same footing as every later one.
    m_dbRef.setLnaGainDb(lnaEffectiveDb());

    // Build the receivers before start(): opening a WDSP channel runs on the
    // I/O thread (~19 s on a first-ever open generating FFTW wisdom, 40-175 ms
    // after; docs/HERMES.md §10, §22.3), which would stall EP2 and the
    // gateware watchdog would halt the stream. The count comes from the static
    // effectiveNumRx(mp), the same clamp the client applies: EP6 carries no
    // receiver count, so host and radio must agree.
    const int actualNumRx = MetisClient::effectiveNumRx(mp);
    mp.numRx = actualNumRx;

    buildReceivers(actualNumRx);
    for (Receiver& r : m_rx) {
        r.sliceFreqHz = startFreqHz;
        r.ncoHz = startFreqHz;
    }

    // Seed the remembered AGC (#4909); the only place receivers are seeded
    // (see seedReceiverAgc()). The state is settled here and pushed to the DSP
    // later by beginDspSetup()/pushInitialState(). Seeded for a different
    // radio (keyed on serial), or when receivers have no carried state (after
    // tearDownReceivers()). Not on a same-radio reconnect whose receivers kept
    // their live per-receiver AGC.
    if (request.serial != m_agcSeededSerial || !m_rxCarriedState) {
        m_agcSeededSerial = request.serial;
        seedReceiverAgc();
    }

    Hl2RxDsp::Config dc;
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 24000;   // AudioEngine's native RX rate

    // The transmit chain follows the TX RECEIVER's mode, not "the mode": with
    // several receivers there is no single one. Built here, on the GUI thread,
    // where m_rx may be read; the open itself happens with the others.
    const Receiver* txRx = rx(m_txDdc);
    const QString txMode = txRx ? txRx->mode : QStringLiteral("USB");
    Hl2TxDsp::Config tc;
    tc.inputSampleRateHz = 24000;    // AudioEngine's rate; submitTxAudio re-checks
    tc.outputSampleRateHz = 48000;   // EP2 is fixed at 48 kHz
    tc.mode = modeFromString(txMode);
    // Sideband-correct from the first key, not from the first mode change. The
    // struct default is a positive 300..2700, so connecting straight into LSB
    // or DIGL would otherwise transmit on the upper sideband until the operator
    // happened to change mode.
    {
        const auto [txLo, txHi] = effectiveTxPassband(txMode);
        tc.filterLowHz  = txLo;
        tc.filterHighHz = txHi;
    }
    // Remember the target the modulator is being handed, so the health snapshot
    // and the unkey diagnostic both report what it is RUNNING rather than what
    // a default-constructed Config would have said. Assigned from `tc` and not
    // from the default so it stays correct the day this Config sets the field.
    m_alcTargetPeak = tc.alcTargetPeak;

    // Echo the passband the modulator is configured with, so the Phone applet
    // shows a restored value. Emitted now rather than after the async open:
    // the value is decided, and this still precedes linkUp.
    {
        TransmitDelta delta;
        delta.txFilterLow = tc.filterLowHz;
        delta.txFilterHigh = tc.filterHighHz;
        emit transmitChanged(delta);
    }

    // Hand the opens to the I/O thread and RETURN. finishDspSetup() picks the
    // connect back up from here, on this thread, once they are done.
    m_pendingConnect = std::make_unique<PendingConnect>();
    m_pendingConnect->mp = mp;
    m_pendingConnect->dc = dc;
    m_pendingConnect->tc = tc;
    m_pendingConnect->actualNumRx = actualNumRx;
    m_pendingConnect->generation = ++m_connectGeneration;
    beginDspSetup();
}

void Hl2Backend::beginDspSetup()
{
    if (!m_pendingConnect)
        return;

    const int actualNumRx = m_pendingConnect->actualNumRx;
    // The TX chain counts as a setup step only in the TXA build, which opens a
    // WDSP transmit channel (the costliest cold-cache step). The phasing build
    // only designs two FIR kernels.
    const int total = actualNumRx + (AETHER_HL2_TX_TXA ? 1 : 0);

    // The chains to open, snapshotted on THIS thread. m_rx is GUI-thread-only
    // (see its declaration), so the I/O thread gets a plain vector of the
    // pointers it may touch and never the container they came out of.
    std::vector<Hl2RxDsp*> chains;
    chains.reserve(static_cast<std::size_t>(actualNumRx));
    std::vector<Hl2RxDsp::Config> configs;
    configs.reserve(static_cast<std::size_t>(actualNumRx));
    for (int i = 0; i < actualNumRx; ++i) {
        Receiver& r = m_rx[static_cast<std::size_t>(i)];
        Hl2RxDsp::Config dc = m_pendingConnect->dc;
        dc.mode = modeFromString(r.mode);
        // Passband through dspFilterHz(), which folds in the CW BFO (#4914).
        std::tie(dc.filterLowHz, dc.filterHighHz) = dspFilterHz(r);
        // The AGC pair, same as the other two Config assembly sites
        // (createPanadapter and the zoom rebuild). Without it every channel
        // opened on Config's defaults and stayed there until pushInitialState()
        // ran at linkUp, so a restored AGC did not reach the DSP for the first
        // second of audio — found as "the first second has the wrong AGC"
        // rather than as a restore bug, which is why the three sites should
        // look identical.
        dc.agcMode = wdspAgcMode(r.agcMode);
        dc.maximumAgcGainDb = m_dbRef.agcCeilingDb(r.agcThresholdDb);
        chains.push_back(r.dsp);
        configs.push_back(dc);
    }

    emit dspSetupProgress(tr("Preparing the receive chain…"), 0, total);
    // MIRRORED TO THE LOG, because dspSetupProgress has exactly one consumer in
    // the tree and it is MainWindow — so the phase is visible to an operator
    // watching a dialog and invisible to everyone else, which is why a headless
    // run showed nothing between here and the wire. (#5413.)
    qCInfo(lcHl2) << "HL2 DSP setup: opening" << total << "receive chain(s)";
    m_pendingConnect->clock.start();
    armDspSetupWatchdog();

    const Hl2TxDsp::Config tc = m_pendingConnect->tc;
    Hl2TxDsp* txDsp = m_txDsp;
    // QPointer guards a backend destroyed mid-build. The cross-thread reads
    // are sound because ~Hl2Backend() blocks on the I/O thread, so this lambda
    // always finishes first; QPointer is not thread-safe, so non-blocking
    // teardown would need another mechanism (docs/HERMES.md §22.4).
    QPointer<Hl2Backend> self(this);

    QMetaObject::invokeMethod(chains.empty() ? static_cast<QObject*>(txDsp)
                                             : static_cast<QObject*>(chains.front()),
        [self, chains, configs, tc, txDsp, total] {
        // ---- I/O THREAD ----
        //
        // Serial rather than parallel on purpose, and it is not about ordering:
        // FFTW's planner is not thread-safe, and Hl2Spectrum builds a plan in
        // its constructor. Two of these at once corrupt the planner's state.
        DspSetupResult result;
        result.rxOk.resize(chains.size(), false);
        result.rxChannelId.resize(chains.size(), -1);
        result.rxErr.resize(chains.size());
        for (std::size_t i = 0; i < chains.size(); ++i) {
            if (self) {
                const int done = static_cast<int>(i);
                // PURE STAGE TEXT — no counter. The label renders the fraction
                // from done/total (MainWindow::wireBackendSeam), so numbering
                // here too put two fractions over two denominators on screen:
                // "Preparing receiver 1 of 2… (1 of 3)".
                const QString stage = tr("Preparing the receive chain…");
                QMetaObject::invokeMethod(self, [self, stage, done, total] {
                    if (self)
                        emit self->dspSetupProgress(stage, done, total);
                }, Qt::QueuedConnection);
            }
            std::string err;
            result.rxOk[i] = chains[i]->configure(configs[i], &err);
            result.rxErr[i] = err;
            if (result.rxOk[i])
                result.rxChannelId[i] = chains[i]->wdspChannelId();
            else
                break;   // the GUI thread trims from here; opening past it is waste
        }
        // Announced only in the build where it is a real step -- see `total`
        // above. In the phasing build this stays silent, because announcing a
        // step that is over before the label repaints is worse than saying
        // nothing.
        if (AETHER_HL2_TX_TXA && self) {
            const QString stage = tr("Preparing the transmit chain…");
            const int done = static_cast<int>(chains.size());
            QMetaObject::invokeMethod(self, [self, stage, done, total] {
                if (self)
                    emit self->dspSetupProgress(stage, done, total);
            }, Qt::QueuedConnection);
        }
        if (txDsp)
            result.txOk = txDsp->configure(tc, &result.txErr);

        // ---- back to the GUI thread ----
        QMetaObject::invokeMethod(self, [self, result] {
            if (self)
                self->finishDspSetup(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Hl2Backend::armDspSetupWatchdog()
{
    if (!m_dspSetupWatchdog) {
        m_dspSetupWatchdog = new QTimer(this);
        m_dspSetupWatchdog->setSingleShot(true);
        connect(m_dspSetupWatchdog, &QTimer::timeout, this,
                &Hl2Backend::onDspSetupWatchdog);
    }
    m_dspSetupWatchdog->start(
        static_cast<int>(AetherSDR::hl2::dspSetupNextCheckMs(0)));
}

void Hl2Backend::onDspSetupWatchdog()
{
    if (!m_pendingConnect) {
        return;               // finished between the fire and this slot
    }
    const qint64 elapsed = m_pendingConnect->clock.elapsed();
    switch (AetherSDR::hl2::dspSetupAction(elapsed)) {
    case AetherSDR::hl2::DspSetupAction::None:
        // Fired early (a timer can). Look again at the point that matters.
        m_dspSetupWatchdog->start(
            static_cast<int>(AetherSDR::hl2::dspSetupNextCheckMs(elapsed)));
        return;
    case AetherSDR::hl2::DspSetupAction::Warn:
        // NOT a failure. A machine's first WDSP open measures its FFT plans
        // rather than loading them and is legitimately slow (#5052) — 98 s on a
        // quiet machine, 188 s under load — so this says so and keeps waiting;
        // the alternative is failing a connect that is working. It repeats,
        // because the wait it is reporting can be minutes long.
        qCWarning(lcHl2) << "HL2 DSP setup: still opening after" << elapsed
                         << "ms — a first open on this machine can be slow;"
                         << "will fail at"
                         << AetherSDR::hl2::kDspSetupFailMs / 1000 << "s";
        m_dspSetupWatchdog->start(
            static_cast<int>(AetherSDR::hl2::dspSetupNextCheckMs(elapsed)));
        return;
    case AetherSDR::hl2::DspSetupAction::Fail:
        break;
    }

    qCWarning(lcHl2) << "HL2 DSP setup: gave up after" << elapsed << "ms";

    // Invalidate, do not tear down: the I/O thread may be inside configure()
    // on these chains. Bump the generation and leave m_pendingConnect set, so
    // finishDspSetup()'s stale branch releases the chains (else WDSP's 32-slot
    // pool leaks) and connectRadio() keeps queuing behind the build (#5415).
    ++m_connectGeneration;
    // Before the emits, not after: a connectionError handler can re-enter this
    // object, and the stale branch must see this flag whatever it does.
    m_pendingConnect->finishSignalled = true;
    invalidateTxDspConfiguration();
    emit connectionError(
        tr("Hermes-Lite 2: the DSP setup did not finish within %1 seconds")
            .arg(AetherSDR::hl2::kDspSetupFailMs / 1000));
    // So a caller waiting on the phase is released rather than left hanging on
    // a signal that now never comes. The build is still running; the flag above
    // keeps the stale branch from emitting this a second time.
    emit dspSetupFinished();
}

void Hl2Backend::finishDspSetup(const DspSetupResult& result)
{
    // Stopped on EVERY exit below, including the two early returns — a timer
    // left running past the phase it measures would fail a connect that had
    // already succeeded.
    if (m_dspSetupWatchdog) {
        m_dspSetupWatchdog->stop();
    }
    if (!m_pendingConnect) {
        return;
    }
    qCInfo(lcHl2) << "HL2 DSP setup: chains open after"
                  << m_pendingConnect->clock.elapsed() << "ms";
    // A disconnect, or a second connect, arrived while the chains were opening.
    // The wire was never started, so there is nothing to stop — but the chains
    // ARE open, and leaving them open would leak WDSP channels out of the
    // 32-slot pool on every abandoned connect.
    if (m_pendingConnect->generation != m_connectGeneration) {
        qCInfo(lcHl2) << "HL2: connect superseded while the DSP was opening —"
                      << "releasing the chains it built";
        // Read before the reset: the phase watchdog may already have ended the
        // phase for a caller that could not wait for this moment.
        const bool alreadyFinished = m_pendingConnect->finishSignalled;
        m_pendingConnect.reset();
        // Safe to block inside here: the build has finished, so the I/O thread
        // is back at its event loop and publishIoDsps() returns promptly.
        tearDownReceivers();
        if (!alreadyFinished) {
            emit dspSetupFinished();
        }
        // A connect that arrived mid-build has been waiting for exactly this.
        if (m_queuedConnect) {
            const RadioConnectRequest queued = *m_queuedConnect;
            m_queuedConnect.reset();
            connectRadio(queued);
        }
        return;
    }

    MetisClient::Params mp = m_pendingConnect->mp;
    const int actualNumRx = m_pendingConnect->actualNumRx;
    m_pendingConnect.reset();

    // m_rx STILL HOLDS actualNumRx RECEIVERS. Worth stating, because the loop
    // below indexes it with a count snapshotted in beginDspSetup() and the two
    // phases no longer run back to back — event-loop turns pass between them.
    // Nothing can resize it in that window: the only two paths that do are
    // createPanadapter() and removePanadapter(), and both return early unless
    // m_connected, which is set from linkUp — that is, only after the wire this
    // function has not started yet. A connect or a disconnect landing here is
    // the generation counter's job and is handled above.
    for (int i = 0; i < actualNumRx; ++i) {
        const bool dspOk = result.rxOk[static_cast<std::size_t>(i)];
        const std::string& err = result.rxErr[static_cast<std::size_t>(i)];
        if (!dspOk) {
            // Receiver 0 failing is a failed connect. A LATER receiver failing
            // is not: the radio works, there is simply one fewer panadapter, and
            // refusing the whole session over it would be a worse outcome than
            // the degradation. Trim to what opened and carry on.
            if (i == 0) {
                invalidateTxDspConfiguration();
                // Withdraw every receiver's meter: buildReceivers() declared one
                // per receiver, none will ever report, and a refused connect
                // emits no disconnected() for RadioModel to wipe the catalogue.
                // The chains are left for the next buildReceivers(). Looked up
                // through the map (the ddc<->ui identity is not an invariant;
                // see Hl2Receivers.h); ui 0's meter is a no-op here.
                for (int k = 0; k < actualNumRx; ++k) {
                    if (const Hl2ReceiverIds* gone = m_ids.byDdc(k)) {
                        withdrawSliceLevelMeter(gone->uiNumber);
                    }
                }
                emit connectionError(
                    QStringLiteral("HL2 DSP: %1").arg(QString::fromStdString(err)));
                emit dspSetupFinished();
                return;
            }
            qCWarning(lcHl2) << "HL2: receiver" << i << "DSP failed —"
                             << QString::fromStdString(err)
                             << "; running" << i << "receiver(s)";
            // Withdraw, then destroy, as in removePanadapter() and
            // releaseReceiverDsps(): the chains are already published to the
            // sample path, and deleteLater() lets the I/O thread close their
            // WDSP channel and FFTW plan. Meters are withdrawn for receivers
            // i..actualNumRx-1 (receiver i opened and declared one), before
            // m_ids.truncate(i), by UI number from the map.
            for (int k = i; k < actualNumRx; ++k) {
                if (const Hl2ReceiverIds* gone = m_ids.byDdc(k)) {
                    withdrawSliceLevelMeter(gone->uiNumber);
                }
            }
            std::vector<Hl2RxDsp*> doomed;
            for (int k = i; k < actualNumRx; ++k) {
                if (Hl2RxDsp* d = m_rx[static_cast<std::size_t>(k)].dsp) {
                    doomed.push_back(d);
                    m_rx[static_cast<std::size_t>(k)].dsp = nullptr;
                }
            }
            m_rx.resize(static_cast<std::size_t>(i));
            publishIoDsps();
            for (Hl2RxDsp* d : doomed) {
                d->disconnect(this);
                d->deleteLater();
            }
            // The wire must carry the TRIMMED count. Leaving mp.numRx at the
            // requested value made the radio send slots nothing was listening
            // on, and made blocks.size() disagree with the fan-out list.
            mp.numRx = i;
            // truncate(), NOT reset(i): the receivers that DID open have their
            // dspChannel and analyzerId recorded already, and reset() would put
            // them back to -1 — losing exactly the ids that cannot be re-derived
            // from the index.
            m_ids.truncate(i);
            break;
        }
        // The WDSP channel id is knowable only after the open, and it is
        // whatever the shared 32-slot pool had free. Carrying it back from the
        // I/O thread rather than deriving it from i is the entire point of the
        // index-space map.
        if (auto* ids = m_ids.mutableByDdc(i)) {
            ids->dspChannel = result.rxChannelId[static_cast<std::size_t>(i)];
            ids->analyzerId = i;   // Hl2Spectrum is per-receiver and owned by it
        }
    }
    if (!result.txOk)
        qWarning() << "Hl2Backend: TX DSP unavailable —"
                   << QString::fromStdString(result.txErr) << "(receive is unaffected)";

    // ---- and only now, the wire ----
    //
    // Every DSP chain is open and configured, so from the first EP6 packet there
    // is somewhere for the samples to go, and the I/O thread is free to pace EP2
    // without a multi-second WDSP open standing in front of it. See the ordering
    // note above buildReceivers().
    //
    // Blocking: start() constructs the QUdpSocket, which must take the I/O
    // thread's affinity, and we need to know whether the bind succeeded.
    bool started = false;
    QMetaObject::invokeMethod(m_metis, [this, &mp, &started] {
        started = m_metis->start(mp);
    }, Qt::BlockingQueuedConnection);
    if (!started) {
        tearDownReceivers();   // do not leave WDSP channels open on a failed connect
        emit connectionError(QStringLiteral("HL2: could not open the UDP socket"));
        emit dspSetupFinished();
        return;
    }

    // Assert a known TX drive rather than inheriting whatever the board held.
    // ZERO is deliberate: this backend has no drive-level control wired to the
    // UI yet, so anything higher would be an un-commanded power level chosen by
    // a default. An operator raising it explicitly is the only way it should go up.
    setTxDriveLevel(0);
    emit dspSetupFinished();

    // Initial slice/pan state is published from the linkUp handler above, once
    // connected() has fired and RadioModel has finished staging the old session.
}

void Hl2Backend::invalidateTxDspConfiguration()
{
    if (!m_txDsp) {
        return;
    }
    if (!m_ioThread || !m_ioThread->isRunning()
        || QThread::currentThread() == m_txDsp->thread()) {
        m_txDsp->invalidateConfiguration();
        return;
    }
    // Serializes after an in-flight configure and before any later read-back.
    // Never wait here: a cancelled cold WDSP open can still take minutes.
    QMetaObject::invokeMethod(m_txDsp, [dsp = m_txDsp] {
        dsp->invalidateConfiguration();
    }, Qt::QueuedConnection);
}

void Hl2Backend::disconnectRadio()
{
    retirePcmStreams();
    invalidateTxDspConfiguration();
    // Invalidate any DSP build still in flight. Without this, a disconnect
    // during the opens would be followed by finishDspSetup() starting a wire
    // for a session the operator has already left. The build itself cannot be
    // cancelled — WDSP's OpenChannel does not return early — so it runs to
    // completion on the I/O thread and finishDspSetup() releases what it built.
    ++m_connectGeneration;
    // The phase watchdog goes with it: the operator has left, so a later fire
    // would report a failure for a connect nobody is waiting on.
    if (m_dspSetupWatchdog) {
        m_dspSetupWatchdog->stop();
    }
    // The stream-free poller keeps its target across a disconnect ON PURPOSE.
    // The operator is most likely to want the radio's temperature and PTT state
    // in the moments after a session ends badly, and that is exactly when the
    // in-band path has nothing. The cadence rule decides whether it actually
    // polls: NotConnected polls only while something is reading the health
    // snapshot, so a disconnect the operator walks away from goes quiet on its
    // own within kHealthDemandWindowMs.

    // And a connect PARKED BEHIND that build is stale for the same reason: the
    // operator has since asked to be disconnected. Leaving it here made
    // finishDspSetup()'s supersede branch re-drive it — a second full build
    // ending in MetisClient::start(), for a session nobody is in. Measured:
    // connect, connect, disconnect gave two dspSetupFinished and a running wire.
    m_queuedConnect.reset();

    if (m_metis)
        // Queued: serialises behind whatever the I/O thread is doing.
        QMetaObject::invokeMethod(m_metis, "stop");   // linkDown -> disconnected()
}

bool Hl2Backend::isConnected() const
{
    return m_connected;
}

QString Hl2Backend::sliceMeterName(int uiNumber)
{
    // The first receiver keeps the bare "SLC:LEVEL" the single-receiver backend
    // published, so every existing consumer (MeterModel bindings, the automation
    // bridge, the health dialog) keeps working untouched. Only the receivers
    // that did not exist before get a suffix.
    return uiNumber == 0 ? QStringLiteral("SLC:LEVEL")
                         : QStringLiteral("SLC%1:LEVEL").arg(uiNumber);
}

int Hl2Backend::sliceLevelMeterIndex(int uiNumber)
{
    return uiNumber == 0 ? 1 : kSliceLevelMeterBase + uiNumber;
}

void Hl2Backend::defineSliceLevelMeter(int uiNumber)
{
    // Receiver 0's is defineMeters()' business — see the header. Declaring it
    // here as well would allocate a SECOND "SLC"/"LEVEL" definition for slice 0
    // and MeterModel's per-slice cache keeps the last one, so the bare
    // "SLC:LEVEL" updates would start resolving to whichever index won.
    if (uiNumber <= 0) {
        return;
    }
    MeterDef d;
    d.index = sliceLevelMeterIndex(uiNumber);
    d.source = QStringLiteral("SLC");
    // THE FIELD THAT WAS MISSING. The suffix in sliceMeterName() is a transport
    // detail; this is the identity. MeterModel::defineMeter keys its per-slice
    // cache on source == "SLC" exactly, so a definition calling itself "SLC1"
    // would be accepted, appear in allMeters(), and still never key the cache —
    // it would look fixed and change nothing.
    d.sourceIndex = uiNumber;
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level, receiver %1").arg(uiNumber + 1);
    emit meterDefined(d);
}

void Hl2Backend::withdrawSliceLevelMeter(int uiNumber)
{
    if (uiNumber <= 0) {
        return;
    }
    // MeterModel::removeMeter purges m_sLevelIdxBySlice for this index, so a
    // closed receiver stops appearing in the meter list instead of freezing at
    // its last reading.
    emit meterRemoved(sliceLevelMeterIndex(uiNumber));
}

void Hl2Backend::setSliceFrequency(int sliceId, double hz)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;   // a slice that is not running: see ddcForSlice
    r->sliceFreqHz = hz;

    // Keep the NCO — and therefore the panadapter centre — where it is, and put
    // the slice at an offset inside the passband. Only when the target would
    // fall outside the usable window does the NCO move, and then it re-centres
    // on the target.
    //
    // Before this the slice frequency WAS the NCO, so the pan centre tracked
    // every tune and the whole display slid under the cursor on each click.
    // That also made a slice offset from centre unrepresentable, which is what
    // a Flex-shaped UI assumes it can do.
    const double halfSpanHz = static_cast<double>(m_sampleRateHz) / 2.0;
    // Stay clear of the band edges: the passband rolls off there, and a slice
    // parked in the roll-off would be attenuated for no visible reason.
    const double usableHz = halfSpanHz * kUsablePassbandFraction;
    // The window is judged on where the receiver LISTENS (dial + RIT on the
    // transmit receiver), so a RIT offset past the edge moves the NCO too.
    const double tunedHz = rxTunedHz(*r);
    const bool ritApplies = (tunedHz != r->sliceFreqHz);
    const bool outsideWindow = std::abs(tunedHz - r->ncoHz) > usableHz;
    // A move RIT alone caused is undone when RIT stops applying to this
    // receiver (cleared, zeroed, or transmit moved away) — the NCO comes back
    // to the dial, or the pan centre stays offset by the old RIT for good.
    const bool ritReturn = !outsideWindow && !ritApplies && r->ncoMovedForRit;
    if (outsideWindow || ritReturn) {
        // Judged against the NCO BEFORE it moves: did the dial alone fit?
        r->ncoMovedForRit = outsideWindow && ritApplies
            && std::abs(r->sliceFreqHz - r->ncoHz) <= usableHz;
        r->ncoHz = tunedHz;
        if (m_metis)
            // THIS receiver's NCO register, not RX1's. The two-argument overload
            // is the whole reason the receivers can sit on different bands.
            QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
                Q_ARG(int, ddc),
                Q_ARG(std::uint32_t, ncoCommandHz(tunedHz)));
        // Notch centres are measured from the NCO, so moving it without saying
        // so leaves every notch parked at its old RF frequency — the operator
        // tunes across the band and the notches follow them, which is precisely
        // the behaviour a TRACKING notch exists to avoid.
        pushNotchTune(*r);
        // The averaged panadapter is on the old axis too.
        dropPanAverage(*r);
    }

    // shift = slice - NCO: the wire places frequency F at -(F - NCO), so this
    // puts the slice at baseband (hl2_shift_test). rxShiftHz(), not
    // dspShiftHz(): in CW the detector's zero is a pitch away from the slice.
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(*r)));

    // The TX NCO (addr 0x01) is a separate register that does not follow the
    // RX DDC; unset, it keeps its last value (zero after boot). It follows the
    // transmit-owning receiver only, not the last tuned slice. Sent even when
    // TX is disabled: it keys nothing.
    if (ddc == m_txDdc) {
        setTxFrequency(r->sliceFreqHz);
        // Per-band memory follows the TRANSMIT-owning receiver (RFC #4603
        // PR 3): leaving a band records its LNA/drive, entering one applies
        // what it remembered. Keyed to the TX slice because drive and LNA are
        // radio-wide hardware — a second receiver browsing another band must
        // not drag the transmitter's setpoints around.
        applyPerBandStateFor(r->sliceFreqHz, "tune");
    }

    // The companion filter board is band hardware in the ANTENNA path, shared by
    // every receiver, and on transmit it is what keeps the harmonics legal.
    // Change-gated inside, so tuning within a band sends nothing.
    applyBandFilter("tune");
    // Tuning one receiver can take the set on or off a shared band, which is
    // what the WIDE indicator reports.
    publishWideState();

    emitSliceState(ddc);
    emitPanState(ddc);
    notifyOperatingStateChanged();
}

void Hl2Backend::setSliceMode(int sliceId, const QString& requested)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;

    // Same gate as applyRestoredState(): aliases canonicalise so the combo can
    // show them (#5580), and a mode modeFromString() does not map is refused,
    // since it would demodulate as USB; the re-publish puts SliceModel's
    // optimistic mode back. Cannot weaken the key refusal: receiveOnlyModes
    // lists each alias pair both ways or neither (hl2_mode_vocabulary_test).
    if (!isKnownModeString(requested)) {
        qCWarning(lcHl2) << "HL2: refusing mode" << requested
                         << "- this radio demodulates only" << publishedModeStrings();
        emitSliceState(ddc);
        return;
    }
    const QString mode = canonicalOfferedMode(requested);

    const QString previous = r->mode;
    r->mode = mode;
    const WdspChannel::Mode wdsp = modeFromString(mode);

    // The passband follows the mode, since no radio-side DSP echoes one back.
    // Adopted only on a mode change, so an operator's filter edit survives
    // until the next mode change.
    if (!previous.isEmpty() && previous.compare(mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        r->filterLowHz  = lo;
        r->filterHighHz = hi;
    }

    // Mode first, then passband, re-pushed on every mode set: in WDSP the NBP
    // filter edges select the sideband, and SetRXAMode/SetTXAMode rebuild that
    // stage, discarding any filter applied before them.
    if (r->dsp) {
        const auto [dspLo, dspHi] = dspFilterHz(*r);
        QMetaObject::invokeMethod(r->dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        QMetaObject::invokeMethod(r->dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, dspLo), Q_ARG(double, dspHi));
        // The BFO is part of the mode, so entering or leaving CW moves the
        // shift as well as the passband. Without this the detector's zero would
        // still be sitting on the marker from the previous mode: CW would tune
        // a pitch low, and coming back OUT of CW would leave every other mode
        // tuned a pitch high, which reads as "the radio is off frequency" long
        // after the operator has left CW behind.
        QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(*r)));
    }

    // The transmit sideband follows the slice. Without this, switching to LSB
    // would receive on the lower sideband and still transmit on the upper.
    //
    // The passband half of that was missing entirely: Hl2TxDsp::setFilter
    // existed and had no caller, so the TX chain ran on its construction-time
    // 300..2700 for every mode of the session. Same ordering rule as RX.
    //
    // Only the TX receiver's mode reaches the modulator: putting receiver 3 into
    // CW to listen for beacons must not switch the transmitter out of SSB.
    if (m_txDsp && ddc == m_txDdc) {
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        pushTxPassband(mode);
    }
    emitSliceState(ddc);
    notifyOperatingStateChanged();
}

void Hl2Backend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
    r->filterLowHz = lowHz;
    r->filterHighHz = highHz;
    if (r->dsp) {
        // The operator's cuts are carrier-relative; the demodulator's are not.
        // Pushing lowHz/highHz straight through would be correct for every mode
        // except CW and silently wrong there — a dragged filter edge would move
        // the passband a pitch away from where the operator dropped it.
        const auto [dspLo, dspHi] = dspFilterHz(*r);
        QMetaObject::invokeMethod(r->dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, dspLo), Q_ARG(double, dspHi));
    }
    emitSliceState(ddc);
    notifyOperatingStateChanged();
}

void Hl2Backend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
    const QString m = mode.trimmed().toLower();

    // AGC threshold: the slice's 0..100 maps to 0..60 dB of WDSP max gain, so
    // the default 65 lands at 39 dB. Measured on WWV 10 MHz USB: clean through
    // 40 dB, clipping hard by 50 dB.
    // Validated on the way in so capture only stores what restore accepts; an
    // unknown mode string leaves the mode unchanged.
    if (!m.isEmpty() && isKnownAgcModeString(m))
        r->agcMode = m;
    else if (!m.isEmpty())
        qCWarning(lcHl2) << "HL2: ignoring unknown AGC mode" << mode
                         << "- keeping" << r->agcMode;
    r->agcThresholdDb = qBound(0, thresholdDb, 100);
    // THE REMEMBERED PAIR IS THE LAST ONE THE OPERATOR SET, on whichever
    // receiver. Capture used to read rx(m_txDdc) instead, which split the model:
    // a change on RX2 fired the notify, then the debounced capture rewrote the
    // document with RX1's unchanged pair, so the change that TRIGGERED the
    // capture was not the change that got captured — and the next launch seeded
    // every receiver from it. Flat restore is the deliberate design (see
    // seedReceiverAgc); this makes the capture side agree with it.
    m_agcMode = r->agcMode;
    m_agcThresholdDb = r->agcThresholdDb;
    // WDSP IS TOLD WHAT THE RECEIVER NOW HOLDS, not what the caller asked for.
    // Deriving from `m` meant a refused mode still reached the DSP as
    // wdspAgcMode()'s medium fallback while r->agcMode, the applet echo and the
    // persisted document all kept the old value — the same four-way
    // disagreement the check above exists to end, with the DSP as the one
    // surface nobody can see. It also fixes the empty-mode call (a
    // threshold-only change), which used to send medium over whatever mode the
    // receiver was actually running.
    const double ceilingDb = m_dbRef.agcCeilingDb(r->agcThresholdDb);
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgcMode(r->agcMode)), Q_ARG(double, ceilingDb));
    emitSliceState(ddc);
    // The capture half. setSliceMode/setSliceFilter next door have always said
    // this and AGC never did, so the operator's AGC was the one control on this
    // radio that moved, took effect, and was gone by the next launch (#4909).
    notifyOperatingStateChanged();
}

void Hl2Backend::setSliceNoiseBlanker(int sliceId, bool on, int level)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
    // PER-RECEIVER, unlike the notches. A notch is a fact about a frequency and
    // therefore about the radio; a blanker setting is a judgement about how
    // aggressively to gate one receiver's audio, and two receivers on different
    // bands can legitimately disagree. The slice model already holds it
    // per-slice, so honouring that is also what stops the second receiver's
    // toggle from moving the first one's.
    r->nbOn = on;
    r->nbLevel = qBound(0, level, 100);
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setNoiseBlanker", Qt::QueuedConnection,
            Q_ARG(bool, r->nbOn), Q_ARG(int, r->nbLevel));
}

void Hl2Backend::setSliceSquelch(int sliceId, bool on, int level)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
    // Per receiver, like the blanker. Not filtered by mode: the pair is stored
    // whatever the mode, and WdspChannel decides which stage (if any) carries
    // it and moves it on every mode change, so the result does not depend on
    // the order mode and squelch were set in.
    r->squelchOn = on;
    r->squelchLevel = qBound(0, level, 100);
    pushSquelch(*r);
    emitSliceState(ddc);
}

void Hl2Backend::setSliceAudioMute(int sliceId, bool mute)
{
    Receiver* r = rx(ddcForSlice(sliceId));
    if (!r || r->audioMuted == mute)
        return;
    r->audioMuted = mute;
    // Drop what this receiver had already queued for the mix. Those blocks are
    // older than the mute and would play out after it — a mute with a tail is
    // indistinguishable from a mute that did not take.
    const int ddc = ddcForSlice(sliceId);
    if (ddc >= 0 && static_cast<std::size_t>(ddc) < m_mixPending.size())
        m_mixPending[static_cast<std::size_t>(ddc)].clear();
    qCDebug(lcHl2) << "HL2: slice" << sliceId << (mute ? "muted" : "unmuted");
    emitSliceState(ddc);
}

void Hl2Backend::setSliceAudioGain(int sliceId, int gainPercent)
{
    Receiver* r = rx(ddcForSlice(sliceId));
    if (!r)
        return;
    // 0..100 -> 0.0..1.0 LINEAR, matching what a Flex does with audio_level
    // rather than inventing a dB curve here. Unity at 100 keeps a single
    // unmuted slice at exactly the level it has today.
    const float scaled = std::clamp(gainPercent, 0, 100) / 100.0f;
    if (r->audioGain == scaled) {
        return;
    }
    r->audioGain = scaled;
    emitSliceState(ddcForSlice(sliceId));
}

void Hl2Backend::setSliceAudioPan(int sliceId, int panPercent)
{
    Receiver* r = rx(ddcForSlice(sliceId));
    if (!r)
        return;
    r->audioPanPercent = std::clamp(panPercent, 0, 100);
}

void Hl2Backend::setActiveSlice(int sliceId)
{
    const int ddc = ddcForSlice(sliceId);
    if (!rx(ddc)) {
        qCWarning(lcHl2) << "HL2: cannot activate slice" << sliceId << "— no such receiver";
        return;
    }
    if (ddc == m_activeDdc) {
        // Confirm rather than return silently, for the same reason setTxSlice
        // does: the asker may believe otherwise, and an unanswered request
        // leaves that disagreement standing.
        emitSliceState(ddc);
        return;
    }

    const int previous = m_activeDdc;
    m_activeDdc = ddc;

    // BOTH slices, old and new. Publishing only the new one would leave two
    // slices claiming to be active, which is the bug this exists to fix — the
    // client would keep resolving to whichever it looked at first, and the RX
    // Controls applet would stay pointed at a receiver the operator had left.
    emitSliceState(previous);
    emitSliceState(ddc);
}

void Hl2Backend::setTxSlice(int sliceId)
{
    const int ddc = ddcForSlice(sliceId);
    const Receiver* r = rx(ddc);
    if (!r) {
        qCWarning(lcHl2) << "HL2: cannot move transmit to slice" << sliceId
                         << "— no such receiver";
        return;
    }
    if (ddc == m_txDdc) {
        // Already ours — but CONFIRM it rather than returning silently. The
        // asker may believe otherwise (a client that restored its own state, a
        // model seeded from somewhere else), and a request answered with
        // nothing leaves that disagreement standing. Republishing costs one
        // signal and makes the backend's answer the one that survives.
        emitSliceState(ddc);
        return;
    }

    const int previous = m_txDdc;
    m_txDdc = ddc;
    qCInfo(lcHl2) << "HL2: transmit moves from DDC" << previous << "to" << ddc
                  << "(slice" << sliceId << ")";

    // Everything transmit-shaped follows the new owner. The TX NCO is a separate
    // register from any RX DDC and nothing reads it back, so leaving it on the
    // old receiver's frequency would key on the wrong band with nothing to say
    // so — the exact failure §14 records from the first bring-up.
    setTxFrequency(r->sliceFreqHz);
    // Band memory follows TRANSMIT (PR #4619 review, Ozy311 finding 2):
    // moving TX from a 40 m receiver to a 20 m receiver must apply 20 m's
    // remembered drive/LNA and re-key later edits — the band key belongs to
    // the transmit-owning slice, not to whichever slice tuned last.
    applyPerBandStateFor(r->sliceFreqHz, "tx slice move");
    if (m_txDsp) {
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(r->mode)));
        pushTxPassband(r->mode);
    }
    // The band filter's keyed-TX override follows m_txDdc, so re-evaluate it.
    applyBandFilter("tx slice");
    publishWideState();
    // RIT belongs to the transmit receiver, so it leaves one and joins the other.
    // On the new receiver this repeats the setTxFrequency() and band-memory
    // writes above via setSliceFrequency(): the same values, and
    // applyPerBandStateFor() returns early on an unchanged band key — a
    // deliberate re-run of the whole tune, not a missing gate.
    if (m_ritOn && m_ritHz != 0) {
        logRitScope();
        retuneReceiver(previous);
        retuneReceiver(ddc);
    }

    // Republish BOTH slices: the one that lost transmit and the one that gained
    // it. Publishing only the new one would leave the old indicator lit, and two
    // slices claiming transmit is worse than none — the interlock would find
    // whichever was selected.
    emitSliceState(previous);
    emitSliceState(ddc);
}

// Manual notch filters: WDSP notches on this host that behave like a Flex TNF
// (absolute RF frequency, stay put while tuning). Every mutation applies to
// every receiver: a notch belongs to the band, not one slice.

int Hl2Backend::notchIndexFor(int notchId) const
{
    for (std::size_t index = 0; index < m_notches.size(); ++index) {
        if (m_notches[index].id == notchId)
            return static_cast<int>(index);
    }
    return -1;
}

void Hl2Backend::pushNotchTune(const Receiver& r)
{
    // The true NCO frequency, not ncoCommandHz(). WDSP places a notch at
    // fcenter - (tunefreq + shift), with shift in the commanded domain and
    // centres in true RF Hz; the residual is e * (fcenter - ncoTrue), under
    // 10 Hz at ±50 ppm and a 384 kHz span edge, against a 50 Hz notch floor.
    // ncoCommandHz() would leave e * fcenter (~7 Hz at 7 MHz).
    if (r.dsp)
        QMetaObject::invokeMethod(r.dsp, "setNotchTuneFrequency", Qt::QueuedConnection,
            Q_ARG(double, r.ncoHz));
}

void Hl2Backend::pushNoiseBlanker(const Receiver& r)
{
    if (!r.dsp)
        return;
    // Sent even when OFF, and that is the point: a chain rebuilt while the
    // operator had the blanker off is already off, but a chain rebuilt after
    // they turned it off during a previous connect is not necessarily, and an
    // unconditional push is the only version with no such case to reason about.
    QMetaObject::invokeMethod(r.dsp, "setNoiseBlanker", Qt::QueuedConnection,
        Q_ARG(bool, r.nbOn), Q_ARG(int, r.nbLevel));
}

void Hl2Backend::pushSquelch(const Receiver& r)
{
    if (!r.dsp)
        return;
    // Sent even when OFF — see pushNoiseBlanker().
    QMetaObject::invokeMethod(r.dsp, "setSquelch", Qt::QueuedConnection,
        Q_ARG(bool, r.squelchOn), Q_ARG(int, r.squelchLevel));
}

void Hl2Backend::seedNotches(const Receiver& r)
{
    if (!r.dsp)
        return;
    // Tune frequency before the notches: the centres are absolute, so a notch
    // placed against a tune frequency of zero lands ~7 MHz away from where it
    // was asked for.
    pushNotchTune(r);
    // Replace, never append: this runs on every linkUp, including linkUps
    // after EP6 silence where the DSP objects survive, so seeding must be
    // idempotent or WDSP's positional notch indices drift from m_notches.
    QMetaObject::invokeMethod(r.dsp, "clearNotches", Qt::QueuedConnection);
    for (std::size_t index = 0; index < m_notches.size(); ++index) {
        const NotchRecord& notch = m_notches[index];
        QMetaObject::invokeMethod(r.dsp, "addNotch", Qt::QueuedConnection,
            Q_ARG(int, static_cast<int>(index)), Q_ARG(double, notch.centerHz),
            Q_ARG(double, notch.widthHz), Q_ARG(bool, notch.active));
    }
    QMetaObject::invokeMethod(r.dsp, "setNotchesEnabled", Qt::QueuedConnection,
        Q_ARG(bool, m_notchesEnabled));
}

void Hl2Backend::createNotch(double centerHz, double widthHz)
{
    if (centerHz <= 0.0 || !std::isfinite(centerHz) || !std::isfinite(widthHz))
        return;
    if (static_cast<int>(m_notches.size()) >= capabilities().maxNotchFilters)
        return;
    // Clamped to what the RX chain can actually produce, and reported back at
    // the clamped value — so the panadapter draws the notch the operator is
    // hearing rather than the one they asked for. WDSP would widen it silently.
    const double width = std::max(widthHz, Hl2RxDsp::kMinNotchWidthHz);

    NotchRecord notch;
    notch.id = m_nextNotchId++;
    notch.centerHz = centerHz;
    notch.widthHz = width;
    notch.active = true;
    // Append, so the new notch's WDSP index is the old size on every receiver.
    const int index = static_cast<int>(m_notches.size());
    m_notches.push_back(notch);

    for (const Receiver& r : m_rx) {
        if (!r.dsp)
            continue;
        // Re-assert the axis before every placement rather than trusting that
        // some earlier setup path did it. It is one cheap queued call that WDSP
        // no-ops when unchanged, and the failure it prevents is silent: a notch
        // measured from a stale tune frequency lands outside the passband and
        // is dropped without an error, while still reading back as present.
        pushNotchTune(r);
        QMetaObject::invokeMethod(r.dsp, "addNotch", Qt::QueuedConnection,
            Q_ARG(int, index), Q_ARG(double, notch.centerHz),
            Q_ARG(double, notch.widthHz), Q_ARG(bool, notch.active));
    }

    // The id is minted HERE, so nothing above knows about this notch until it
    // is told. Same contract as a Flex, where the radio assigns the id and
    // reports it back as status.
    AetherSDR::NotchDelta delta;
    delta.centerHz = notch.centerHz;
    delta.widthHz = notch.widthHz;
    delta.active = notch.active;
    emit notchChanged(notch.id, delta);
}

void Hl2Backend::setNotch(int notchId, const AetherSDR::NotchDelta& delta)
{
    const int index = notchIndexFor(notchId);
    if (index < 0)
        return;
    NotchRecord& notch = m_notches[static_cast<std::size_t>(index)];

    // depth and permanent are deliberately ignored rather than approximated.
    // A WDSP notch is a full null with no depth, and there is nowhere in an HL2
    // for a "permanent" notch to persist — capabilities().notchHasDepth is
    // false so the UI never offers the first, and the second is a Flex concept
    // the seam simply carries past us.
    if (delta.centerHz && std::isfinite(*delta.centerHz))
        notch.centerHz = *delta.centerHz;
    if (delta.widthHz && std::isfinite(*delta.widthHz))
        notch.widthHz = std::max(*delta.widthHz, Hl2RxDsp::kMinNotchWidthHz);
    if (delta.active)
        notch.active = *delta.active;

    // A combined centre+width delta lands as ONE edit here, which matters in a
    // way it does not on a Flex: each edit rebuilds the whole multi-bandpass
    // filter mask, and a panadapter drag delivers these ~30 times a second.
    // Nothing builds that combined delta yet — SpectrumWidget emits move and
    // width separately — so a diagonal drag still pays for two rebuilds. See
    // NotchDelta.h.
    for (const Receiver& r : m_rx) {
        if (r.dsp)
            QMetaObject::invokeMethod(r.dsp, "editNotch", Qt::QueuedConnection,
                Q_ARG(int, index), Q_ARG(double, notch.centerHz),
                Q_ARG(double, notch.widthHz), Q_ARG(bool, notch.active));
    }

    // Echo the APPLIED values, which may differ from what was asked (width
    // clamping). Reporting the request instead would let the overlay drift away
    // from the audio a little more with every drag.
    AetherSDR::NotchDelta applied;
    applied.centerHz = notch.centerHz;
    applied.widthHz = notch.widthHz;
    applied.active = notch.active;
    emit notchChanged(notch.id, applied);
}

void Hl2Backend::removeNotch(int notchId)
{
    const int index = notchIndexFor(notchId);
    if (index < 0)
        return;
    // Erase here and in WDSP by the SAME index, which is what keeps position
    // and identity in step: both sides close the gap, so every surviving
    // notch's index shifts down by one on both sides at once.
    m_notches.erase(m_notches.begin() + index);
    for (const Receiver& r : m_rx) {
        if (r.dsp)
            QMetaObject::invokeMethod(r.dsp, "removeNotch", Qt::QueuedConnection,
                Q_ARG(int, index));
    }
    emit notchRemoved(notchId);
}

void Hl2Backend::setNotchesEnabled(bool on)
{
    m_notchesEnabled = on;
    for (const Receiver& r : m_rx) {
        if (r.dsp)
            QMetaObject::invokeMethod(r.dsp, "setNotchesEnabled", Qt::QueuedConnection,
                Q_ARG(bool, on));
    }
}

// Intent ignored — the HL2's DDC window is independent of any slice.
void Hl2Backend::setPanCenter(const QString& panId, double hz, PanCenterIntent)
{
    // Moving the window means moving the DDC. The slice does NOT move with it —
    // that is the point of keeping the two separate — so its offset from the new
    // centre is recomputed and re-applied as a shift.
    if (hz <= 0.0)
        return;
    const int ddc = ddcForPan(panId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
    // A drag delivers a centre command every 33 ms and forwards every one. Skip
    // the ones that do not actually move the DDC rather than re-sending an
    // identical NCO bank ~30 times a second.
    if (hz == r->ncoHz)
        return;
    r->ncoHz = hz;
    // The operator placed the centre: it is theirs now, not RIT's to undo.
    r->ncoMovedForRit = false;
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
            Q_ARG(int, ddc),
            Q_ARG(std::uint32_t, ncoCommandHz(hz)));
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(*r)));
    // The NCO moved, so the notch axis has to move with it. See the note at the
    // matching site in setSliceFrequency.
    pushNotchTune(*r);
    // So has the panadapter's frequency axis. A drag delivers ~30 of these a
    // second, so while the operator drags, the display is effectively
    // unaveraged — which is right: an average of a moving axis is smear.
    dropPanAverage(*r);
    // Panning one receiver can move it onto another band, which changes what the
    // SHARED filter board should be doing. Re-evaluate across every receiver.
    applyBandFilter("pan");
    publishWideState();
    emitPanState(ddc);
}

void Hl2Backend::setPanBandwidth(const QString& panId, double hz)
{
    if (hz <= 0.0)
        return;
    // The DDC rate is a RADIO-WIDE register (0x00[25:24]), so a zoom on any one
    // panadapter re-spans them all. The pan id is validated rather than used to
    // select a target: a control for a receiver that is not running must still
    // be refused, but there is no per-receiver span for it to have changed.
    if (ddcForPan(panId) < 0)
        return;

    // Coalesce a zoom sweep. See kBandwidthThrottleMs: each span change is a
    // blocking WDSP rebuild on the thread that paces EP2, and a drag delivers
    // ~30 of them a second. Leading edge applies now so a discrete step is not
    // delayed; the rest are collapsed into one.
    if (!m_bandwidthThrottle) {
        m_bandwidthThrottle = new QTimer(this);
        m_bandwidthThrottle->setSingleShot(true);
        m_bandwidthThrottle->setInterval(kBandwidthThrottleMs);
        connect(m_bandwidthThrottle, &QTimer::timeout, this, [this] {
            if (m_pendingBandwidthHz <= 0.0)
                return;              // cooldown expired with nothing waiting
            const double pending = m_pendingBandwidthHz;
            m_pendingBandwidthHz = 0.0;
            applyPanBandwidth(pending);
            // Re-arm: a sweep still in progress must keep coalescing.
            m_bandwidthThrottle->start();
        });
    }

    if (m_bandwidthThrottle->isActive()) {
        m_pendingBandwidthHz = hz;   // superseded by any later request
        return;
    }

    applyPanBandwidth(hz);
    m_bandwidthThrottle->start();
}

void Hl2Backend::setPanRfGain(const QString& panId, int gainDb)
{
    // RF gain is the AD9866's LNA (0x0a[5:0]) and there is exactly ONE of those
    // behind every DDC, so this control is radio-wide however many panadapters
    // present it. The pan id is validated, not used to select a target, and the
    // echo below goes to EVERY pan — a slider that moved only the pane it was
    // dragged on would leave three others showing a gain the radio is not using.
    if (ddcForPan(panId) < 0)
        return;
    const int clamped = qBound(kLnaGainMinDb, gainDb, kLnaGainMaxDb);

    // ONLY the register write is redundant when the value has not moved. The
    // equality check used to return above everything below it, which made an
    // operator who set exactly the value already live invisible to the band
    // memory — and the one value guaranteed to be already live is the one a
    // connect param pinned. 20 m stored at -12, connect with lnaGainDb=20, and
    // the operator still on 20 m deliberately setting 20 ended no pin and
    // recorded no band, so the snapshot kept persisting -12. (#5402 review.)
    const bool moved = (clamped != m_lnaGainDb);
    if (moved) {
        applyLnaGainDb(clamped);
        qCInfo(lcHl2) << "HL2 LNA gain:" << m_lnaGainDb << "dB (requested" << gainDb << ")";
    }

    // The operator's gain belongs to the band they set it on (RFC #4603 PR 3).
    // This is also what ends a session pin: the value is now the operator's
    // own choice for this band, so the band memory is theirs to overwrite.
    // Choosing the value the session was pinned to is still choosing it.
    const bool endedPin = m_lnaSessionPin;
    m_lnaSessionPin = false;
    bool recordedBand = false;
    if (!m_currentBandKey.isEmpty()) {
        const auto stored = m_lnaDbByBand.constFind(m_currentBandKey);
        if (stored == m_lnaDbByBand.constEnd() || *stored != m_lnaGainDb) {
            m_lnaDbByBand.insert(m_currentBandKey, m_lnaGainDb);
            recordedBand = true;
        }
    }
    // A slider that moved nothing, ended no pin and changed no stored entry has
    // nothing to persist; notifying anyway would schedule a debounced store for
    // a no-op. Any of the three actually changing still notifies as before.
    if (moved || endedPin || recordedBand) {
        notifyOperatingStateChanged();
    }
}

void Hl2Backend::applyPanBandwidth(double hz)
{
    // Widening the window means running the DDC at a higher rate. There is no
    // continuous zoom here: the gateware offers four rates, so the request is
    // snapped to the nearest and the caller is told what it actually got via
    // emitPanState() at the end.
    const int rate = nearestIqSampleRateHz(hz);
    if (rate == m_sampleRateHz) {
        // Still re-publish. A zoom the hardware cannot honour must not leave the
        // display sitting on the operator's requested span — the model deferred
        // to us precisely so the view follows the radio, and re-emitting the
        // unchanged span is how the widget snaps back to what is real.
        //
        // The model's setter is change-gated, so RadioModel force-republishes for
        // a raw-spectrum backend on exactly this path; without that the emit here
        // is swallowed and the widget stays wider than the data. (#4470)
        emitAllPanState();
        return;
    }

    // THE COMMITTED RATE, NOT m_sampleRateHz. This is the rate a failed build
    // must put back, and m_sampleRateHz is not it while another crossing is in
    // flight: that one already moved it optimistically to a rate the register
    // has not been written with. Capturing it here made a failed second
    // crossing "restore" a rate the radio had never been commanded to.
    const int previousRate = m_rateLedger.committed();

    // A WIDER span may not fit the receivers that are running. Both axes cost
    // bandwidth, so zooming out with four receivers open can cross the link
    // budget where the same zoom with one receiver would not. Refusing the zoom
    // is better than taking it and dropping EP6 packets, because dropped packets
    // are a gap in every panadapter at once and nothing says why.
    const int allowed = maxReceiversAtRate(rate, static_cast<int>(m_rx.size()));
    if (allowed < static_cast<int>(m_rx.size())) {
        qCWarning(lcHl2).nospace()
            << "HL2: refusing " << rate / 1000 << " kHz span — "
            << m_rx.size() << " receivers would need "
            << QString::asprintf("%.1f", ep6BitsPerSecond(rate, static_cast<int>(m_rx.size())) / 1e6)
            << " Mbit/s on a 100BASE-T link (max " << allowed
            << " receivers at this span). Close a receiver to zoom out further.";
        emitAllPanState();   // snap the widget back to the span that is real
        return;
    }

    m_sampleRateHz = rate;

    // THE RATE REGISTER IS NOT WRITTEN HERE. It used to be — posted to
    // MetisClient at exactly this point, before a single chain had been
    // rebuilt — and that is the ordering this change exists to undo: the radio
    // switched rate and then every receiver spent the length of the rebuild
    // decimating the new IQ for the old rate. The write now happens on the
    // SUCCESS PATH only, in the same I/O-thread turn that installs the new
    // chains, and a failed rebuild therefore never reaches the radio at all.
    // See the install step below and finishRateChange().

    // Three threads. GUI (here): snapshot every Config, since m_rx is GUI-only.
    // Build (m_dspBuildThread): construct all N new chains while the old set
    // keeps running; N because the DDC rate register is radio-wide. I/O
    // (m_metis): if all N built, swap pointers and write the rate register in
    // one turn. The build stays off the I/O thread because EP2 pacing, EP6
    // ingest and processIqBlock run there, and the gateware watchdog halts the
    // stream if EP2 stops (docs/HERMES.md §20.8). No roll-back is needed: on
    // any failure the register and live chains are untouched. Glitch-free
    // audio across a crossing is unmeasured.
    struct RebuildStep {
        // QPointer: a receiver can close mid-build and tearDownReceivers()
        // deleteLater()s it on the I/O thread, so every null check happens on
        // the I/O thread. The build thread only copies these (atomic refcount)
        // and never checks them; buildChannel() is static and takes a Config.
        QPointer<Hl2RxDsp> dsp;
        Hl2RxDsp::Config next;
        std::size_t index = 0;
    };
    std::vector<RebuildStep> steps;
    steps.reserve(m_rx.size());
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        Receiver& r = m_rx[i];
        if (!r.dsp)
            continue;
        RebuildStep st;
        st.dsp = r.dsp;
        st.index = i;
        st.next.inputSampleRateHz = m_sampleRateHz;
        st.next.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
        st.next.mode = modeFromString(r.mode);
        std::tie(st.next.filterLowHz, st.next.filterHighHz) = dspFilterHz(r);
        // Carried through the rebuild rather than reapplied afterwards. A
        // reconfigured channel opens on Config's defaults, so an operator who
        // had moved their AGC would have had it silently snap back to
        // medium/39 dB every time they zoomed.
        st.next.agcMode = wdspAgcMode(r.agcMode);
        st.next.maximumAgcGainDb = m_dbRef.agcCeilingDb(r.agcThresholdDb);
        steps.push_back(st);
    }

    // A SECOND CROSSING WHILE ONE IS IN FLIGHT. An operator dragging a zoom
    // produces these back to back. The generation is checked TWICE: once on
    // the I/O thread immediately before anything live is touched, so a
    // superseded set is destroyed instead of installed, and again on the GUI
    // thread so a superseded outcome publishes nothing.
    const quint64 generation = m_rateLedger.beginCrossing();
    const int targetRate = m_sampleRateHz;

    if (steps.empty()) {
        // No chains to rebuild, so nothing can fail — command the rate and
        // publish, which is what the whole success path below reduces to here.
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, "setSampleRate", Qt::QueuedConnection,
                Q_ARG(AetherSDR::hl2::SampleRate, sampleRateEnum(rate)));
            // Commanded, so it is committed. Queued rather than direct, but
            // nothing can overtake it: the next crossing's register write is
            // posted to the same thread behind this one.
            m_rateLedger.commit(rate);
        }
        Hl2Settings::setSpanMhz(static_cast<double>(m_sampleRateHz) / 1.0e6);
        announceReceiverCeilingRevision();
        emitAllPanState();
        notifyOperatingStateChanged();
        return;
    }

    // m_metis is the handle onto the I/O thread's event loop — the object that
    // actually lives there (publishIoDspList() says the same). It is created in
    // the constructor and deleted in the destructor, after both threads are
    // joined, so it is stable for this object's whole life and safe to capture.
    MetisClient* const metis = m_metis;
    if (!metis) {
        // No wire object means no I/O thread loop to post to and no radio to
        // command. Nothing was changed; report the failure the way a failed
        // build would.
        finishRateChange(false, generation, targetRate, previousRate, {}, 0,
                         "HL2: no wire client to command the new rate");
        return;
    }

    QMetaObject::invokeMethod(metis, [this, metis, steps, generation, targetRate,
                                      previousRate] {
        // I/O thread, turn 1. beginRebuild() first, so control calls arriving
        // during the build update the chain's mirrors instead of blocking on
        // WDSP's setup mutex; installRebuiltChannel() re-applies them at the
        // swap. The NB pair is snapshotted because the channel opens with it
        // and it is not part of Config.
        struct BuildInput {
            Hl2RxDsp::Config cfg;
            bool nbOn = false;
            int nbLevel = 50;
        };
        std::vector<BuildInput> inputs;
        inputs.reserve(steps.size());
        for (const RebuildStep& st : steps) {
            BuildInput in;
            in.cfg = st.next;
            if (st.dsp) {
                st.dsp->beginRebuild(st.next);
                in.nbOn = st.dsp->noiseBlankerEnabled();
                in.nbLevel = st.dsp->noiseBlankerLevel();
            }
            inputs.push_back(in);
        }

        QMetaObject::invokeMethod(m_dspBuildContext, [this, metis, steps, inputs,
                                                      generation, targetRate,
                                                      previousRate] {
            // ---- BUILD THREAD: the whole cost, and nothing live in reach ----
            //
            // SERIAL, and not as a concession: WDSP's OpenChannel and
            // Hl2Spectrum's FFT plan both run under the SAME process-wide lock
            // (WdspChannel.cpp's g_setupMutex, taken by open() and by
            // WdspChannel::fftwSetupLock() which Hl2Spectrum's constructor
            // uses). N builds on N threads would serialise on that lock anyway
            // and the wall clock would be identical. What matters is WHICH
            // thread pays it, not how many.
            std::vector<Hl2RxDsp::RebuildResult> built;
            built.reserve(steps.size());
            bool ok = true;
            std::size_t failedAt = 0;
            std::string err;
            for (std::size_t n = 0; n < inputs.size(); ++n) {
                Hl2RxDsp::RebuildResult r = Hl2RxDsp::buildChannel(
                    inputs[n].cfg, inputs[n].nbOn, inputs[n].nbLevel);
                if (!r.channel) {
                    ok = false;
                    failedAt = n;
                    err = r.error;
                    break;   // the set is all-or-nothing; building past the
                             // first failure is waste
                }
                built.push_back(std::move(r));
            }
            const std::size_t failedIndex = ok ? 0 : steps[failedAt].index;

            QMetaObject::invokeMethod(metis, [this, metis, steps, generation,
                                              targetRate, previousRate, ok,
                                              failedIndex, err,
                                              b = std::move(built)]() mutable {
                // ---- I/O THREAD, turn 2: the only step that touches live state ----
                //
                // SUPERSEDED, FAILED, and SUCCEEDED all end the same way for
                // the radio: unless every chain is ready AND this is still the
                // rate being asked for, the register is not written and no
                // published chain is touched. `b` is destroyed on the way out,
                // which closes the new channels on this thread — the thread
                // that owns them.
                const bool current = (m_rateLedger.isCurrent(generation));
                if (!ok || !current) {
                    for (const RebuildStep& st : steps) {
                        if (st.dsp)
                            st.dsp->abandonRebuild();
                    }
                    b.clear();
                } else {
                    // ALL N SUCCEEDED. Install every chain, then command the
                    // radio, in ONE turn of this thread's loop — which is also
                    // the thread EP6 is delivered on, so no block is processed
                    // between the two and no chain is ever fed IQ at a rate it
                    // was not built for by this sequence. (The radio's own
                    // latency to latch the register is a separate, much shorter
                    // window, and is NOT covered here — see finishRateChange().)
                    for (std::size_t n = 0; n < steps.size(); ++n) {
                        if (steps[n].dsp)
                            steps[n].dsp->installRebuiltChannel(std::move(b[n]));
                        else
                            b[n] = Hl2RxDsp::RebuildResult {};   // receiver closed mid-build
                    }
                    // Straight through, not queued: this lambda is ALREADY on
                    // m_metis's thread, so the call is the same turn as the
                    // installs above. The DDC rate lives in the config register
                    // (C0=0x00), latched into the next C&C round. Deliberately
                    // NOT followed by a filter-pipeline reset: sending 0x39 on
                    // every geometry change is what wedged a board hard enough
                    // to need a power cycle (see
                    // MetisClient::requestPipelineReset). The decimation
                    // filters settle on their own within a few blocks.
                    metis->setSampleRate(sampleRateEnum(targetRate));
                    // COMMITTED — written here and nowhere else on this path,
                    // in the same turn as the register write, so the value can
                    // never describe a rate the wire did not get.
                    m_rateLedger.commit(targetRate);
                }

                // WHICH CHAINS THIS CROSSING ACTUALLY COVERED. Identified by
                // pointer, not by index: closeReceiver() erases from the middle
                // of m_rx, so an index captured at snapshot time can be naming a
                // different receiver by the time this returns. Carried to the
                // GUI thread so finishRateChange() can tell a chain this
                // crossing rebuilt from one that was opened while it ran.
                std::vector<QPointer<Hl2RxDsp>> covered;
                covered.reserve(steps.size());
                for (const RebuildStep& st : steps)
                    covered.push_back(st.dsp);

                QMetaObject::invokeMethod(this, [this, ok, generation, targetRate,
                                                 previousRate, failedIndex, err,
                                                 covered = std::move(covered)] {
                    finishRateChange(ok, generation, targetRate, previousRate,
                                     covered, failedIndex, err);
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

// GUI-thread half of a rate change. Unlike AnanBackend there is no mute
// between the register write and the radio latching the new rate (one C&C
// round of wrong-rate IQ), because muting N chains would share
// setAudioMuted() with the TX mute, which a zoom must not lift. The window's
// length is unmeasured.
void Hl2Backend::finishRateChange(bool ok, quint64 generation, int targetRate,
                                  int previousRate,
                                  const std::vector<QPointer<Hl2RxDsp>>& covered,
                                  std::size_t failedIndex,
                                  const std::string& error)
{
    // SUPERSEDED. A newer crossing has already snapshotted its own steps from
    // this receiver set and is running or has run; publishing this one's
    // outcome would report a rate that is no longer being asked for. The I/O
    // thread has already made the same check against the same counter, and
    // installed nothing — this one only stops the REPORT.
    if (!m_rateLedger.isCurrent(generation))
        return;

    if (!ok) {
        // NOTHING TO UNDO ON THE RADIO. The register is written only on the
        // success path, in the same I/O-thread turn that installs the chains,
        // so a failed build never reached the wire and every receiver is still
        // running at previousRate with the chain it always had. All that is
        // wrong is this object's optimistic m_sampleRateHz.
        qWarning() << "Hl2Backend: could not build RX DSP" << failedIndex
                   << "for" << targetRate << "Hz —"
                   << QString::fromStdString(error)
                   << "— staying at" << previousRate << "Hz";
        m_sampleRateHz = previousRate;
        announceReceiverCeilingRevision();
        emitAllPanState();
        return;
    }

    Hl2Settings::setSpanMhz(static_cast<double>(m_sampleRateHz) / 1.0e6);

    // A receiver opened during the build is not in `covered` and was built for
    // the old rate; rebuild it here, synchronously on the GUI thread. Cheap:
    // only receivers opened within one rebuild window.
    for (Receiver& r : m_rx) {
        if (!r.dsp || r.configuredRateHz == targetRate)
            continue;
        // A CHAIN WHOSE OWN BUILD IS STILL IN FLIGHT IS NOT OURS TO RECONCILE.
        //
        // createPanadapter() no longer opens a chain inline — it posts the build
        // to m_dspBuildThread and returns (startReceiverDspBuild()). A receiver
        // opened inside this crossing's window can therefore still be BUILDING
        // when this runs, and the configure() below would then run a second
        // OpenChannel against the process-wide WDSP setup mutex the first one is
        // holding: on the GUI thread, for the length of the build, which is the
        // stall this whole family of changes exists to remove. And it would be
        // pointless as well as slow, because the in-flight build installs over
        // the result the moment it lands.
        //
        // Left to finishReceiverDspBuild(), which re-reads the ledger when its
        // build completes and starts another one if the rate moved under it. The
        // flag is cleared there before that check, so nothing is dropped.
        if (r.dspBuildInFlight)
            continue;
        const bool wasCovered =
            std::any_of(covered.begin(), covered.end(),
                        [&r](const QPointer<Hl2RxDsp>& p) { return p == r.dsp; });
        if (wasCovered) {
            // Rebuilt by this crossing and installed on the I/O thread; only
            // this object's record of it still says the old rate.
            r.configuredRateHz = targetRate;
            continue;
        }

        Hl2RxDsp::Config dc;
        dc.inputSampleRateHz = targetRate;
        dc.audioSampleRateHz = 24000;
        dc.mode = modeFromString(r.mode);
        std::tie(dc.filterLowHz, dc.filterHighHz) = dspFilterHz(r);
        dc.agcMode = wdspAgcMode(r.agcMode);
        dc.maximumAgcGainDb = m_dbRef.agcCeilingDb(r.agcThresholdDb);

        std::string err;
        bool built = false;
        Hl2RxDsp* dsp = r.dsp;
        QMetaObject::invokeMethod(dsp, [dsp, &dc, &err, &built] {
            built = dsp->configure(dc, &err);
        }, Qt::BlockingQueuedConnection);
        if (built) {
            r.configuredRateHz = targetRate;
            qCInfo(lcHl2) << "HL2: rebuilt receiver opened during the"
                          << targetRate << "Hz crossing";
        } else {
            // The radio has already moved; this one chain could not follow. Say
            // so rather than leaving it silently decimating for a rate that is
            // no longer on the wire.
            qCWarning(lcHl2) << "HL2: receiver opened during the" << targetRate
                             << "Hz crossing could not be rebuilt —"
                             << QString::fromStdString(err)
                             << "— its audio and spectrum will be wrong until it"
                                " is reconfigured";
        }
    }

    // #5594 (M1): the rate is committed, so the receiver ceiling this radio can
    // honestly offer may have moved with it — maxSlices and maxPanadapters both
    // report it. Announced here rather than at the top of the function because
    // an announcement before the reconfigure could be rolled back below.
    // Guarded: the majority of zooms stay inside one ceiling and say nothing.
    announceReceiverCeilingRevision();

    // A narrower window may no longer contain the slice: the usable passband
    // shrank, and a slice left outside it would sit in the roll-off (or off the
    // display entirely) with nothing to say why it went quiet. Re-running the
    // tune re-centres the NCO only if it has to, and re-emits both states.
    // Every receiver, because the window shrank for all of them at once.
    for (const auto& ids : m_ids.all()) {
        if (const Receiver* r = rx(ids.ddcIndex))
            setSliceFrequency(ids.uiNumber, r->sliceFreqHz);
    }
    notifyOperatingStateChanged();
}

void Hl2Backend::setPanFrameRate(const QString& panId, int fps)
{
    // Straight through to the DSP, which skips the FFT itself when a frame is
    // not due. Queued: the cap is read on the DSP thread.
    //
    // PER PAN, unlike the span: the frame rate is a display cost, not a hardware
    // register, so a background receiver can be paced slowly while the one the
    // operator is watching runs fast. That is worth having at four receivers —
    // four full-rate FFTs is four times the render cost of one.
    const int ddc = ddcForPan(panId);
    Receiver* r = rx(ddc);
    if (!r || !r->dsp)
        return;
    QMetaObject::invokeMethod(r->dsp, "setSpectrumRateFps", Qt::QueuedConnection,
        Q_ARG(int, fps));
}

void Hl2Backend::setPanAverage(const QString& panId, int average)
{
    // Per pan, like the frame rate: each receiver has its own spectrum.
    // Stored first, so a chain that does not exist yet (or is rebuilt on
    // reconnect) picks it up in pushPanAveraging().
    Receiver* r = rx(ddcForPan(panId));
    if (!r)
        return;
    r->panAverage = std::clamp(average, 0, 100);
    pushPanAveraging(*r);
}

void Hl2Backend::setPanWeightedAverage(const QString& panId, bool on)
{
    Receiver* r = rx(ddcForPan(panId));
    if (!r)
        return;
    r->panWeightedAverage = on;
    pushPanAveraging(*r);
}

void Hl2Backend::pushPanAveraging(const Receiver& r)
{
    if (!r.dsp)
        return;
    // Queued: the spectrum's state is read on the DSP thread (see
    // Hl2Spectrum::setAverageFrames on why no other thread may touch it).
    // Both pushed every time; each is a no-op in Hl2Spectrum when unchanged,
    // so a replay does not throw the running average away.
    QMetaObject::invokeMethod(r.dsp, "setSpectrumAverageMs", Qt::QueuedConnection,
        Q_ARG(int, averageTimeMsForStep(r.panAverage)));
    QMetaObject::invokeMethod(r.dsp, "setSpectrumLogAverage", Qt::QueuedConnection,
        Q_ARG(bool, r.panWeightedAverage));
}

void Hl2Backend::dropPanAverage(const Receiver& r)
{
    if (!r.dsp || r.panAverage <= 0)
        return;
    QMetaObject::invokeMethod(r.dsp, "dropSpectrumAverage", Qt::QueuedConnection);
}

void Hl2Backend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    applyKeying(key, operation, completion, false);
}

void Hl2Backend::applyKeying(bool key, const TxCoordinator::Operation& operation,
                            const TxCoordinator::Completion& completion, bool cwBreakIn)
{
    if (!TxCoordinator::Command{operation, key}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    m_lastTxOperation = operation;
    // Keying is gated twice on purpose. capabilities().canTransmit reflects the
    // gate, so the engine guard above the seam already refuses when transmit is
    // off; MetisClient refuses independently at the wire. Neither is trusted to
    // be the only one -- this backend keyed nothing at all until very recently,
    // and the cost of a wrong key is an unintended emission.
    if (!m_txAllowed) {
        if (key)
            qWarning() << "Hl2Backend: key refused — automation bridge is active "
                          "without AETHER_AUTOMATION_ALLOW_TX";
        return;
    }
    // A manual PTT/MOX asserted while Break-In owns the current key transfers
    // that ownership to the operator. The backend is already keyed, so without
    // this explicit handoff the pending CW hang timer would later unkey a PTT
    // that is still being held.
    //
    // The automatic CW path sets m_cwAutoKeyed only AFTER its own applyKeying(true)
    // call below, so that internal key-up cannot be mistaken for manual intent.
    if (key && m_keyed && m_cwAutoKeyed) {
        if (m_cwHangTimer) {
            m_cwHangTimer->stop();
        }
        m_cwAutoKeyed = false;
        m_cwHangCompletion = {};
    }
    // Quiet-microphone advice at unkey. With a reduction-only ALC (#5646) the
    // pre-ALC mic peak is the on-air level. Gated off for EngineGenerated
    // audio (the WSPR beacon has no slider in its path); AX.25 is tagged
    // Microphone and gets the advice, since the slider is its only level
    // control. Once per transmission on the main thread, never per block.
    if (m_keyed && !key) {
        // Margin below alcTargetPeak that counts as quiet. Measured
        // (d81b-speech-pauses-alc, hpsdrsim loopback, not on air): speech
        // crest 18.87 dB (sd 0.80), burst spread <= 0.91 dB, and unity slider
        // peaks 19.56-19.67 dB below target (-21.08/-20.97 dBFS vs
        // 20*log10(0.85) = -1.4116 dBFS). So the margin must stay below
        // 19.56 dB or unity operators are never told. 12.0 is chosen: fires on
        // unity with 7.6 dB to spare, well clear of the measured noise. Only
        // bounded from one side; a leg at slider ~74 would bracket it.
        constexpr double kQuietMarginBelowTargetDb = 12.0;
        const double targetDbfs =
            20.0 * std::log10(std::max(1e-9, m_alcTargetPeak));
        const double quietBelowDbfs = targetDbfs - kQuietMarginBelowTargetDb;
        // Not for client-leveled transmissions. The reason is no longer that the
        // ALC is bypassed there — the mic and client paths are identical now
        // that the ceiling is unity on both. It is that the ADVICE is wrong: a
        // TCI/DAX client's transmit level is set in the client (WSJT-X's Pwr
        // slider and the rest), and "raise mic gain" would send an operator to a
        // control that is not the one holding their level down.
        if (!m_txAudioClientLeveled && !m_txAudioEngineGenerated
            && m_txMicPeakMaxDbfs > -139.0f
            && m_txMicPeakMaxDbfs < static_cast<float>(quietBelowDbfs)) {
            // Only advise raising mic gain if the remaining slider travel
            // covers the shortfall; derived from the mapping so it stays true
            // if either changes.
            const double shortfallDb = targetDbfs - m_txMicPeakMaxDbfs;
            const double travelLeftDb =
                micSliderToGainDb(100) - micSliderToGainDb(m_micLevel);
            if (travelLeftDb >= shortfallDb) {
                qCInfo(lcHl2) << "HL2 TX: microphone peaked at" << m_txMicPeakMaxDbfs
                              << "dBFS for the whole transmission, about"
                              << shortfallDb
                              << "dB under the ALC target of" << targetDbfs
                              << "dBFS — the ALC only reduces, so that audio went"
                                 " out quiet. Raise mic gain (currently"
                              << m_micLevel << "of 100).";
            } else {
                qCInfo(lcHl2) << "HL2 TX: microphone peaked at" << m_txMicPeakMaxDbfs
                              << "dBFS for the whole transmission, about"
                              << shortfallDb
                              << "dB under the ALC target of" << targetDbfs
                              << "dBFS, and mic gain is at" << m_micLevel
                              << "of 100 with only" << travelLeftDb
                              << "dB of travel left — the slider cannot close"
                                 " this. Raise the microphone's own level"
                                 " (AetherVoice input gain, or the mic's own"
                                 " control) instead.";
            }
        }
    }
    if (key) {
        m_txMicPeakMaxDbfs = -140.0f;
        // A new transmission decides afresh whether it is client-leveled; the
        // first submitTxAudio() block of the over re-marks it.
        m_txAudioClientLeveled = false;
        m_txAudioEngineGenerated = false;
        // Start each transmission's peak hold from nothing, rather than trusting
        // the unkeyed branch in publishTelemetry() to have already walked it
        // down. Telemetry is 10 Hz, so a key inside 100 ms of the previous unkey
        // can arrive before any no-carrier sample does, and the new over would
        // open displaying the old one's peak.
        m_fwdPeakWatts = 0.0;
    }

    const bool keyChanged = m_keyed != key;
    // The receive path keeps describing the operator's own transmission for a
    // measured 178-285 ms after this point (FINDINGS.md FIND-16, run
    // d83-unkey-transient), so anything the converter reports inside that
    // window is about the transmitter rather than the antenna. The automatic
    // gain control reads this; nothing else does.
    if (keyChanged && !key) {
        m_sinceUnkey.start();
    }
    m_keyed = key;
    if (keyChanged) {
        // Manual PTT is already mirrored optimistically by RadioModel, but CW
        // break-in keys inside this backend. Publish that edge so the TX
        // indicator, TCI clients and receive-side TX gates see the real state.
        // TransmitDelta::mox is observed state, not client intent, so this does
        // not start microphone capture or feed a second key command back down.
        TransmitDelta delta;
        delta.mox = key;
        emit transmitChanged(delta);
    }
    // Mute receive audio while keyed: the HL2 hears its own transmission, and
    // with an open mic that closes an acoustic feedback loop. Muted at the
    // demodulator (clocked with silence) so the spectrum keeps running and
    // nothing drains on unkey. Every receiver, since all share the antenna,
    // unless the TX audio monitor is on (radiocert's sideband stage
    // demodulates our own transmission).
    const bool muteWhileKeyed = key && !m_txMonitor;
    // The edges are asymmetric (#5497). Key down mutes here, first. Key up
    // releases at the bottom, after the MOX-off is queued, then holds past the
    // T/R turnaround (releaseRxAudioMuteAfterHold()). Receivers and
    // MetisClient share m_ioThread, so queued calls deliver in posting order.
    // A timer, because there is no T/R-complete signal: Ep6Response::ptt is
    // cw_on | ext_ptt, which a host MOX never raises.
    if (muteWhileKeyed) {
        if (m_unkeyUnmuteTimer) {
            m_unkeyUnmuteTimer->stop();   // a re-key inside the hold cancels it
        }
        applyRxAudioMute(true);
    }
    // Drop whatever was already queued for the mix. On unkey these would be the
    // stalest blocks in the buffer and would play out ahead of live audio.
    for (auto& q : m_mixPending)
        q.clear();

    // While keyed the shared filter board must follow the TRANSMIT receiver
    // rather than the agree-or-bypass receive policy — see applyBandFilter().
    applyBandFilter(key ? "key" : "unkey");

    // A voice key must not inherit a TUNE carrier (the packet builder prefers
    // the tone over audio). Only a tone TUNE raised is cleared; an explicitly
    // requested tone is the operator's to key.
    if (key && !m_tuning && m_toneFromTune)
        setTxTestTone(0.0, 0.0, operation);
    if (!key) {
        if (m_cwHangTimer) {
            m_cwHangTimer->stop();
        }
        m_cwHangCompletion = {};
        m_cwAutoKeyed = false;
        // Every unkey ends tune, so drive is restored here rather than in
        // setTune(): the TX watchdog, key verb, MOX/PTT coordinator and
        // disconnect reset all call setKeying(false) directly.
        const bool wasTuning = m_tuning;
        m_tuning = false;
        // Cleared on EVERY unkey, not only a tuning one, and unconditionally
        // rather than behind `wasTuning`: this is the single point every unkey
        // path converges on (the comment above enumerates them), and a tune
        // request left standing is a bit the radio re-reads on the next key —
        // a tuner that starts on the operator's next voice transmission.
        applyAtuTuneRequest(false);
        if (wasTuning) {
            setTxPower(m_rfPowerPercent);
            // Re-decide the filter: the earlier call ran with m_tuning still
            // set and so forced the TX receiver's filter, and the
            // `oc == m_ocFilterByte` early-out would keep it until a retune.
            applyBandFilter("tune-end");
        }
    }
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, [metis = m_metis, key, operation, cwBreakIn] {
            if (cwBreakIn) {
                metis->setCwMox(key, operation);
            } else {
                metis->setMox(key, operation);
            }
        }, Qt::QueuedConnection);
    }
    // Release only now, after the MOX-off is queued, so even a zero hold
    // posts behind it; the hold covers the control-packet wait, network hop,
    // T/R relay and PA decay. Armed by the key-up edge (keyChanged), not the
    // unkeyed state: a redundant setKeying(false) must not restart the timer.
    // Redundant unkeys happen (requestTransmitStop() calls stopTune() then
    // setMox(false); pushInitialState() too) and nothing upstream filters them.
    if (!muteWhileKeyed) {
        if (key) {
            // KEY DOWN WITH THE TX AUDIO MONITOR ON. muteWhileKeyed is false
            // only because the monitor asked to HEAR the transmitter. There is
            // no turnaround at a key-down and so nothing to wait for: anything
            // still held is released NOW rather than deferred into a fresh
            // hold. Normally a no-op, because the monitor path already
            // unmuted; it is here so that it stays a no-op.
            if (m_unkeyUnmuteTimer) {
                m_unkeyUnmuteTimer->stop();
            }
            if (m_rxAudioMuted) {
                applyRxAudioMute(false);
            }
        } else if (cwBreakIn && m_cwHangTimer
                   && m_cwHangTimer->interval() < m_unkeyUnmuteHoldMs) {
            // CW full break-in with a hang shorter than the hold skips the hold
            // (#5850), or the hold would outlast the inter-element space and
            // kill QSK. Uses m_cwHangTimer's interval (the configured hang).
            // Longer hangs and semi-break-in get the ordinary hold. Cost: the
            // receiver hears PA decay per element; inter-element space
            // (1200/WPM ms) falls below the 70 ms hold at ~17 WPM.
            if (m_unkeyUnmuteTimer) {
                m_unkeyUnmuteTimer->stop();
            }
            if (m_rxAudioMuted) {
                applyRxAudioMute(false);
            }
        } else if (keyChanged) {
            // THE ONE TRUE KEY-UP EDGE, and the only site allowed to arm a
            // hold.
            releaseRxAudioMuteAfterHold();
        } else if (!m_unkeyUnmuteTimer || !m_unkeyUnmuteTimer->isActive()) {
            // A redundant unkey with no hold running — an unmuted chain, or one
            // whose hold has already expired. Kept going through the helper so
            // both settle exactly as they always did.
            releaseRxAudioMuteAfterHold();
        }
        // else: a redundant unkey INSIDE a running hold. Leave the timer
        // exactly as the real edge armed it; restarting it is the defect above.
    }
    if (!key) {
        // Drop buffered audio on unkey so the next transmission does not open
        // with the tail of the previous one. BOTH stages hold audio and both
        // have to be cleared: the modulator's input buffer and filter history,
        // AND the wire queue behind it.
        //
        // Resetting only the modulator was not enough, and the gap was visible
        // on hardware — a key with no audio at all still produced ~1000 counts
        // of forward power for a moment, which was the previous transmission's
        // last half second going out on the air.
        if (m_txDsp) {
            QMetaObject::invokeMethod(m_txDsp, [dsp = m_txDsp, operation] {
                if (operation.permitsCleanup()) {
                    dsp->reset();
                }
            }, Qt::QueuedConnection);
        }
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, [metis = m_metis, operation] {
                if (operation.permitsCleanup()) {
                    metis->flushTxIq();
                }
            }, Qt::QueuedConnection);
        }
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, [metis = m_metis, operation] {
                if (operation.permitsCleanup()) {
                    metis->clearCwKeying();
                }
            }, Qt::QueuedConnection);
        }
    }
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, [completion] { completion.finish(); }, Qt::QueuedConnection);
    }
}

void Hl2Backend::setCwKeying(bool down, bool breakIn, int breakInDelayMs, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    if (!TxCoordinator::Command{operation, down}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    if (!m_txAllowed) {
        if (down) {
            qWarning() << "Hl2Backend: CW key refused — automation bridge is active "
                          "without AETHER_AUTOMATION_ALLOW_TX";
        }
        return;
    }

    const Receiver* txReceiver = rx(m_txDdc);
    const QString mode = txReceiver ? txReceiver->mode.toUpper() : QString();
    if (mode != QLatin1String("CW") && mode != QLatin1String("CWU")
        && mode != QLatin1String("CWL")) {
        // A key binding pressed in SSB must not become an unmodulated carrier.
        // Release still clears a previously-held edge during a mode change.
        if (down) {
            qCWarning(lcHl2) << "HL2 CW key ignored outside CW mode:" << mode;
            return;
        }
    }

    if (m_cwHangTimer) {
        m_cwHangTimer->stop();
    }
    m_cwHangCompletion = {}; // a fresh element supersedes that local hang
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, [metis = m_metis, down, operation, completion] {
            metis->setCwKeyDown(down, operation);
        }, Qt::QueuedConnection);
    }

    if (down) {
        // Full break-in raises MOX on the first element and keeps it through
        // the configured inter-element hang. With break-in off, CW only rides
        // an MOX/PTT the operator already asserted — matching Flex behavior and
        // piHPSDR's software-keyer path.
        if (breakIn && !m_keyed) {
            applyKeying(true, operation, {}, true);
            m_cwAutoKeyed = true;
        }
        return;
    }

    if (m_cwAutoKeyed && m_cwHangTimer) {
        // Preserve the complete five-millisecond fall even when the sidebar is
        // set to zero delay; otherwise MOX could fall before the shaped tail is
        // emitted. The configured delay remains the dominant hang at normal
        // operating values.
        constexpr int kCwEnvelopeReleaseMs = 6;
        m_cwHangOperation = operation;
        m_cwHangCompletion = completion;
        m_cwHangTimer->start(std::max(kCwEnvelopeReleaseMs,
                                      std::clamp(breakInDelayMs, 0, 2000)));
    } else if (!m_keyed && m_metis) {
        // A break-in-off key press without manual PTT produced no RF. Do not
        // leave CW owning the IQ stream after its release, or a later voice MOX
        // would correctly key but transmit only silence.
        QMetaObject::invokeMethod(m_metis, [metis = m_metis, operation] {
            if (operation.permitsCleanup()) {
                metis->clearCwKeying();
            }
        }, Qt::QueuedConnection);
    }
}

void Hl2Backend::setTxFrequency(double hz)
{
    if (!m_metis || hz <= 0.0)
        return;
    // Scaled like every RX NCO, and for a reason worth stating: the gateware
    // derives BOTH oscillators from one freqcomp (radio.v assigns tx_phase0 and
    // rx_phase[] from the same value), so a calibration that corrected receive
    // and not transmit would put the operator's signal where they used to hear
    // themselves — off frequency by the full error, on the air.
    //
    // TX is single-stage: there is no software shift behind this the way there
    // is on receive, so the 1 Hz register granularity is the floor here. At
    // 10 ppm on 28 MHz that is a 280 Hz error corrected to under 1 Hz.
    //
    // XIT lands here and ONLY here: every caller passes the transmit receiver's
    // dial, so the offset reaches the TX register on every path that writes it
    // (tune, TX slice move, reconnect, calibration) and never the receive side.
    double txHz = hz + (m_xitOn ? m_xitHz : 0);
    // XIT can take the command through zero behind the dial guard, and
    // ncoCommandHz() maps that to DC. Skipping the write is no better: the
    // register keeps its last value, possibly another band. Hold the dial.
    if (txHz <= 0.0) {
        qCWarning(lcHl2) << "HL2: XIT" << m_xitHz << "Hz would put TX at" << txHz
                         << "Hz from a dial of" << hz
                         << "Hz; TX register holds the dial, XIT not applied";
        txHz = hz;
    }
    QMetaObject::invokeMethod(m_metis, "setTxFrequencyHz", Qt::QueuedConnection,
        Q_ARG(std::uint32_t, ncoCommandHz(txHz)));
}

std::uint32_t Hl2Backend::ncoCommandHz(double trueHz) const noexcept
{
    return Hl2FreqCal::ncoCommandHz(trueHz, m_freqCalScale);
}

double Hl2Backend::dspShiftHz(double sliceTrueHz, double ncoTrueHz) const noexcept
{
    return Hl2FreqCal::dspShiftHz(sliceTrueHz, ncoCommandHz(ncoTrueHz),
                                  m_freqCalScale);
}

double Hl2Backend::cwBfoHz(const QString& mode) const noexcept
{
    return cwBfoOffsetHz(mode, m_cwPitchHz);
}

std::pair<double, double> Hl2Backend::dspFilterHz(const Receiver& r) const noexcept
{
    const double bfo = cwBfoHz(r.mode);
    return {static_cast<double>(r.filterLowHz) + bfo,
            static_cast<double>(r.filterHighHz) + bfo};
}

double Hl2Backend::rxShiftHz(const Receiver& r) const noexcept
{
    // Minus the BFO: the shift names the RF frequency the detector treats as
    // zero (dspShiftHz's contract), so lowering it by the pitch lifts the
    // marker onto the pitch. Not scaled by the frequency calibration: this is
    // an audio offset, not an RF frequency.
    return dspShiftHz(rxTunedHz(r), r.ncoHz) - cwBfoHz(r.mode);
}

double Hl2Backend::rxTunedHz(const Receiver& r) const noexcept
{
    // RIT is radio-wide at the seam (no slice id), and it only means anything
    // relative to the transmit frequency — so it belongs to the receiver that
    // owns transmit. Added before dspShiftHz() so it stays in the TRUE-RF
    // domain the frequency calibration already corrects.
    const bool ownsTx = (&r == rx(m_txDdc));
    return r.sliceFreqHz + ((m_ritOn && ownsTx) ? m_ritHz : 0);
}

void Hl2Backend::retuneReceiver(int ddc)
{
    // The same re-run setPanSampleRate() uses: re-centres the NCO only if the
    // receive frequency left the window, re-pushes the shift, re-emits state.
    const Hl2ReceiverIds* ids = m_ids.byDdc(ddc);
    const Receiver* r = rx(ddc);
    if (ids && r) {
        setSliceFrequency(ids->uiNumber, r->sliceFreqHz);
    }
}

void Hl2Backend::logRitScope() const
{
    // Radio-wide at the seam (RadioModel passes no slice id), so whichever VFO
    // the operator turned, this is the receiver whose audio actually moved.
    const Hl2ReceiverIds* ids = m_ids.byDdc(m_txDdc);
    qCInfo(lcHl2) << "HL2: RIT" << (m_ritOn ? "on," : "off,") << "offset" << m_ritHz
                  << "Hz — follows transmit: receiver DDC" << m_txDdc
                  << "(slice" << (ids ? ids->uiNumber : -1) << ")";
}

void Hl2Backend::setRitEnabled(bool on)
{
    if (on == m_ritOn) {
        return;
    }
    m_ritOn = on;
    if (m_ritHz != 0) {
        logRitScope();
        retuneReceiver(m_txDdc);
    }
}

void Hl2Backend::setRitOffset(int hz)
{
    const int requested = hz;
    hz = std::clamp(hz, -kRitXitMaxHz, kRitXitMaxHz);
    if (hz != requested) {
        qCWarning(lcHl2) << "HL2: RIT offset" << requested << "Hz clamped to" << hz
                         << "Hz (limit +/-" << kRitXitMaxHz << "Hz)";
    }
    if (hz == m_ritHz) {
        return;
    }
    m_ritHz = hz;
    if (m_ritOn) {
        logRitScope();
        retuneReceiver(m_txDdc);
    }
}

void Hl2Backend::setXitEnabled(bool on)
{
    if (on == m_xitOn) {
        return;
    }
    m_xitOn = on;
    const Receiver* txRx = rx(m_txDdc);
    if (m_xitHz != 0 && txRx) {
        setTxFrequency(txRx->sliceFreqHz);
    }
}

void Hl2Backend::setXitOffset(int hz)
{
    const int requested = hz;
    hz = std::clamp(hz, -kRitXitMaxHz, kRitXitMaxHz);
    if (hz != requested) {
        qCWarning(lcHl2) << "HL2: XIT offset" << requested << "Hz clamped to" << hz
                         << "Hz (limit +/-" << kRitXitMaxHz << "Hz)";
    }
    if (hz == m_xitHz) {
        return;
    }
    m_xitHz = hz;
    const Receiver* txRx = rx(m_txDdc);
    if (m_xitOn && txRx) {
        setTxFrequency(txRx->sliceFreqHz);
    }
}

void Hl2Backend::setCwPitch(int hz)
{
    // TransmitModel's own range. Clamped again rather than trusted: this is a
    // seam, and a pitch of 0 would silently turn CW back into the
    // marker-on-DC geometry this whole path exists to remove.
    hz = std::clamp(hz, 100, 6000);
    if (hz == m_cwPitchHz)
        return;
    m_cwPitchHz = hz;

    // Re-push every CW receiver. The pitch moves the BFO, and the BFO is baked
    // into BOTH the shift and the demodulator's passband, so a pitch change
    // that only re-sent one of them would leave the filter and the detector
    // disagreeing — the operator would hear the tone move and the signal fade.
    //
    // The operator's own cuts are untouched: they are carrier-relative, so a
    // 500 Hz filter stays a 500 Hz filter centred on the marker whatever the
    // pitch is. That is the point of keeping the two domains apart.
    for (Receiver& r : m_rx) {
        if (!r.dsp || cwBfoHz(r.mode) == 0.0)
            continue;
        const auto [lo, hi] = dspFilterHz(r);
        QMetaObject::invokeMethod(r.dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(r)));
        QMetaObject::invokeMethod(r.dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, lo), Q_ARG(double, hi));
    }
}

void Hl2Backend::repushAllFrequencies()
{
    if (!m_metis)
        return;
    for (const auto& s : m_ids.all()) {
        const Receiver* r = rx(s.ddcIndex);
        if (!r)
            continue;
        QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
            Q_ARG(int, s.ddcIndex),
            Q_ARG(std::uint32_t, ncoCommandHz(r->ncoHz)));
        if (r->dsp)
            QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
                Q_ARG(double, rxShiftHz(*r)));
    }
    // The transmit oscillator does not follow a receiver on its own — it is a
    // separate register that only setTxFrequency() writes. Omitting it here
    // would leave transmit on the OLD calibration until the next tune, which is
    // exactly the window an operator calibrating before a contest would key in.
    if (const Receiver* txRx = rx(m_txDdc); txRx && txRx->sliceFreqHz > 0.0)
        setTxFrequency(txRx->sliceFreqHz);
}

void Hl2Backend::forwardSpeakerAudioToCodec(const std::vector<float>& mixed)
{
    // THE ONE PLACE the speaker feed leaves for the radio's own codec, and it
    // is deliberately a tap on the EXISTING mix rather than a second mixer:
    // what the internal loudspeaker plays has to be what the host speaker
    // plays, including every fader, balance and mute the operator set. A
    // parallel path would drift the moment either side gained a control.
    if (!m_metis || !m_hw.hasLocalCodec() || mixed.empty())
        return;

    // 24 kHz -> 48 kHz: Hl2RxDsp outputs the engine's RX rate and EP2 runs at a
    // fixed 48 kHz, so this is an exact 2x linear interpolation (repetition
    // would mirror the baseband about 12 kHz). m_codecLastL/R carry the last
    // frame so the midpoint across a block boundary is correct.
    const std::size_t frames = mixed.size() / 2;
    if (frames == 0)
        return;
    std::vector<std::int16_t> out;
    out.reserve(frames * 4);              // 2 output frames x 2 channels

    // THE RADIO'S OWN LEVEL, which is not the application's. AudioEngine
    // applies the operator's volume as a device attenuation on the host's
    // QAudioSink, so it is not in these samples and no tap could find it —
    // see Hl2HardwareOptions::speakerLevelPercent for why that makes a
    // separate fader necessary rather than merely convenient.
    const float speakerGain = m_hw.speakerGain();
    if (speakerGain <= 0.0f) {
        // Silent, but the interpolator's carry still has to advance or the
        // first sample after the operator brings the level back up would be
        // interpolated out of a frame from before it went down — a click at
        // exactly the moment they are listening for one.
        m_codecLastL = mixed[2 * (frames - 1)];
        m_codecLastR = mixed[2 * (frames - 1) + 1];
        m_codecHavePrev = true;
        return;
    }

    const auto toI16 = [speakerGain](float v) -> std::int16_t {
        v *= speakerGain;
        // Symmetric clamp, 32767 not 32768: letting a full-scale sample wrap to
        // the negative rail is a click, and this is a speaker feed.
        v = std::clamp(v, -1.0f, 1.0f);
        return static_cast<std::int16_t>(v * 32767.0f);
    };

    float prevL = m_codecHavePrev ? m_codecLastL : (mixed.size() > 0 ? mixed[0] : 0.0f);
    float prevR = m_codecHavePrev ? m_codecLastR : (mixed.size() > 1 ? mixed[1] : 0.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        const float l = mixed[2 * f];
        const float r = mixed[2 * f + 1];
        // Midpoint FIRST, then the sample itself: the interpolated frame sits
        // between the previous original and this one, so emitting it after
        // would put it half a sample into the future and delay the whole
        // stream by one output frame every block.
        out.push_back(toI16(0.5f * (prevL + l)));
        out.push_back(toI16(0.5f * (prevR + r)));
        out.push_back(toI16(l));
        out.push_back(toI16(r));
        prevL = l;
        prevR = r;
    }
    m_codecLastL = prevL;
    m_codecLastR = prevR;
    m_codecHavePrev = true;

    QMetaObject::invokeMethod(
        m_metis, "submitSpeakerAudio", Qt::QueuedConnection,
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(out.data()),
                                     static_cast<qsizetype>(out.size()
                                         * sizeof(std::int16_t)))));
}

void Hl2Backend::applyAtuTuneRequest(bool tuning)
{
    if (!m_metis)
        return;
    // Gated on the operator's gateware-ATU declaration (an IO-board ATU over
    // I2C must never see this bit) and on m_txAllowed. exttuner.v leaves IDLE
    // on the bit alone (`if (enable) state_next = DELAY;`, enable =
    // cmd_data[20]), with no key or PTT; DELAY/TRY/HANG then assert txinhibit
    // (hermeslite_core.v: tx_en(tx_on & ~atu_txinhibit)) and TRY drives
    // `start` low to begin an external tune. Clearing is never gated: `tuning`
    // false always clears the bit.
    const bool request = tuning && m_hw.atuGateware && m_txAllowed;
    QMetaObject::invokeMethod(m_metis, "setAtuTuneRequest", Qt::QueuedConnection,
                              Q_ARG(bool, request));
}

void Hl2Backend::applyHardwareOptions(const Hl2HardwareOptions& next, bool persist)
{
    const Hl2HardwareOptions before = m_hw;
    m_hw = next;
    if (persist) {
        // Never write an empty radio_id row: RadioSettingsScope::isValid() only
        // requires a family, so an empty serial would write the family-wide
        // default and apply one radio's options (e.g. the dither bit) to every
        // HL2. Reachable before connect via invokeExtension; mirrors the guard
        // in applyFreqCalPpb().
        if (m_radioSerial.isEmpty()) {
            qCWarning(lcHl2) << "HL2: not persisting hardware options —"
                             << "no radio identity yet; applying for this session only";
        } else {
            Hl2HardwareOptions::save(
                RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial), m_hw);
        }
    }
    if (!m_metis)
        return;

    // EACH FIELD PUSHED ONLY IF IT MOVED, and through the setter that owns it
    // rather than by rebuilding the session. Every one of these is live-
    // changeable on a running stream: the config register rides every EP2
    // frame, the drive bank is a one-shot, the VersaClock sequence is a queue
    // of one-shots, and the codec gate is a flag the packet builder reads.
    // Reconnecting to apply them would drop the operator's audio to change a
    // checkbox.
    if (m_hw.ditherBitOnWire() != before.ditherBitOnWire()
        || m_hw.randomBit != before.randomBit) {
        QMetaObject::invokeMethod(m_metis, "setDitherRandomBits", Qt::QueuedConnection,
                                  Q_ARG(bool, m_hw.ditherBitOnWire()),
                                  Q_ARG(bool, m_hw.randomBit));
    }
    if (m_hw.hasLocalCodec() != before.hasLocalCodec()) {
        // The resampler's carry describes a stream that is about to stop or
        // start; either way the next block must not be interpolated out of it.
        m_codecHavePrev = false;
        QMetaObject::invokeMethod(m_metis, "setLocalCodec", Qt::QueuedConnection,
                                  Q_ARG(bool, m_hw.hasLocalCodec()));
    }
    if (m_hw.hasLocalCodec() && m_hw.speakerLevelPercent == 0
        && before.speakerLevelPercent != 0) {
        // The fader stops new samples in forwardSpeakerAudioToCodec(), but
        // samples already queued would keep the speaker audible for up to 250 ms.
        QMetaObject::invokeMethod(m_metis, "clearSpeakerAudio", Qt::QueuedConnection);
    }
    if (m_hw.cl1RefClock != before.cl1RefClock) {
        // DSP setup is asynchronous; start() must consume the latest accepted
        // intent rather than overwrite the idle transport with its old snapshot.
        if (m_pendingConnect) {
            m_pendingConnect->mp.cl1RefClock = m_hw.cl1RefClock;
        }
        QMetaObject::invokeMethod(m_metis, "setCl1RefClock", Qt::QueuedConnection,
                                  Q_ARG(bool, m_hw.cl1RefClock));
        // AND ZERO THE MANUAL CORRECTION — through the one rule that owns it,
        // because the checkbox is not the only way the two documents can end up
        // disagreeing. See normalizeCl1Calibration().
        if (normalizeCl1Calibration("CL1 external reference engaged")) {
            if (m_pendingConnect) {
                m_pendingConnect->mp.rxFrequencyHz = ncoCommandHz(m_rx.front().ncoHz);
            }
            repushAllFrequencies();
        }
    }
    if (m_hw.atuGateware != before.atuGateware) {
        // Turning the option OFF mid-tune has to clear a request that is
        // already standing, which applyAtuTuneRequest() does because it
        // re-derives the bit from m_hw rather than from what was asked for.
        applyAtuTuneRequest(m_tuning);
    }
    if (m_hw.filterBoard != before.filterBoard || m_hw.n2adrHpf != before.n2adrHpf) {
        // Re-evaluated, not recomputed here: applyBandFilter() owns the
        // agree-or-bypass rule and the transmit override, and a second site
        // deciding the same byte is how the two drift apart.
        applyBandFilter("hardware options");
        publishWideState();
    }
}

std::optional<std::pair<bool, std::uint32_t>> Hl2Backend::pendingCl1ReferenceForTest() const
{
    if (!m_pendingConnect) {
        return std::nullopt;
    }
    return std::pair(m_pendingConnect->mp.cl1RefClock, m_pendingConnect->mp.rxFrequencyHz);
}

bool Hl2Backend::normalizeCl1Calibration(const char* why)
{
    // THE INVARIANT: an external 10 MHz reference at CL1 and a non-zero manual
    // correction cannot both stand. docs/architecture/hl2-frequency-calibration.md
    // §4: "When CL1 is locked, the manual ppb from this feature must be forced to
    // zero and the control disabled — otherwise we would correct an
    // already-correct clock."
    //
    // The two describe the same error in the same units and compose by
    // multiplication, so leaving a ppb standing under a disciplined reference
    // does not half-correct anything: it INTRODUCES exactly the error the
    // operator measured off the old crystal, permanently, against a clock that
    // no longer has it.
    //
    // ONE RULE, TWO CALLERS, and that is the point of the function. It ran only
    // inside applyHardwareOptions()' change-of-checkbox branch, which covers the
    // operator ticking the box and nothing else — not a connect that restores an
    // inconsistent pair, which is the case the review found.
    if (!m_hw.cl1RefClock || m_freqCalPpb == 0) {
        return false;
    }
    qCWarning(lcHl2) << "HL2:" << why << "— CL1 external reference is enabled with"
                     << "a manual frequency calibration of" << m_freqCalPpb
                     << "ppb standing; forcing it to 0"
                     << "(hl2-frequency-calibration.md §4)";
    m_freqCalPpb = 0;
    m_freqCalScale = Hl2FreqCal::scaleForPpb(0);
    // PERSISTED, not merely applied. The ppb lives in this radio's settings
    // document and a session-only zero would come back on the next connect,
    // which is the one place nobody would think to look for it. savePpb()
    // reports its own failure — it removes the row for a zero and warns if the
    // store refused — so a write that does not land is on the record rather than
    // silently repaired again next session.
    //
    // The empty-serial guard is applyFreqCalPpb()'s, restated because this does
    // not go through it: a row written with no identity becomes the family-wide
    // default that every HL2 without one of its own adopts (AGENTS.md).
    if (m_radioSerial.isEmpty()) {
        qCWarning(lcHl2) << "HL2: not persisting the forced zero —"
                         << "no radio identity yet; applying for this session only";
    } else {
        Hl2FreqCal::savePpb(RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial), 0);
    }
    return true;
}

void Hl2Backend::applyFreqCalPpb(int ppb, bool persist)
{
    const int clamped = Hl2FreqCal::clampPpb(ppb);
    if (persist) {
        // Never write an empty radio_id row (AGENTS.md): RadioSettingsScope
        // falls back exact-radio → family-wide on read, so a row written with no
        // identity is silently adopted by every HL2 that has none of its own —
        // exactly the contamination the per-MAC key exists to prevent.
        // m_radioSerial is only assigned in connectRadio(), so this is reachable
        // before the first connect and from a hand-built connect request.
        if (m_radioSerial.isEmpty()) {
            qCWarning(lcHl2) << "HL2: not persisting frequency calibration —"
                             << "no radio identity yet; applying for this session only";
        } else {
            Hl2FreqCal::savePpb(RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial),
                                clamped);
        }
    }
    if (clamped == m_freqCalPpb)
        return;                       // no-op: do not churn every NCO for nothing
    m_freqCalPpb = clamped;
    m_freqCalScale = Hl2FreqCal::scaleForPpb(clamped);
    qCInfo(lcHl2) << "HL2: frequency calibration" << clamped << "ppb"
                  << "— effective clock"
                  << Hl2FreqCal::effectiveClockHz(clamped) << "Hz";
    repushAllFrequencies();
}

void Hl2Backend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                               TxAudioSource source,
                               const TxCoordinator::Context& context)
{
    if (!context.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    // Only modulate while actually keyed. Feeding the modulator unkeyed would
    // fill the transmit queue with audio that goes out the instant MOX asserts —
    // the operator would hear the last second of the shack on their first
    // syllable.
    if (!m_txDsp || !m_keyed || int16Stereo.isEmpty())
        return;
    // Sticky per transmission until #5647 gives it a job in TxAudioSource
    // (see Hl2Backend.h). Assumes sources never interleave within one
    // transmission. EngineGenerated's only producer, startWsprPump(), is
    // fenced from the mic path (setDaxTxMode(true); onTxAudioReady() returns on
    // m_daxTxMode) and from TCI/DAX (feedDaxTxAudio() returns while the beacon
    // is active). Hl2TxDsp::processAudioBlock also drops m_inBuffer residue on
    // a mid-transmission source change, with a warning. Anyone changing the
    // mic-capture gate or adding an engine feed must re-check these fences.
    m_txAudioClientLeveled =
        m_txAudioClientLeveled || (source == TxAudioSource::ClientLeveled);
    // The unkey diagnostic must not tell a WSPR beacon to raise its mic gain.
    // Engine-generated audio has no mic slider in its path at all now, so
    // "raise mic gain" would point at a control that cannot move it.
    m_txAudioEngineGenerated =
        m_txAudioEngineGenerated || (source == TxAudioSource::EngineGenerated);
    if (sampleRateHz != 24000) {
        // Stated rather than silently resampled: the modulator's upsampler
        // assumes this rate, and a mismatch transmits at the wrong pitch.
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "Hl2Backend: TX audio arrived at" << sampleRateHz
                       << "Hz, expected 24000 — not transmitting";
        }
        return;
    }

    // Interleaved stereo to mono. AudioEngine duplicates the mic across both
    // channels, so averaging is right for that and still sane if they differ.
    const auto* pcm = reinterpret_cast<const qint16*>(int16Stereo.constData());
    const int frames = static_cast<int>(int16Stereo.size() / sizeof(qint16)) / 2;
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (int n = 0; n < frames; ++n) {
        const float l = static_cast<float>(pcm[2 * n]) / 32768.0f;
        const float r = static_cast<float>(pcm[2 * n + 1]) / 32768.0f;
        mono[static_cast<std::size_t>(n)] = 0.5f * (l + r);
    }
    QMetaObject::invokeMethod(m_txDsp,
                              [dsp = m_txDsp, mono = std::move(mono), source, context] {
        dsp->processAudioBlock(mono, source, context);
    }, Qt::QueuedConnection);
}

void Hl2Backend::setTxTestTone(double offsetHz, double amplitude, const TxCoordinator::Operation& operation)
{
    if (!TxCoordinator::Command{operation, amplitude > 0.0}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    if (!m_metis)
        return;
    // Whoever calls this owns the tone. setTune() re-asserts ownership straight
    // after, which is what lets keying distinguish "a carrier TUNE left behind"
    // from "a tone the operator asked for".
    m_toneFromTune = false;
    if (amplitude > 0.0 && !m_txAllowed) {
        qWarning() << "Hl2Backend: test tone refused — transmit not available";
        return;
    }
    QMetaObject::invokeMethod(m_metis, [metis = m_metis, offsetHz, amplitude, operation] {
        metis->setTxTestTone(offsetHz, amplitude, operation);
    }, Qt::QueuedConnection);
}

void Hl2Backend::setTxAudioMonitor(bool on)
{
    m_txMonitor = on;
    // Applies immediately while keyed. Every receiver, matching setKeying();
    // mixReceiverAudio() also honours m_txMonitor. applyRxAudioMute() stamps
    // SliceSamplingGate on resume. No #5497 hold here: that covers the PA's
    // T/R turnaround, and a monitor change while keyed must take effect now.
    if (m_keyed && !on) {
        if (m_unkeyUnmuteTimer) {
            m_unkeyUnmuteTimer->stop();
        }
        applyRxAudioMute(true);
    } else if (!on && !m_keyed && m_unkeyUnmuteTimer && m_unkeyUnmuteTimer->isActive()) {
        // Unkeyed with the post-unkey hold armed: monitor OFF leaves the
        // timer running and the receiver muted until it expires, so it cannot
        // unmute inside the T/R turnaround. Monitor ON takes the immediate
        // path below.
    } else {
        applyRxAudioMute(false);
        if (m_unkeyUnmuteTimer) {
            m_unkeyUnmuteTimer->stop();   // overtaken, or answered on purpose
        }
    }
}

void Hl2Backend::setTune(bool on, int tunePowerPercent, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    if (!TxCoordinator::Command{operation, on}.permitsDispatch(TxCoordinator::monotonicMs())) {
        return;
    }
    // Tune is the test tone at zero offset (a carrier on the TX NCO) with
    // keying held. Carrier up before keying; key down before the carrier on
    // release. Drive comes from TUNE power, since the carrier is full-scale.
    // The RF power restore lives in setKeying(false), where all unkey paths
    // converge, so m_tuning is set here only on the way up and setKeying()
    // clears it.
    if (on) {
        m_tuning = true;
        // BEFORE the carrier, not after. The HL2's AH-4 handler samples the
        // request alongside the key, and raising it after the tone is already
        // radiating leaves the first moments of the tune un-requested — which
        // on some ATUs is long enough to be a transmission the tuner ignores.
        // No-op unless the operator declared the gateware owns the tuner.
        applyAtuTuneRequest(true);
        // Straight to the drive register rather than through setTxPower(), which
        // would overwrite the saved RF power we have to restore on release.
        if (tunePowerPercent >= 0)
            applyDrive(tunePowerPercent);
        setTxTestTone(0.0, kTuneCarrierAmplitude, operation);
        m_toneFromTune = true;   // set AFTER: setTxTestTone clears the flag
        setKeying(true, operation, completion);
    } else {
        setKeying(false, operation, completion);   // clears m_tuning and restores the operator's RF power
        setTxTestTone(0.0, 0.0, operation);
    }
}

// Clamp, map to the drive register, and honour the transmit gate. Shared by
// setTxPower() and setTune() so the mapping — whose coarseness is documented in
// setTxPower() — exists once and cannot drift between the two.
void Hl2Backend::applyDrive(int percent)
{
    // Drive is gated exactly like keying. setTxDriveLevel writes the PA-enable
    // bit (0x09[19]) every frame, so an ungated call — e.g. the push-current-
    // power-on-connect path with a default rfPower of 100 — would bias the PA on
    // uncommanded in a transmit-BLOCKED session, defeating connectRadio()'s
    // deliberate drive=0 safety seed. Assert drive off instead. (#4449 review)
    if (!m_txAllowed) {
        setTxDriveLevel(0);
        return;
    }
    const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    setTxDriveLevel(clamped * kTxDriveMax / 100);
}

std::pair<int, int> Hl2Backend::effectiveTxPassband(const QString& mode) const
{
    // SSB voice only: the Phone applet's TX cut is a voice control, so CW,
    // AM/FM and digital modes keep their mode passbands (digital's matches
    // what far-end decoders expect).
    const QString u = mode.toUpper();
    const bool ssbVoice = u == QLatin1String("USB") || u == QLatin1String("LSB");
    if (m_txFilterFromOperator && ssbVoice)
        return {m_txFilterLowHz, m_txFilterHighHz};
    return defaultTxPassbandForMode(mode);
}

// Push the effective passband to the modulator and echo it upward, so the
// Phone applet shows what the modulator runs (including a restored
// passband). TransmitModel::applyChanges() does not emit
// txFilterCommandIssued, so the echo cannot loop (pinned by
// transmit_model_test).
void Hl2Backend::pushTxPassband(const QString& mode)
{
    const auto [lo, hi] = effectiveTxPassband(mode);
    if (m_txDsp) {
        QMetaObject::invokeMethod(m_txDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(lo)),
            Q_ARG(double, static_cast<double>(hi)));
    }
    TransmitDelta delta;
    delta.txFilterLow = lo;
    delta.txFilterHigh = hi;
    emit transmitChanged(delta);
}

// The Phone applet's TX low-cut / high-cut.
//
// eSSB works here and needs nothing special: the modulator's bandpass is
// designed per call (Hl2TxDsp::setFilter re-runs designFilters), the audio
// arriving from AudioEngine is 24 kHz so anything below ~11 kHz is
// representable, and the 255-tap Blackman prototype keeps a usable skirt across
// that range. The upper bound below is the modulator's, not the operator's
// taste: what belongs on the air is a band-plan question this layer has no
// business deciding.
void Hl2Backend::setTxFilter(int lowHz, int highHz)
{
    lowHz = std::clamp(lowHz, 0, kTxAudioMaxHz - 50);
    highHz = std::clamp(highHz, lowHz + 50, kTxAudioMaxHz);

    m_txFilterFromOperator = true;
    m_txFilterLowHz = lowHz;
    m_txFilterHighHz = highHz;

    qCInfo(lcHl2) << "HL2: TX passband set to" << lowHz << ".." << highHz << "Hz"
                  << "(operator override; mode defaults no longer apply)";

    // Through effectiveTxPassband(), so a change made in CW is remembered and
    // applied on return to SSB. Not via pushTxPassband(): an echo here would
    // snap the applet to the mode default and the next nudge would overwrite
    // the remembered pair.
    const Receiver* txRx = rx(m_txDdc);
    const QString txMode = txRx ? txRx->mode : QStringLiteral("USB");
    const auto [applyLo, applyHi] = effectiveTxPassband(txMode);
    if (m_txDsp)
        QMetaObject::invokeMethod(m_txDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(applyLo)),
            Q_ARG(double, static_cast<double>(applyHi)));

    // Client-authoritative state moved, so the capture half has to know — the
    // radio will never report this back, and without the hook the setting only
    // reaches disk if some OTHER setter happens to fire before the session ends.
    notifyOperatingStateChanged();
}

// The Phone applet's MIC slider (0..100) onto the modulator's linear pre-ALC
// gain. 50 is unity. Below 50: -20 dB at 0.4 dB/step; above: +40 dB at
// 0.8 dB/step (no ALC makeup gain; hl2_tx_level_policy_test pins the join).
// Applies to mic and TCI/DAX, not EngineGenerated audio (Hl2TxLevelPolicy.h).
// Level 0 mutes the mic and TCI/DAX path; it does not stop a WSPR beacon.
void Hl2Backend::setMicGain(int level)
{
    level = std::clamp(level, 0, 100);
    const bool moved = level != m_micLevel;
    m_micLevel = level;

    const double linear = micSliderToLinear(level);

    if (m_txDsp)
        QMetaObject::invokeMethod(m_txDsp, "setMicGain", Qt::QueuedConnection,
            Q_ARG(double, linear));

    // Capture half of TxSetpoints memory; RadioModel debounces the store.
    // Only on change: setupBackend() re-asserts the level on every rebuild,
    // and an unconditional notify would write it under the next radio.
    if (moved)
        notifyOperatingStateChanged();
}

void Hl2Backend::setTxPower(int percent)
{
    // The operator's 0..100 maps onto the HL2's 0..255 drive field. The gateware
    // only decodes the top nibble, so the effective resolution is coarser than
    // this suggests — the mapping is linear in the register, NOT calibrated to
    // watts, and nothing here should imply otherwise.
    //
    // Remember the operator's drive so setTune() can drop to tune power and the
    // unkey can put this back. Recorded BEFORE the transmit gate and even while
    // tuning: a power change made mid-tune, or while TX is blocked, is still
    // what the operator wants once the carrier drops or the gate opens.
    const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    // OPERATOR intent only — and only on CHANGE: internal band-memory applies
    // (m_applyingBandMemory) and value-identical echoes (RadioModel's
    // connect-time power push re-asserting what we just seeded) must neither
    // claim the band nor set the baseline. Without the change-gate, the
    // connect push at the model default bootstrapped defaultPercent=100 and
    // rewrote the start band's stored drive on every reconnect
    // (PR #4619 bench + review).
    const bool operatorChange = !m_applyingBandMemory
                                && clamped != m_rfPowerPercent;
    m_rfPowerPercent = clamped;
    if (operatorChange) {
        // The operator's drive belongs to the band they set it on.
        if (!m_currentBandKey.isEmpty())
            m_driveByBand.insert(m_currentBandKey, m_rfPowerPercent);
        // The FIRST drive an operator sets becomes the radio's baseline for
        // bands never visited (PR #4619 review: nothing else can ever raise
        // the sentinel, so the unvisited-band fallback was dead and a new
        // band inherited the previous band's drive). First-set-wins so later
        // per-band tweaks don't move the baseline.
        if (m_driveDefaultPercent < 0)
            m_driveDefaultPercent = m_rfPowerPercent;
    }
    notifyOperatingStateChanged();
    if (m_tuning)
        return;   // tune power owns the drive register until the carrier drops
    applyDrive(m_rfPowerPercent);
}

void Hl2Backend::setTxDriveLevel(int level)
{
    if (!m_metis)
        return;
    // Retained purely so the health snapshot can report what was ACTUALLY
    // written (#4912). The value went straight out over a queued invoke and was
    // kept nowhere, so nothing downstream could report the applied drive — only
    // TransmitModel's requested percent, which is the operator's ask.
    m_txDriveRegister = level;
    QMetaObject::invokeMethod(m_metis, "setTxDriveLevel", Qt::QueuedConnection,
        Q_ARG(int, level));
}

namespace {

// A restored mic level, read against the curve its document was written on.
// Absent `micLevelCurve` means curve 1 (the key postdates it); writing the
// level back stamps the curve, so migration happens once. Unknown (future)
// curves are taken as written rather than guessed.
[[nodiscard]] int migrateRestoredMicLevel(int level, const QJsonObject& txSetpoints)
{
    const QJsonValue curve = txSetpoints.value(QStringLiteral("micLevelCurve"));
    const int storedCurve = curve.toInt(0);
    if (curve.isDouble() && storedCurve > 1)
        return level;
    // Zero and negatives are not curve numbers from the future, they are a
    // damaged or hand-edited document — so they take the malformed branch and
    // SAY SO rather than silently suppressing the migration. The sibling
    // micLevel is range-validated by the caller for the same reason
    // (Principle VII); a curve that is merely unreadable must not be the one
    // field that fails quietly.
    if (!curve.isUndefined() && (!curve.isDouble() || storedCurve < 1)) {
        qCInfo(lcHl2) << "HL2: restored mic level carries a malformed curve"
                      << curve.toVariant() << "- reading it as written";
        return level;
    }
    const int migrated = AetherSDR::hl2::micLevelFromCurve1(level);
    if (migrated != level)
        qCInfo(lcHl2) << "HL2: restored mic level" << level
                      << "was stored against mic curve 1; it is"
                      << migrated << "on curve" << AetherSDR::hl2::kMicLevelCurve
                      << "- the same gain, a different position";
    return migrated;
}

// WDSP's AGC mode integer as the string the bridge and the operator use, so a
// read-back can be compared against what was asked for without the reader
// having to know WDSP's enumeration.
QString agcModeName(int wdspMode)
{
    switch (wdspMode) {
    case 0:  return QStringLiteral("off");
    case 1:  return QStringLiteral("long");
    case 2:  return QStringLiteral("slow");
    case 3:  return QStringLiteral("med");
    case 4:  return QStringLiteral("fast");
    default: return QStringLiteral("unknown(%1)").arg(wdspMode);
    }
}

}  // namespace

// What the DSP is actually configured with. See IRadioBackend::dspChains().
//
// The gather is a STATIC member taking the two lists it may read, so it has no
// `this` and cannot reach m_rx — see the declaration in the header for why that
// is the enforcement rather than a comment. dspChains() below is the one place
// that chooses what to hand it.
QVariantList Hl2Backend::gatherDspChains(const std::vector<Hl2RxDsp*>& rxDsps,
                                         Hl2TxDsp* txDsp)
{
    QVariantList chains;

    // rxDsps is the caller's snapshot, and everything below is a function of
    // it. Its callers hand it m_ioDsps — the I/O thread's own DDC-indexed list,
    // which is why the index reported here is the DDC — and never m_rx, whose
    // storage the GUI thread reallocates underneath a reader. Nothing here
    // needs Receiver in any case: every field reported comes from the DSP
    // object, which was the point of reading the DSP rather than the mirror.
    for (int i = 0; i < static_cast<int>(rxDsps.size()); ++i) {
        Hl2RxDsp* dsp = rxDsps[static_cast<std::size_t>(i)];
        QVariantMap e;
        e[QStringLiteral("chain")] = QStringLiteral("rx-wdsp");
        e[QStringLiteral("receiver")] = i;
        if (!dsp || !dsp->isConfigured()) {
            // Reported as present-but-unconfigured rather than omitted: a
            // receiver that exists with no channel behind it is exactly the
            // state worth seeing.
            e[QStringLiteral("level")] = QStringLiteral("not-configured");
            chains.append(e);
            continue;
        }
        const WdspChannel::Config* c = dsp->channelConfig();
        if (!c) {
            e[QStringLiteral("level")] = QStringLiteral("not-configured");
            chains.append(e);
            continue;
        }
        // "channel-config" and not "dsp": these are the values the channel
        // was OPENED with, after any clamping or refusal. That is one level
        // below the model and one above a query into WDSP itself, and the
        // difference decides what a mismatch proves.
        e[QStringLiteral("level")] = QStringLiteral("channel-config");
        e[QStringLiteral("inputRateHz")] = c->inputSampleRate;
        e[QStringLiteral("dspRateHz")] = c->dspSampleRate;
        e[QStringLiteral("outputRateHz")] = c->outputSampleRate;
        e[QStringLiteral("inputBlockSize")] = static_cast<int>(c->inputBlockSize);
        e[QStringLiteral("dspBlockSize")] = static_cast<int>(c->dspBlockSize);
        e[QStringLiteral("outputBlockSize")] =
            static_cast<int>(dsp->channelOutputBlockSize());
        e[QStringLiteral("filterLowHz")] = c->filterLowHz;
        e[QStringLiteral("filterHighHz")] = c->filterHighHz;
        e[QStringLiteral("agcMode")] = agcModeName(c->agcMode);
        e[QStringLiteral("agcMaxGainDb")] = c->maximumAgcGainDb;
        e[QStringLiteral("agcSlopeDb")] = c->agcSlopeDb;
        e[QStringLiteral("agcFixedGainDb")] = c->agcFixedGainDb;
        // Level 4 where it exists: these two ask WDSP itself rather than
        // reading the config, and are marked so a reader can tell.
        e[QStringLiteral("wdspNotchCount")] = dsp->wdspNotchCount();
        e[QStringLiteral("appliedNoiseBlanker")] =
            dsp->appliedNoiseBlankerEnabled();
        chains.append(e);
    }

    if (txDsp) {
        QVariantMap e;
        e[QStringLiteral("chain")] = QStringLiteral("hl2-tx");
        if (!txDsp->isConfigured()) {
            e[QStringLiteral("level")] = QStringLiteral("not-configured");
            chains.append(e);
            return chains;
        }
        const Hl2TxDsp::Config& t = txDsp->config();
        // WHICH MODULATOR THIS BINARY CARRIES. There is no runtime switch --
        // AETHER_HL2_TX_TXA decides it at compile time and the other chain is
        // not in the process -- so an operator cannot be on the wrong one. They
        // can be on the wrong BUILD, though, and a transmit report that does
        // not say which modulator produced the signal is not actionable. This
        // is what makes the build visible without making it switchable.
        e[QStringLiteral("modulator")] =
            QString::fromLatin1(Hl2TxDsp::modulatorName());
        // TXA reports the configuration accepted by WdspChannel; the phasing
        // implementation reports its local DSP configuration. Neither is RF
        // readback from the radio.
        const int txChannelId = txDsp->wdspChannelId();
        const WdspChannel::Config* channel = txDsp->channelConfig();
        e[QStringLiteral("level")] = channel ? QStringLiteral("channel-config")
                                             : QStringLiteral("dsp-config");
        if (txChannelId >= 0) {
            e[QStringLiteral("wdspChannelId")] = txChannelId;
            // Blocks the modulator could not place on the wire. NOT omitted
            // when it is zero: "no faults" and "nobody counted" must not look
            // the same, which is the whole lesson of the silent TXA failure
            // this build flag exists for.
            e[QStringLiteral("modulatorFaultBlocks")] =
                static_cast<qulonglong>(txDsp->modulatorFaultBlocks());
            e[QStringLiteral("modulatorBlocks")] =
                static_cast<qulonglong>(txDsp->modulatorBlocks());
        }
        e[QStringLiteral("inputRateHz")] = t.inputSampleRateHz;
        e[QStringLiteral("outputRateHz")] = t.outputSampleRateHz;
        e[QStringLiteral("dspBlockSize")] = channel
            ? static_cast<int>(channel->dspBlockSize) : t.dspBlockSize;
        if (channel) {
            e[QStringLiteral("inputBlockSize")] = static_cast<int>(channel->inputBlockSize);
            e[QStringLiteral("dspRateHz")] = channel->dspSampleRate;
        }
        e[QStringLiteral("filterLowHz")] = channel ? channel->filterLowHz : t.filterLowHz;
        e[QStringLiteral("filterHighHz")] = channel ? channel->filterHighHz : t.filterHighHz;
        e[QStringLiteral("alcEnabled")] = t.alcEnabled;
        e[QStringLiteral("alcTargetPeak")] = t.alcTargetPeak;
        e[QStringLiteral("alcReleaseSec")] = t.alcReleaseSec;
        e[QStringLiteral("micGainLinear")] = txDsp->micGain();
        chains.append(e);
    }
    return chains;
}

// Gathered on the I/O thread, because that is where both chains live and this
// is called from the GUI thread. Same shape as AutomationServer's
// dspSnapshotOnObjectThread(): a same-thread fast path, otherwise a blocking
// queued invocation. A failed invocation returns empty rather than a
// half-filled list — "we could not ask" and "it answered zero" must not look
// alike, which is the same rule healthSnapshot() follows.
QVariantList Hl2Backend::dspChains() const
{
    // THE ONE LINE THAT CHOOSES. m_ioDsps, never m_rx: the I/O side gets its own
    // DDC-indexed list, rebuilt by publishIoDsps() when the receiver set
    // changes, and the EP6 fan-out reads it for the same reason. Nothing in the
    // gather needs Receiver anyway — every field it reports comes from the DSP
    // object, which was the point of reading the DSP rather than the mirror.
    if (!m_txDsp || m_txDsp->thread() == QThread::currentThread()) {
        return gatherDspChains(m_ioDsps, m_txDsp);
    }
    if (!m_ioThread || !m_ioThread->isRunning()) {
        return {};  // no event loop can answer a blocking invocation
    }

    QVariantList out;
    const bool invoked = QMetaObject::invokeMethod(
        m_txDsp, [this, &out]() { out = gatherDspChains(m_ioDsps, m_txDsp); },
        Qt::BlockingQueuedConnection);
    return invoked ? out : QVariantList{};
}

void Hl2Backend::invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                                 const QVariant& arg)
{
    if (ns == QLatin1String("hl2")) {
        // Manual frequency calibration. Completes LOCALLY — unlike the Flex
        // tuner/amp verbs this namespace is modelled on, there is no device
        // round trip to await: the correction is a host-side scalar and the
        // radio is never asked about it. So the reply is emitted synchronously
        // rather than fabricated later.
        if (verb == QLatin1String("freqcal.set")) {
            // REFUSED UNDER A LOCKED REFERENCE, not silently clamped to zero.
            // §4 asks for the control to be disabled; a caller that reaches the
            // verb anyway — the bridge, a client that missed the state — gets
            // told why rather than getting a success it did not receive. The
            // dialog dims the control for the same reason, but the dialog is
            // not the authority on this and must not be the only guard.
            if (m_hw.cl1RefClock && Hl2FreqCal::clampPpb(arg.toInt()) != 0) {
                if (requestId != 0) {
                    emit extensionError(requestId,
                        QStringLiteral("freqcal: the radio is locked to an external 10 MHz "
                                       "reference at CL1, which already removes the crystal's "
                                       "error — a manual correction would reintroduce it. "
                                       "Clear the CL1 setting on the HL2 Hardware page first."));
                }
                return;
            }
            applyFreqCalPpb(arg.toInt(), /*persist=*/true);
            if (requestId != 0)
                emit extensionResult(requestId, QVariant(m_freqCalPpb));
            return;
        }
        // Live trim: apply and re-push, but do NOT touch the settings store.
        // Trim auto-repeats at 120 ms and AppSettings::save() is a full
        // non-atomic file write, so persisting every repeat would hammer the
        // store eight times a second. The UI commits once on button release.
        if (verb == QLatin1String("freqcal.set_live")) {
            // Same gate as freqcal.set. A live trim under a locked reference is
            // the same error arriving 120 ms at a time.
            if (m_hw.cl1RefClock && Hl2FreqCal::clampPpb(arg.toInt()) != 0) {
                if (requestId != 0) {
                    emit extensionError(requestId,
                        QStringLiteral("freqcal: locked to the external reference at CL1"));
                }
                return;
            }
            applyFreqCalPpb(arg.toInt(), /*persist=*/false);
            if (requestId != 0)
                emit extensionResult(requestId, QVariant(m_freqCalPpb));
            return;
        }
        // WHICH HL2 THIS IS. Completes locally, exactly like the
        // calibration verbs above and for the same reason: nothing in
        // Protocol 1 reads any of these back, so a device round trip to await
        // would be a confirmation the wire cannot give. What the radio DOES
        // with them is visible on the air, not in a reply.
        if (verb == QLatin1String("hw.get")) {
            if (requestId != 0) {
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("codec"), static_cast<int>(m_hw.codec)},
                    {QStringLiteral("ditherBit"), m_hw.ditherBit},
                    // The intent AND the wire value. No variant overrides the
                    // operator any more (Hl2HardwareOptions::ditherBitOnWire),
                    // so the two agree today — reported as a pair anyway so a
                    // caller reads the wire from the field that names it, and
                    // a future override cannot change this reply's meaning
                    // without changing its shape.
                    {QStringLiteral("ditherBitOnWire"), m_hw.ditherBitOnWire()},
                    {QStringLiteral("randomBit"), m_hw.randomBit},
                    {QStringLiteral("filterBoard"), static_cast<int>(m_hw.filterBoard)},
                    {QStringLiteral("n2adrHpf"), m_hw.n2adrHpf},
                    {QStringLiteral("cl1RefClock"), m_hw.cl1RefClock},
                    {QStringLiteral("atuGateware"), m_hw.atuGateware},
                    {QStringLiteral("speakerLevelPercent"), m_hw.speakerLevelPercent},
                });
            }
            return;
        }
        if (verb == QLatin1String("hw.set")) {
            const QVariantMap in = arg.toMap();
            // PARTIAL BY DESIGN: a caller sets the one field it cares about and
            // every absent key keeps its current value. Defaulting an absent
            // key to false instead would let a bridge command that meant to
            // change the filter board silently switch off the operator's codec.
            Hl2HardwareOptions next = m_hw;
            const auto boolOr = [&in](const char* key, bool current) {
                const auto it = in.constFind(QLatin1String(key));
                return it == in.constEnd() ? current : it->toBool();
            };
            if (in.contains(QStringLiteral("codec"))) {
                next.codec = Hl2HardwareOptions::clampCodec(
                    in.value(QStringLiteral("codec")).toInt());
                // A board change re-seeds the dither bit, because 0x00[11] means
                // band volts on a bare HL2 and a loudspeaker on codec boards.
                // Done here, not in the dialog, so bridge callers get the same
                // rule and the dialog needs no vendor(hl2) header. An explicit
                // ditherBit in the same call still wins (boolOr() below).
                if (next.codec != m_hw.codec) {
                    next.ditherBit = Hl2HardwareOptions::ditherBitOnCodecChange(
                        next.codec, m_hw.ditherBit);
                }
            }
            if (in.contains(QStringLiteral("filterBoard")))
                next.filterBoard = Hl2HardwareOptions::clampFilterBoard(
                    in.value(QStringLiteral("filterBoard")).toInt());
            next.ditherBit   = boolOr("ditherBit", next.ditherBit);
            next.randomBit   = boolOr("randomBit", next.randomBit);
            next.n2adrHpf    = boolOr("n2adrHpf", next.n2adrHpf);
            next.cl1RefClock = boolOr("cl1RefClock", next.cl1RefClock);
            next.atuGateware = boolOr("atuGateware", next.atuGateware);
            if (in.contains(QStringLiteral("speakerLevelPercent")))
                next.speakerLevelPercent = Hl2HardwareOptions::clampSpeakerLevel(
                    in.value(QStringLiteral("speakerLevelPercent")).toInt());
            applyHardwareOptions(next, /*persist=*/true);
            if (requestId != 0) {
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("codec"), static_cast<int>(m_hw.codec)},
                    {QStringLiteral("ditherBit"), m_hw.ditherBit},
                    {QStringLiteral("ditherBitOnWire"), m_hw.ditherBitOnWire()},
                    {QStringLiteral("randomBit"), m_hw.randomBit},
                    {QStringLiteral("filterBoard"), static_cast<int>(m_hw.filterBoard)},
                    {QStringLiteral("n2adrHpf"), m_hw.n2adrHpf},
                    {QStringLiteral("cl1RefClock"), m_hw.cl1RefClock},
                    {QStringLiteral("atuGateware"), m_hw.atuGateware},
                    {QStringLiteral("speakerLevelPercent"), m_hw.speakerLevelPercent},
                });
            }
            return;
        }
        if (verb == QLatin1String("freqcal.get")) {
            if (requestId != 0) {
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("ppb"), m_freqCalPpb},
                    {QStringLiteral("effectiveClockHz"),
                     Hl2FreqCal::effectiveClockHz(m_freqCalPpb)},
                    {QStringLiteral("scale"), m_freqCalScale},
                    // Whether a manual correction is allowed at all right now.
                    // Reported HERE rather than left for the caller to derive
                    // from hw.get, so that a client reading the calibration
                    // state gets the reason it is refused in the same answer as
                    // the value — see §4 and freqcal.set below.
                    {QStringLiteral("externalReference"), m_hw.cl1RefClock},
                });
            }
            return;
        }
        // Wideband bandscope (EP 0x04): a diagnostic with no UI or setting, off
        // again at next connect; the only caller is the bridge's `bandscope`
        // verb (the auto RF gain loop starts the gate itself, in
        // applyBandscopeForAutoGain). This starts MetisClient's duty-cycle gate,
        // not the raw stream: one 2048-sample block per period, 12 datagrams/s,
        // 0.11 Mbit/s (3.3 Mbit/s ungated; see setBandscopeEnabled in
        // MetisClient.h). Completes locally: Protocol 1 never reads the run byte
        // back.
        if (verb == QLatin1String("bandscope.enable")) {
            // Refused while disconnected, and REPORTED as refused: MetisClient
            // ignores a run byte with no stream behind it, so echoing the
            // request back would be this side inventing a state the radio was
            // never told about.
            const bool on = arg.toBool() && m_connected;
            // Not mirrored here: the health row follows LinkCounters (what
            // MetisClient has). An explicit operator action takes ownership
            // from the auto-gain loop in both directions; see
            // applyBandscopeForAutoGain.
            m_bandscopeOwnedByAutoGain = false;
            QMetaObject::invokeMethod(m_metis, "setBandscopeEnabled",
                                      Qt::QueuedConnection, Q_ARG(bool, on));
            if (requestId != 0) {
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("enabled"), on},
                });
            }
            return;
        }
        // One wideband frame on demand: the only request/reply verb here,
        // since the radio must arm a cycle to produce the block. On demand
        // rather than continuous because a permanent consumer's load on the
        // hl2-io thread is unmeasured; one frame costs ~13 ms of wide_spectrum.
        if (verb == QLatin1String("bandscope.frame")) {
            if (requestId == 0) {
                // Nothing to send the samples to. Refusing is the honest
                // answer: the alternative is putting a block on the wire and
                // throwing it away.
                return;
            }
            if (!m_connected) {
                emit extensionError(requestId,
                                    QStringLiteral("Not connected to a radio."));
                return;
            }
            if (m_bandscopeFrameRequest != 0) {
                // One outstanding at a time. The second caller is told so
                // rather than silently inheriting the first one's frame.
                emit extensionError(
                    requestId,
                    QStringLiteral("A bandscope frame is already pending."));
                return;
            }
            m_bandscopeFrameRequest = requestId;
            QMetaObject::invokeMethod(m_metis, "requestBandscopeFrame",
                                      Qt::QueuedConnection);
            return;
        }
        // Noise-blanker READBACK, per receiver. Exists because the bridge's
        // `get dsp` reports the SLICE MODEL's nb flag, which is set the moment
        // the operator clicks and says nothing about whether the intent reached
        // the DSP. On a radio whose blanker is a host-side stage with no wire
        // traffic to capture, that gap is the whole difficulty of proving the
        // feature: a backend that dropped the verb entirely would still report
        // nb=true. This answers from the backend's own state instead.
        if (verb == QLatin1String("nb.get")) {
            if (requestId != 0) {
                QVariantList rxList;
                for (std::size_t i = 0; i < m_rx.size(); ++i) {
                    const Receiver& r = m_rx[i];
                    const auto* ids = m_ids.byDdc(static_cast<int>(i));
                    // `on`/`level` are what the DSP has applied (Hl2RxDsp's
                    // atomics), not the request in r.nbOn; requestedOn/
                    // requestedLevel are reported alongside so a mismatch shows.
                    // No chain means `on` is false.
                    const bool appliedOn = r.dsp && r.dsp->appliedNoiseBlankerEnabled();
                    const int appliedLevel =
                        r.dsp ? r.dsp->appliedNoiseBlankerLevel() : 0;
                    rxList.append(QVariantMap{
                        {QStringLiteral("ddc"), static_cast<int>(i)},
                        {QStringLiteral("panId"), ids ? ids->panId : QString()},
                        {QStringLiteral("on"), appliedOn},
                        {QStringLiteral("level"), appliedLevel},
                        {QStringLiteral("requestedOn"), r.nbOn},
                        {QStringLiteral("requestedLevel"), r.nbLevel},
                        {QStringLiteral("hasChain"), r.dsp != nullptr},
                        // The value the APPLIED level became inside WDSP. Now a
                        // real assertion rather than f(x) == f(x): it is
                        // computed from the level the DSP took, so a request
                        // that never crossed the seam shows a threshold that
                        // does not match the requested level.
                        {QStringLiteral("threshold"),
                         WdspChannel::noiseBlankerThresholdForLevel(appliedLevel)},
                    });
                }
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("receivers"), rxList},
                });
            }
            return;
        }
    }
    // No other HL2 extension verbs; honor the async contract without hanging.
    if (requestId != 0)
        emit extensionError(requestId, QStringLiteral("hl2: no extension verbs implemented"));
}

namespace {
// The control law's reason enum in the operator's words. Kept here rather than
// in the policy header so the header stays free of anything presentational --
// it has no Qt and no strings, and a health row is not a control decision.
const char* autoGainReasonText(AetherSDR::hl2::AutoGainReason r)
{
    using R = AetherSDR::hl2::AutoGainReason;
    switch (r) {
    case R::Disarmed:       return "off";
    case R::Warmup:         return "warming up";
    case R::Keyed:          return "held — transmitting";
    case R::UnkeyHoldoff:   return "held — settling after unkey";
    case R::Void:           return "waiting — too few observations";
    case R::Stale:          return "held — no observations (is the radio streaming?)";
    case R::Cooldown:       return "clipping — waiting out the attack cooldown";
    case R::AttackHot:      return "reducing gain — clipping most of the time";
    case R::AttackMarginal: return "reducing gain — clipping occasionally";
    case R::AtFloor:        return "AT FLOOR and still clipping";
    case R::Dwell:          return "clean — waiting before giving gain back";
    case R::ReleaseHold:    return "clean — holding at this band's known limit";
    // The two the wideband reading adds, and they are deliberately different
    // sentences: one is a measurement that said no, the other is no
    // measurement at all, and an operator can act on the second (is the
    // bandscope running?) but not on the first.
    case R::HeadroomHold:   return "clean — not enough measured headroom for a step";
    case R::HeadroomAbsent: return "held — no wideband headroom reading";
    case R::Release:        return "giving gain back";
    case R::Idle:           return "clean — nothing held";
    }
    return "unknown";
}
}  // namespace

// RFC #5535's visibility condition, published. Emitted from the two places the
// inputs move -- the 10 Hz telemetry window and the control tick -- and gated on
// CHANGE, because an indicator that repaints at 10 Hz forever is a distraction
// rather than a signal, and a screen reader driven at that rate is unusable.
void Hl2Backend::publishFrontEndOverload()
{
    AetherSDR::FrontEndOverload s;

    // LEVEL, from the family's own classifier rather than a second opinion.
    // Hl2AutoGainPolicy already decides what "clipping occasionally" means on
    // this front end, and two thresholds for one question is how a readout and
    // a regulator end up disagreeing in front of an operator.
    using W = AetherSDR::hl2::AutoGainWindow;
    const W w = AetherSDR::hl2::classifyWindow(
        m_adcWindowSamples, m_adcOverloadWindowSamples, m_autoGainConfig);
    switch (w) {
    case W::Void:     s.level = AetherSDR::FrontEndLevel::Unobserved; break;
    case W::Clean:    s.level = AetherSDR::FrontEndLevel::Clean;      break;
    case W::Marginal: s.level = AetherSDR::FrontEndLevel::Marginal;   break;
    case W::Hot:      s.level = AetherSDR::FrontEndLevel::Hot;        break;
    }

    // AT FLOOR OUTRANKS THE WINDOW. Still clipping with the loop as deep as it
    // is allowed to go is the one state no amount of gain management fixes, and
    // it must not read as an ordinary Hot window that the regulator is busy
    // handling -- it is precisely the case where it cannot.
    if (m_autoGainReason == AetherSDR::hl2::AutoGainReason::AtFloor) {
        s.level = AetherSDR::FrontEndLevel::AtFloor;
    }

    s.autoArmed = m_autoRfGainEnabled;
    s.autoOffsetDb = m_autoGainState.offsetDb;
    s.reason = QString::fromUtf8(autoGainReasonText(m_autoGainReason));

    if (s == m_lastFrontEndOverload) {
        return;
    }
    m_lastFrontEndOverload = s;
    emit frontEndOverloadChanged(s);
}


IRadioBackend::HealthSnapshot Hl2Backend::healthSnapshot() const
{
    HealthSnapshot h;
    auto put = [&h](const char* key, const QString& label, const QVariant& v) {
        const QString k = QString::fromLatin1(key);
        h.order.append(k);
        h.labels.insert(k, label);
        // An INVALID variant is left out of `values` on purpose: that is what
        // renders as "not reported". Writing a zero here instead would turn
        // "this radio never told us its FIFO depth" into "the FIFO is empty",
        // which is the single most misleading thing a health readout can do.
        if (v.isValid())
            h.values.insert(k, v);
    };
    auto section = [&h](const char* key, const QString& title) {
        h.sections.insert(QString::fromLatin1(key), title);
    };
    // std::optional -> QVariant, preserving "not seen yet" as an invalid variant.
    auto opt = [](const auto& o) -> QVariant {
        return o ? QVariant(*o) : QVariant();
    };


    // In-band readings only. AutomationServer::doHealth() merges these over
    // Hl2TelemetryService's stream-free rows, in-band winning (10 Hz vs 1-2 Hz),
    // so the stream-free rows still work with no backend. m_telemetry is never
    // cleared, so these rows report nothing (invalid variants, omitted from
    // `values`) unless the link is Streaming; zeros would erase the poller's.
    static const Hl2Telemetry kNoInBandReadings{};
    const bool inBandLive = telemetryLinkState() == Hl2LinkState::Streaming;
    const Hl2Telemetry& t = inBandLive ? m_telemetry : kNoInBandReadings;

    section("connected", QStringLiteral("Radio"));
    put("connected", QStringLiteral("Connected"), m_connected);
    put("model", QStringLiteral("Model"), QStringLiteral("Hermes-Lite 2"));
    // From EP6 RADDR 0, C4[7:0]. This is the GATEWARE's firmware revision as
    // the running radio reports it — not the version string the discovery reply
    // carried, which is only ever seen by the radio picker before connecting.
    put("firmwareVersion", QStringLiteral("Firmware version"),
        opt(t.firmwareVersion));

    section("adcOverload", QStringLiteral("Converter"));
    // The two ADC readings side by side; they disagree by design. Pre-DDC sees
    // the whole converter, post-DDC sees one slice, so a quiet slice can hide a
    // saturating converter. Neither is calibrated and they share no scale; only
    // the pairing is meaningful. Display only; reasoning in Hl2AdcPairing.h.
    // Uses `t`, so the flag is absent unless the link is Streaming (#5414).
    put("adcOverload", QStringLiteral("ADC overload (pre-DDC, 0–38.4 MHz)"),
        opt(t.adcOverload));
    // Per receiver, because the post-DDC half of the pairing is per SLICE: two
    // receivers on different bands get two different answers from one converter
    // flag, which is the multi-slice form of the same disagreement.
    for (const auto& ids : m_ids.all()) {
        const Receiver* r = rx(ids.ddcIndex);
        // `!r` is a receiver that does not exist; a receiver with no DSP chain
        // is one between rebuilds, and that is "not reported", not "not there".
        // Dropping its rows would take their labels with them — the sibling
        // loop below guards on `!r` alone and the noise-blanker extension verb
        // reports `hasChain: false` rather than omitting, for the same reason.
        if (!r)
            continue;
        const QString suffix = m_ids.size() > 1
                                   ? QStringLiteral(" (RX%1)").arg(ids.uiNumber + 1)
                                   : QString();
        // ABSENT UNTIL A BLOCK HAS BEEN PROCESSED, which is HealthSnapshot's
        // "absent means not reported" contract doing work no default could:
        // 0.00 dBFS in particular would read as a hard clip.
        const std::optional<double> peak =
            r->dsp ? r->dsp->adcPeakDbfs() : std::nullopt;
        const bool realPeak = peak && hl2::adcMeterReadingIsReal(*peak);
        put(QStringLiteral("adcSlicePeakDbfs%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("ADC peak, post-DDC slice (uncalibrated dBFS)") + suffix,
            realPeak ? QVariant(QString::number(*peak, 'f', 2)) : QVariant());
        // How old that reading is. It STOPS ADVANCING while transmitting — the
        // receive chain is clocked with silence there, so Hl2RxDsp deliberately
        // holds the last receive value rather than measuring our own mute — and
        // it stops advancing again if the IQ stream stalls. Without an age on
        // it, a frozen number reads as a current one. The pairing row below
        // does not merely display this age, it is GATED on it: see
        // kSliceStaleMs.
        const std::optional<std::int64_t> ago =
            r->dsp ? r->dsp->adcPeakObservedAgoMs() : std::nullopt;
        put(QStringLiteral("adcSliceObservedAgoMs%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("Post-DDC slice peak observed (ms ago)") + suffix,
            ago ? QVariant(static_cast<qulonglong>(*ago)) : QVariant());
        // The pairing as one sentence. Two liveness gates, because `peak` is
        // held through transmit while adcOverload keeps moving. The sampling
        // gate (SliceSamplingGate) catches key-down at once and only reopens
        // on a peak stamped after the resume request; the age gate
        // (kSliceStaleMs) catches stalls and rebuilds. See Hl2AdcPairing.h.
        // NaN is the don't-care because 0.0 dBFS is a real reading.
        const hl2::AdcPairing verdict =
            hl2::adcPairing(realPeak,
                            realPeak ? *peak : std::numeric_limits<double>::quiet_NaN(),
                            ago && *ago <= hl2::kSliceStaleMs,
                            m_sliceSampling.applied(
                                r->dsp ? r->dsp->adcPeakObservedAtNs() : 0),
                            t.adcOverload.has_value(),
                            t.adcOverload.value_or(false));
        const QString headroom =
            realPeak ? QString::number(hl2::sliceHeadroomDb(*peak), 'f', 1) : QString();
        // A slice peak can sit ABOVE wire full scale — I and Q each at +-1 puts
        // the magnitude at sqrt(2), about +3 dB — and "within -1.5 dB of full
        // scale" is not a sentence. Say what is actually true instead.
        const bool overFullScale = realPeak && hl2::sliceHeadroomDb(*peak) < 0.0;
        QVariant pairing;
        switch (verdict) {
        case hl2::AdcPairing::Unknown:
            break;   // one side has not reported; renders as "not reported"
        case hl2::AdcPairing::BothClear:
            pairing = QStringLiteral(
                          "agree — no converter overload, slice %1 dB below full scale")
                          .arg(headroom);
            break;
        case hl2::AdcPairing::ConverterOnly:
            pairing = QStringLiteral(
                          "DISAGREE — converter overloading while this slice sits %1 dB "
                          "below full scale; the signal doing it is elsewhere in "
                          "0–38.4 MHz")
                          .arg(headroom);
            break;
        case hl2::AdcPairing::SliceOnly:
            pairing = overFullScale
                          ? QStringLiteral(
                                "DISAGREE — slice above full scale, converter not "
                                "overloading; the level is arriving through the DDC, "
                                "not at the front end")
                          : QStringLiteral(
                                "DISAGREE — slice within %1 dB of full scale, converter "
                                "not overloading; the level is arriving through the "
                                "DDC, not at the front end")
                                .arg(headroom);
            break;
        case hl2::AdcPairing::BothHot:
            pairing = overFullScale
                          ? QStringLiteral(
                                "agree — converter overloading and the slice is above "
                                "full scale; the strong signal is in this slice")
                          : QStringLiteral(
                                "agree — converter overloading and the slice is within "
                                "%1 dB of full scale; the strong signal is in this "
                                "slice")
                                .arg(headroom);
            break;
        }
        put(QStringLiteral("adcPairing%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("Pre-DDC vs post-DDC") + suffix, pairing);
    }
    // THE VALUE ON THE WIRE. Kept under its original key because that is what
    // this row has always meant — what the AD9866 is running — and because a
    // reader that wanted the operator's stored number would be reaching for the
    // wrong one either way. The two rows below say which is which, and while no
    // automatic control is armed all three agree.
    put("lnaGainDb", QStringLiteral("LNA gain (dB)"), lnaEffectiveDb());
    put("lnaBaselineDb", QStringLiteral("LNA baseline (dB, operator)"), m_lnaGainDb);
    put("lnaAutoOffsetDb", QStringLiteral("LNA auto offset (dB below baseline)"),
        m_lnaAutoOffsetDb);
    // Clip rate with its denominator: the single overload bit is sampled at
    // 10 Hz from a flag cycling up to ~190 Hz, so it is nearly a coin toss.
    // The percentage is absent when the window had too few observations. The
    // gateware clears this counter only in the EP6 response cycle, so the rows
    // go quiet when streaming stops; "observed" gives the last window's age.
    put("adcClipWindowSamples", QStringLiteral("ADC observations (last window)"),
        m_adcWindowSamples > 0 ? QVariant(m_adcWindowSamples) : QVariant());
    put("adcClipWindowOverload", QStringLiteral("ADC clipped observations (last window)"),
        m_adcWindowSamples > 0 ? QVariant(m_adcOverloadWindowSamples) : QVariant());
    {
        const auto pct = AetherSDR::hl2::adcClipRatePercent(
            m_adcWindowSamples, m_adcOverloadWindowSamples, kAdcMinWindowSamples);
        put("adcClipWindowPct", QStringLiteral("ADC clip rate (last window, %)"),
            pct ? QVariant(*pct) : QVariant());
    }
    put("adcClipWindowMs", QStringLiteral("Window length (ms)"),
        m_adcWindowMs > 0 ? QVariant(m_adcWindowMs) : QVariant());
    put("adcClipTotalSamples", QStringLiteral("ADC observations (since connect)"),
        m_adcTotalSamples > 0 ? QVariant(m_adcTotalSamples) : QVariant());
    put("adcClipTotalOverload", QStringLiteral("ADC clipped observations (since connect)"),
        m_adcTotalSamples > 0 ? QVariant(m_adcTotalOverloadSamples) : QVariant());
    {
        // Deliberately computed through the SAME function as the window row,
        // so the two can never disagree about what a rate is or when there
        // isn't one. int is safe: the totals are clamped into it only for the
        // ratio, and a session would need billions of observations to matter.
        const auto pct = AetherSDR::hl2::adcClipRatePercent(
            static_cast<int>(m_adcTotalSamples > 1'000'000'000u
                                 ? 1'000'000'000u : m_adcTotalSamples),
            static_cast<int>(m_adcTotalOverloadSamples > 1'000'000'000u
                                 ? 1'000'000'000u : m_adcTotalOverloadSamples),
            kAdcMinWindowSamples);
        put("adcClipSessionPct", QStringLiteral("ADC clip rate (since connect, %)"),
            pct ? QVariant(*pct) : QVariant());
    }
    put("adcObservedMsAgo", QStringLiteral("ADC last observed (ms ago)"),
        m_adcWindowClock.isValid() ? QVariant(qint64(m_adcWindowClock.elapsed()))
                                   : QVariant());
    put("autoRfGain", QStringLiteral("Auto RF gain"), m_autoRfGainEnabled);
    put("autoRfGainFloorDb", QStringLiteral("Auto RF gain floor (dB below baseline)"),
        m_autoGainConfig.maxOffsetDb);
    // WHICH LAW, not which numbers. The rows above show what the loop is doing;
    // this one shows which controller is doing it, and without it a bench
    // reading a trace has no way to tell a slow ramp from a failed probe.
    put("autoRfGainMode", QStringLiteral("Auto RF gain mode"), m_autoGainMode);
    put("autoRfGainProbeIntervalMs",
        QStringLiteral("Auto RF gain probe interval (ms, 0 = not probing)"),
        m_autoGainConfig.probeConfirmMs > 0
            ? QVariant(qint64(m_autoGainState.dwellRequiredMs > 0
                                  ? m_autoGainState.dwellRequiredMs
                                  : m_autoGainConfig.releaseDwellMs))
            : QVariant(qint64(0)));
    // What the loop is doing THIS INSTANT, in the operator's words rather than
    // the enum's. Reported even while disarmed, because "disarmed" is the
    // answer to "why is nothing happening" as much as "stale" is.
    put("autoRfGainState", QStringLiteral("Auto RF gain state"),
        QString::fromLatin1(autoGainReasonText(m_autoGainReason)));

    // ---- WHAT THE LOOP IS RELEASING AGAINST ----
    //
    // Three rows, and together they answer "why has it not given my gain back"
    // in a way neither the state string nor the raw headroom row can alone.
    //
    // ABSENT WHEN THE LAW DOES NOT USE IT. A loop running "ramp", "probe" or
    // "binary" releases on the clip flag alone, and showing a headroom
    // requirement beside it would describe a rule that is not in force.
    if (m_autoGainConfig.requireHeadroomToRelease) {
        const AetherSDR::hl2::HeadroomObservation h =
            AetherSDR::hl2::bandscopeHeadroom(m_bandscopeBlock,
                                              bandscopeBlockAgeMs());
        // How much room the step needs: the step, plus the gate's sampling
        // bias, plus the configured margin. This is the number the reading is
        // actually compared against, so publishing it saves an operator from
        // doing the arithmetic in their head against three other rows.
        put("autoRfGainReleaseNeedsDb",
            QStringLiteral("Auto RF gain: headroom needed to release (dB)"),
            QString::number(m_autoGainConfig.releaseStepDb
                                + m_autoGainConfig.headroomBiasDb
                                + m_autoGainConfig.releaseHeadroomMarginDb,
                            'f', 2));
        // And how much of that requirement is the duty cycle's bias budget
        // rather than the step -- which is the part that looks arbitrary until
        // it is named.
        put("autoRfGainHeadroomBiasDb",
            QStringLiteral("Auto RF gain: gated-peak bias budgeted (dB)"),
            QString::number(m_autoGainConfig.headroomBiasDb, 'f', 2));
        // ABSENT, not zero, when there is no current reading. Zero headroom is
        // an instruction to attenuate; no reading is an instruction to wait,
        // and rendering the second as the first would be the worst available
        // mistake on this row.
        put("autoRfGainHeadroomDb",
            QStringLiteral("Auto RF gain: measured headroom (dB below clip)"),
            h.isMeasurement() ? QVariant(QString::number(h.headroomDb, 'f', 2))
                              : QVariant());
    }

    section("txInhibited", QStringLiteral("Transmit"));
    // The register bit is ACTIVE LOW and MetisProtocol already decodes it, so
    // what is shown here is the plain-language sense: true means transmit is
    // being held off. Displaying the raw bit would read exactly backwards.
    put("txInhibited", QStringLiteral("TX inhibited"), opt(t.txInhibited));
    // A plain bool, so unlike every field above it has no "not reported"
    // value of its own: blanking `t` would publish false, and false WINS the
    // merge and would overwrite the poller's fresh PTT with a claim that the
    // radio is unkeyed. So the absence is spelled here instead.
    put("ptt", QStringLiteral("PTT (radio)"), inBandLive ? QVariant(t.ptt) : QVariant());
    put("keyed", QStringLiteral("Keyed (app)"), m_keyed);
    put("tuning", QStringLiteral("Tune carrier"), m_tuning);
    // The FPGA's transmit sample buffer. The oracle calls its depth "the most
    // important number in the protocol"; the gateware at 883a338 does not send
    // a depth. It sends the TOP 7 BITS of the fill level and one fault flag
    // (fifos.v:100-110) — so this reads as a level out of 127, not a count, and
    // the label says so rather than inviting the reader to treat 64 as samples.
    // Rising means we are sending faster than the radio consumes; falling is an
    // impending underrun. See MetisProtocol.cpp's apply() for the full layout
    // and for what the previous three rows here got wrong.
    put("txFifoFillMsbs", QStringLiteral("TX FIFO fill (0-127, coarse)"),
        opt(t.txFifoFillMsbs));
    // ONE bit for TWO faults: ran empty, or writes blocked after filling. The
    // gateware does not distinguish them, so this must not be split into an
    // underflow row and an overflow row — the two rows that stood here reported
    // opposite faults for the same flag depending on fill-level bit 6.
    put("txFifoRecovery", QStringLiteral("TX pacing fault (under OR overrun)"),
        opt(t.txFifoRecovery));

    // Drive requested vs written (#4912): applyDrive()'s TX gate forces the
    // register to 0 while the percent reads back unchanged. The raw register
    // is shown too because the gateware decodes only the top nibble, so 100
    // percents map onto 16 drive steps.
    put("rfPowerPercent", QStringLiteral("Drive requested (0-100)"), m_rfPowerPercent);
    // Absent until first write, per this section's "never told" convention — a
    // 0 here would read as "the radio was commanded to zero drive".
    put("txDriveRegister", QStringLiteral("Drive written (raw 0-255)"),
        m_txDriveRegister >= 0 ? QVariant(m_txDriveRegister) : QVariant());
    // Reported from the GATE, not from an observation of it acting. Latching this
    // inside applyDrive() looked equivalent and was not: finishDspSetup() seeds the
    // register with a direct setTxDriveLevel(0) that never goes through applyDrive(),
    // so a TX-blocked session where nobody touched the drive slider read
    // "rfPowerPercent: 100, txDriveRegister: 0, txDriveGated: false" — the row
    // positively denying responsibility for the exact divergence it exists to
    // explain. The gate is a session property, so it is always knowable.
    put("txDriveGated", QStringLiteral("Drive held at 0 by the TX gate"), !m_txAllowed);

    // The voice chain as the modulator runs it, read from this backend rather
    // than TransmitModel, so request and applied value can be compared.
    section("micLevel", QStringLiteral("Transmit voice chain"));
    put("micLevel", QStringLiteral("Mic slider (0-100, 50 = unity)"), m_micLevel);
    // NUMERIC ON EVERY PATH. An earlier revision reported the string "muted"
    // here at slider 0 and a number everywhere else, which reads fine in the
    // dialog and breaks the bridge: `health` serialises this row straight to
    // JSON, so any script comparing it changes type underneath itself at
    // exactly the value most likely to be under a microscope. The mute is a
    // separate FACT, not a gain — micSliderToLinear() short-circuits to 0.0
    // rather than resolving the -20 dB this mapping would otherwise give it —
    // so it is reported as its own row rather than smuggled into this one's
    // type.
    put("micGainDb", QStringLiteral("Mic gain requested (dB, continuous mapping)"),
        micSliderToGainDb(m_micLevel));
    put("micMuted", QStringLiteral("Mic muted (slider at 0)"), m_micLevel == 0);
    // THE ROW THAT WOULD HAVE CAUGHT THE ORIGINAL BUG. Echoed by the modulator,
    // so it stays absent — "not reported" — if the push never arrived, however
    // confidently the row above claims a value.
    //
    // It is what the modulator HOLDS, which is not always what it APPLIES: on an
    // engine-generated over processAudioBlock() substitutes 1.0 and this value
    // sits unused. txAudioSource below is how a reader tells which happened.
    put("micGainAppliedLinear", QStringLiteral("Mic gain at the modulator (linear)"),
        std::isnan(m_appliedMicGainLinear) ? QVariant()
                                           : QVariant(m_appliedMicGainLinear));
    // Which levelling rule this transmission took: engine audio bypasses the
    // mic slider. Sticky per transmission (cleared on each key edge in
    // setKeying()), so a mixed over reports "mic+engine".
    put("txAudioSource", QStringLiteral("Audio source this over (mic slider applies?)"),
        [this]() -> QVariant {
            QStringList carried;
            if (m_txAudioEngineGenerated)
                carried << QStringLiteral("engine-generated (slider bypassed)");
            if (m_txAudioClientLeveled)
                carried << QStringLiteral("client-leveled (slider applies)");
            // Neither sticky flag set, but audio did arrive: the mic path is the
            // only remaining producer, so name it rather than reporting nothing.
            if (carried.isEmpty() && m_txMicPeakMaxDbfs > -139.0f)
                carried << QStringLiteral("microphone (slider applies)");
            // Nothing at all this over — absent, not a guess.
            if (carried.isEmpty())
                return QVariant();
            return carried.join(QStringLiteral(" + "));
        }());
    // Peak mic level for the CURRENT transmission, reset at each key. Compared
    // against the ALC target below, these two rows are the whole "why did I go
    // out quiet" diagnosis: the ALC only reduces, so this peak IS the on-air
    // level up to the target, and a peak well under it means nothing lifted it
    // and the answer is mic gain.
    put("txMicPeakDbfs", QStringLiteral("Mic peak this over (dBFS)"),
        m_txMicPeakMaxDbfs > -139.0f ? QVariant(m_txMicPeakMaxDbfs) : QVariant());
    // The configured target, read from the modulator rather than a default
    // Config. Applies to every path: the ALC only reduces, with its ceiling at
    // unity.
    put("alcTargetPeak",
        QStringLiteral("ALC target peak (linear, all paths)"),
        m_alcTargetPeak);
    put("alcGainDb", QStringLiteral("ALC gain applied (dB)"),
        std::isnan(m_alcGainDb) ? QVariant() : QVariant(m_alcGainDb));
    put("alcPeakDbfs", QStringLiteral("Post-ALC peak (dBFS)"),
        std::isnan(m_alcPeakDbfs) ? QVariant() : QVariant(m_alcPeakDbfs));
    // The passband the modulator is running RIGHT NOW, which on any mode other
    // than SSB voice is NOT the operator's stored pair — effectiveTxPassband()
    // deliberately declines to apply a voice setting to CW or the digital modes.
    // Reporting the stored pair here would explain nothing on exactly the modes
    // where the two disagree.
    {
        const Receiver* txRx = rx(m_txDdc);
        const QString txMode = txRx ? txRx->mode : QStringLiteral("USB");
        const auto [lo, hi] = effectiveTxPassband(txMode);
        put("txPassbandHz", QStringLiteral("TX passband in use (Hz)"),
            QStringLiteral("%1 .. %2").arg(lo).arg(hi));
        put("txPassbandFromOperator", QStringLiteral("TX passband is an operator override"),
            m_txFilterFromOperator);
    }

    section("temperatureC", QStringLiteral("Analog / thermal"));
    // `inBandLive`, like every row around it. m_paTempC is a SMOOTHED value
    // with no age of its own: publishTelemetry() sets m_havePaTemp and nothing
    // ever clears it, so without this gate the one row a human actually reads
    // would keep showing a figure from a stream that stopped minutes ago while
    // temperatureRaw beside it correctly reported nothing and yielded to the
    // poller. The telemetrySource row two sections down already reasons this
    // way — "we hold one forever" — and this row was the exception
    // (aethersdr-agent, #5642 review).
    put("temperatureC", QStringLiteral("PA temperature (°C)"),
        (inBandLive && m_havePaTemp) ? QVariant(m_paTempC) : QVariant());
    put("temperatureRaw", QStringLiteral("Temperature (raw counts)"),
        opt(t.temperatureRaw));
    put("biasCurrentRaw", QStringLiteral("PA bias current (raw counts)"),
        opt(t.biasCurrentRaw));

    section("forwardPowerRaw", QStringLiteral("Directional coupler (uncalibrated)"));
    put("forwardPowerRaw", QStringLiteral("Forward (raw counts)"),
        opt(t.forwardPowerRaw));
    put("forwardPowerW", QStringLiteral("Forward (W, approx — instantaneous)"),
        t.forwardPowerRaw
            ? QVariant(directionalWatts(*t.forwardPowerRaw)) : QVariant());
    // The held value driving FWDPWR, beside its instantaneous source: a wide
    // gap on SSB shows envelope peaks being missed; held above instantaneous
    // on TUNE would mean the release inflates. Gated on the same optional so
    // the pair is absent together. Display hold only (#4912): assert on
    // forwardPowerW, not this.
    put("forwardPowerPeakW",
        QStringLiteral("Forward (W, approx — peak HOLD, display only)"),
        t.forwardPowerRaw ? QVariant(m_fwdPeakWatts) : QVariant());
    put("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),
        opt(t.reversePowerRaw));
    put("reversePowerW", QStringLiteral("Reverse (W, approx)"),
        t.reversePowerRaw
            ? QVariant(directionalWatts(*t.reversePowerRaw)) : QVariant());
    // SWR from linearized counts (#4578), absent below kMinForwardCountsForSwr
    // like the TX:SWR meter, where the ratio would be noise.
    {
        QVariant swr;
        if (t.forwardPowerRaw && t.reversePowerRaw
            && *t.forwardPowerRaw >= kMinForwardCountsForSwr) {
            if (const auto v = swrFromRaw(*t.forwardPowerRaw,
                                          *t.reversePowerRaw))
                swr = *v;
        }
        put("swr", QStringLiteral("SWR"), swr);
    }

    // Telemetry attribution. Published here and winning the merge over
    // Hl2TelemetryService's same key, so both inputs must be asked here
    // (the service is injected for that), or `port-1025` would be
    // unreachable whenever a backend exists. Decided by the shared
    // hl2TelemetrySource() policy.
    section("telemetrySource", QStringLiteral("Telemetry source"));
    put("telemetrySource", QStringLiteral("Source"),
        hl2TelemetrySource(m_connected,
                           // Not "we hold a temperature reading" — we hold one
                           // forever. Whether the in-band path is DELIVERING.
                           /*haveInBand=*/inBandLive && t.temperatureRaw.has_value(),
                           /*haveStreamFree=*/m_telemetryService
                               && m_telemetryService->lastReply().has_value()));

    section("bandFilter", QStringLiteral("Front end"));
    put("bandFilter", QStringLiteral("J16 filter byte"),
        (m_ocFilterByte >= 0 && m_ocFilterByte <= 0x7F)
            ? QVariant(QString::asprintf("0x%02X — %s", m_ocFilterByte,
                       ocFilterName(static_cast<std::uint8_t>(m_ocFilterByte))))
            : QVariant());
    // Per receiver, because "the slice frequency" stops being a single value.
    // When the receivers span bands the filter byte above reads as a bypass, and
    // these are the numbers that explain why.
    for (const auto& ids : m_ids.all()) {
        const Receiver* r = rx(ids.ddcIndex);
        if (!r)
            continue;
        const QString suffix = m_ids.size() > 1
                                   ? QStringLiteral(" (RX%1)").arg(ids.uiNumber + 1)
                                   : QString();
        put(QStringLiteral("rxFrequencyHz%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("Slice frequency (Hz)") + suffix,
            static_cast<qulonglong>(r->sliceFreqHz < 0 ? 0 : r->sliceFreqHz));
        put(QStringLiteral("ncoHz%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("DDC / pan centre (Hz)") + suffix,
            static_cast<qulonglong>(r->ncoHz < 0 ? 0 : r->ncoHz));
    }

    section("sampleRateHz", QStringLiteral("Link"));
    put("receivers", QStringLiteral("Active receivers"), m_ids.size());
    put("sampleRateHz", QStringLiteral("IQ sample rate (Hz)"), m_sampleRateHz);
    // The link budget this configuration actually consumes. Dropped packets
    // below are the symptom; this is the cause, and having both side by side is
    // what turns "the audio sounds wrong" into a diagnosis (oracle §6).
    put("ep6MbitPerSec", QStringLiteral("EP6 wire rate (Mbit/s)"),
        QString::number(ep6BitsPerSecond(m_sampleRateHz, m_ids.empty() ? 1 : m_ids.size())
                            / 1.0e6, 'f', 1));
    // Cumulative EP6 sequence gaps. The oracle is blunt that UDP loss on a
    // marginal link is the most common cause of "the audio sounds wrong", so
    // this is the first number to look at when it does.
    put("droppedPackets", QStringLiteral("Dropped EP6 packets"),
        static_cast<qulonglong>(m_drops));
    // Silence-watchdog recovery record: MetisClient re-sends the run command on
    // EP6 silence before declaring the link down, and a recovery that works is
    // visible nowhere else. Read together: attempts climbing while completions
    // do not means the re-send is not fixing the real fault.
    put("silenceRecoveryAttempts", QStringLiteral("EP6 silence recoveries attempted"),
        static_cast<qulonglong>(m_silenceRecoveryAttempts));
    put("silenceRecoveriesCompleted", QStringLiteral("EP6 silence recoveries completed"),
        static_cast<qulonglong>(m_silenceRecoveriesCompleted));
    // Partial FFT windows discarded at discontinuities, including accepted
    // rewinds and duplicates. Poll each DSP's atomic like the ADC peak rows;
    // its lifetime is protected by the GUI-owned receiver list.
    for (const auto& ids : m_ids.all()) {
        const Receiver* r = rx(ids.ddcIndex);
        // Guarded on `!r` alone, like the sibling loops: a receiver with no DSP
        // chain is one between rebuilds, and that is "not reported" rather than
        // a zero that would read as "the panadapter is clean".
        if (!r)
            continue;
        const QString suffix = m_ids.size() > 1
                                   ? QStringLiteral(" (RX%1)").arg(ids.uiNumber + 1)
                                   : QString();
        put(QStringLiteral("spectrumGapDiscards%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("Panadapter frames discarded at a sequence gap") + suffix,
            r->dsp ? QVariant(static_cast<qulonglong>(r->dsp->spectrumGapDiscards()))
                   : QVariant());
    }
    // The wideband bandscope. Reported unconditionally rather than only when it
    // is on, because "off" is the answer the reader of a health dialog needs
    // first — an absent row would leave "is this costing me link budget?"
    // unanswered rather than answered "no".
    put("bandscopeEnabled", QStringLiteral("Wideband bandscope (EP4)"),
        m_bandscopeEnabled);
    put("ep4Packets", QStringLiteral("Bandscope packets"),
        static_cast<qulonglong>(m_ep4Packets));
    put("ep4Drops", QStringLiteral("Dropped EP4 packets"),
        static_cast<qulonglong>(m_ep4Drops));
    // Kept apart from the drops above, not folded in. Exactly one rewind is
    // expected per stream start — the gateware re-aligns ep4_seq_no's low two
    // bits while the capture FIFO fills — and none after it. Inside a counter
    // that is supposed to read zero, a second one would be invisible.
    put("ep4Rewinds", QStringLiteral("EP4 sequence rewinds"),
        static_cast<qulonglong>(m_ep4Rewinds));
    // Bandscope gate health. Accepted blocks != ep4Packets/4 (most packets are
    // armed, flushed or trailing). Non-zero timeouts do not by themselves mean
    // the radio ignored the run byte: check bandscopeGuardMs's assumptions first
    // (sample-clocked arming delay; EP4 rate at 2 and 4+ receivers unmeasured).
    put("bandscopeBlocks", QStringLiteral("Bandscope blocks accepted"),
        static_cast<qulonglong>(m_ep4Blocks));
    put("bandscopeTimeouts", QStringLiteral("Bandscope block timeouts"),
        static_cast<qulonglong>(m_ep4Timeouts));

    // Headroom rows: always put(), with an invalid variant until a block has
    // arrived, so rows keep their place and read "not reported" (0.00 dBFS
    // would look like a clip). Uncalibrated, pre-DDC AD9866 levels, comparable
    // only with the gateware's clip/good-level flags. Display only.
    //
    // Per-receiver WDSP rows: blocks the DSP refused (WdspChannel::ProcessResult).
    // Underruns are separate because they are normal (fexchange2 returns -2 when
    // the async output has nothing ready). Machine form: Hl2RxDsp::processTally().
    bool dspSectionOpen = false;
    for (const auto& ids : m_ids.all()) {
        const Receiver* r = rx(ids.ddcIndex);
        // Heading attached to the FIRST key actually emitted, not to a
        // guessed "dspBlocks0": with no receivers this loop emits nothing,
        // and a heading keyed to a row that was never put() is a section
        // title with no section under it.
        if (!dspSectionOpen) {
            section(QStringLiteral("dspBlocks%1").arg(ids.uiNumber).toUtf8().constData(),
                    QStringLiteral("Receive DSP"));
            dspSectionOpen = true;
        }
        const QString suffix = m_ids.size() > 1
                                   ? QStringLiteral(" (RX%1)").arg(ids.uiNumber + 1)
                                   : QString();
        const WdspProcessTally::Counts tally =
            r && r->dsp ? r->dsp->processTally() : WdspProcessTally::Counts{};
        const bool seen = tally.blocks() > 0;
        put(QStringLiteral("dspBlocks%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("DSP blocks processed") + suffix,
            seen ? QVariant(static_cast<qulonglong>(tally.blocks())) : QVariant());
        put(QStringLiteral("dspUnderruns%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("DSP pipeline underruns (normal)") + suffix,
            seen ? QVariant(static_cast<qulonglong>(tally.underrun)) : QVariant());
        // ONE ROW, NAMING THE KINDS, rather than four rows of mostly zeros.
        // Which kind it is changes the diagnosis completely -- an allocation
        // on the real-time path is a code defect, a `Busy` is a lost control
        // race, an engine error is WDSP refusing the data -- so the kinds
        // cannot be summed away; but three of the four are zero on every
        // radio that has ever worked, and four permanently-zero rows per
        // receiver is how a dialog stops being read.
        QString faults;
        if (tally.faults() == 0) {
            faults = QStringLiteral("none");
        } else {
            QStringList parts;
            if (tally.allocationViolation)
                parts << QStringLiteral("allocation on the real-time path %1")
                             .arg(tally.allocationViolation);
            if (tally.engineError)
                parts << QStringLiteral("WDSP engine error %1").arg(tally.engineError);
            if (tally.invalidBuffer)
                parts << QStringLiteral("invalid buffer geometry %1")
                             .arg(tally.invalidBuffer);
            if (tally.busy)
                parts << QStringLiteral("busy %1").arg(tally.busy);
            faults = QStringLiteral("%1 - %2")
                         .arg(tally.faults())
                         .arg(parts.join(QStringLiteral(", ")));
        }
        put(QStringLiteral("dspProcessFaults%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("DSP processing faults") + suffix,
            seen ? QVariant(faults) : QVariant());
        // The fault total as a number, so the bridge's `health` verb can
        // threshold it without parsing the prose row above. One row, not one
        // per kind.
        put(QStringLiteral("dspFaultCount%1").arg(ids.uiNumber).toUtf8().constData(),
            QStringLiteral("DSP processing faults (count)") + suffix,
            seen ? QVariant(static_cast<qulonglong>(tally.faults())) : QVariant());
    }

    // WITH the clip flag, not with the link counters. These are the AD9866's
    // own scale and their entire justification is that they are commensurable
    // with adcOverload, which sits under "Converter" — reporting them under
    // "Link", where the last section marker left them, put the two at opposite
    // ends of the dialog. (PR #5650 review round 3.)
    section("adcPeakDbfs", QStringLiteral("Converter"));
    // `haveObservation`: a block has ever arrived. `haveBlock`: the newest is
    // still current (kHeadroomMaxAgeMs, shared with the auto-gain loop). Level
    // rows go absent once the EP4 stream stops, including during transmit
    // (MetisClient::bandscopeInterlocked()); adcObservedAgoMs says which silence.
    const std::int64_t blockAgeMs = bandscopeBlockAgeMs();
    const bool haveObservation = m_bandscopeBlock.samples > 0;
    const bool haveBlock =
        AetherSDR::hl2::bandscopeBlockIsCurrent(m_bandscopeBlock, blockAgeMs);
    const double peak = haveBlock ? m_bandscopeBlock.peakDbfs() : 0.0;
    const double rms  = haveBlock ? m_bandscopeBlock.rmsDbfs()  : 0.0;
    auto dbfs = [haveBlock](double v) {
        return haveBlock ? QVariant(QString::number(v, 'f', 2)) : QVariant();
    };
    put("adcPeakDbfs", QStringLiteral("ADC peak (uncalibrated pre-DDC dBFS)"),
        dbfs(peak));
    // AC: the deviation about the block's own mean, not about zero, so a
    // converter DC offset is not counted as signal. The row is labelled for it
    // because the two numbers above and below it are absolute and this one is
    // not — a reader comparing adcPeakDbfs with adcRmsDbfs is comparing two
    // different references, and the label is the only place that says so.
    // (#5802.)
    put("adcRmsDbfs", QStringLiteral("ADC RMS, AC (uncalibrated pre-DDC dBFS)"),
        dbfs(rms));
    // The mean the RMS row removed, on the same scale and gate as its
    // neighbours (#5856). Named for where it is measured, not for a cause. A
    // non-zero mean under half a code reports below kEp4FloorDbfs, as
    // adcRmsDbfs does (see Ep4Stats::dcDbfs()).
    put("adcDcDbfs", QStringLiteral("ADC DC level (uncalibrated pre-DDC dBFS)"),
        dbfs(haveBlock ? m_bandscopeBlock.dcDbfs() : 0.0));
    // The same mean, signed, in raw codes: the sign says which rail it sits
    // toward, and it separates a zero mean from a half-code one.
    put("adcDcCodes", QStringLiteral("ADC DC level (signed codes)"),
        haveBlock ? QVariant(QString::number(m_bandscopeBlock.meanCodes(), 'f', 2))
                  : QVariant());
    // Crest factor is scale-free (the calibration offset cancels): ~11-12 dB
    // for a broadband floor over 2048 samples, ~3 dB for a sinusoid. With AC
    // RMS and absolute peak, a large crest can also mean a large DC level;
    // adcDcDbfs above says which (#5856). Not reported when either term is at
    // the floor sentinel (Ep4Stats::crestDb()); the row stays in place.
    const std::optional<double> crest =
        haveBlock ? m_bandscopeBlock.crestDb() : std::nullopt;
    put("adcCrestDb", QStringLiteral("ADC crest factor (dB)"),
        crest ? QVariant(QString::number(*crest, 'f', 2)) : QVariant());
    // Counted with ad9866.v's OWN two thresholds — rxclipp at +2047 and
    // rxclipn at -2048 — and not a symmetric |code| >= 2048, which can
    // never fire on a positive clip because +2048 is not a code a 12-bit
    // two's-complement converter can produce.
    put("adcClippedPerBlock", QStringLiteral("ADC samples at the rail (per 2048)"),
        haveBlock ? QVariant(static_cast<qulonglong>(m_bandscopeBlock.clippedSamples))
                  : QVariant());
    // How old the reading is. A gated sensor's number is a snapshot, and a
    // snapshot with no age on it invites being read as current.
    //
    // GATED ON haveObservation, NOT ON haveBlock, and that is the whole reason
    // the two names exist. This row is what EXPLAINS the six above going
    // absent: an operator who sees six dashes and an age of 46 810 ms knows
    // the gate stopped, where six dashes and a seventh dash says only that
    // something is missing. Expiring the age along with the values would
    // delete the evidence for the expiry.
    put("adcObservedAgoMs", QStringLiteral("ADC level observed (ms ago)"),
        (haveObservation && m_bandscopeBlockClock.isValid())
            ? QVariant(static_cast<qulonglong>(blockAgeMs))
            : QVariant());
    return h;
}


// ─── RFC #4603 PR 3: the client is this radio's memory ───────────────────────

void Hl2Backend::applyRestoredState(const RestoredRadioState& state)
{
    // Validation boundary: the document is operator data from disk. Each field
    // is range-checked once and dropped on failure, never fixed up. Restoring
    // never keys transmit; everything below is a setpoint.
    // Full reset first: applyRestoredState({}) means "no memory for this
    // radio", and RadioModel calls this on every connect so a same-family swap
    // cannot leak radio A's state into radio B.
    m_restoredState = RestoredRadioState{};
    m_haveRestoredState = false;
    m_lnaDbByBand.clear();
    m_driveByBand.clear();
    m_lnaGainDb = hl2::kLnaDefaultGainDb;
    // The automatic offset is session state and a radio swap ends the session:
    // radio B must not come up attenuated by a decision taken about radio A's
    // antenna. (Hl2GainSplit.h)
    m_lnaAutoOffsetDb = 0;
    // A radio swap ends the session, and an automatic control armed about radio
    // A's antenna has nothing to say about radio B's.
    m_autoRfGainEnabled = false;
    // Nor does a refusal composed about radio A's baseline: the interface
    // promises an empty reason from a backend that has not been asked, and
    // radio B has not been. Left standing, any reader other than the toggle
    // lambda would surface radio A's number as radio B's.
    m_autoRfGainRefusal.clear();
    m_autoGainState = AetherSDR::hl2::AutoGainState{};
    // The same call the constructor makes. Neither the law nor the floor is
    // persisted: both are the constructed default after every connect.
    installDefaultAutoGainLaw();
    m_autoGainReason = AetherSDR::hl2::AutoGainReason::Disarmed;
    m_autoGainBandKey.clear();
    m_autoGainBaselineDb = 0;
    m_autoGainSampleRateHz = 0;
    m_sinceUnkey.invalidate();
    // The clip totals are a per-session denominator and must not carry radio
    // A's observations into radio B's rate.
    m_adcWindowSamples = 0;
    m_adcOverloadWindowSamples = 0;
    m_adcWindowMs = 0;
    m_adcTotalSamples = 0;
    m_adcTotalOverloadSamples = 0;
    m_adcWindowClock.invalidate();
    m_lnaSessionPin = false;
    m_driveDefaultPercent = -1;
    m_rfPowerPercent = 100;       // TransmitModel's session default
    m_sampleRateHz = 48000;       // construction default — radio B must not
                                  // inherit radio A's span (PR #4619 review)
    m_currentBandKey.clear();
    // Same rule for the TX passband: a same-family swap must not carry radio A's
    // eSSB cuts onto radio B. Back to virgin defaults, which for this pair means
    // "the operator has chosen nothing" — so effectiveTxPassband() resumes
    // deriving from the mode until either a restore or the operator says
    // otherwise.
    m_txFilterFromOperator = false;
    m_txFilterLowHz = 300;
    m_txFilterHighHz = 2700;
    // Everything the voice chain OBSERVED goes back to "never reported". These
    // rows answer "what was the chain doing on that over?", and carrying radio
    // A's last ALC figures and held power under radio B's identity is worse
    // than a blank row: it answers a question about this radio with a confident
    // number measured on a different one.
    m_alcGainDb = std::numeric_limits<double>::quiet_NaN();
    m_alcPeakDbfs = std::numeric_limits<double>::quiet_NaN();
    m_fwdPeakWatts = 0.0;
    m_txMicPeakMaxDbfs = -140.0f;
    // The STAGED restore is reset, even though m_micLevel below is not: it
    // names radio A's document and this call may be radio B arriving with no
    // memory. Clearing it is what makes applyRestoredState({}) mean "this radio
    // has nothing stored" for the mic level too.
    m_restoredMicLevel = -1;
    // m_micLevel and m_appliedMicGainLinear are not reset: they mirror the
    // host-side modulator, which a same-family swap does not rebuild. A swap
    // that does rebuild gets a fresh pair via RadioModel::setupBackend().

    RestoredRadioState valid;
    if (state.rfFrequencyHz >= 100'000.0 && state.rfFrequencyHz <= 38'400'000.0)
        valid.rfFrequencyHz = state.rfFrequencyHz;
    // Accept, then canonicalise to the spelling the menu offers (e.g. NFM ->
    // FM), so the mode combo can find the restored mode. Alias pairs share one
    // WdspChannel mode and the same receive-only status (pinned by
    // hl2_mode_vocabulary_test). Must precede the passband work below, which
    // reads valid.mode.
    if (isKnownModeString(state.mode))
        valid.mode = canonicalOfferedMode(state.mode);
    // A passband is kept only as a sane pair; mode+passband are applied
    // together in pushInitialState() (the #4484 reconciliation).
    if (state.filterLowHz < state.filterHighHz
        && state.filterLowHz >= -12'000.0 && state.filterHighHz <= 12'000.0)
    {
        valid.filterLowHz = state.filterLowHz;
        valid.filterHighHz = state.filterHighHz;
    }
    // Drop pre-#4914 CW passbands. CW filter edges are now carrier-relative
    // ({-250, 250}) rather than audio-relative ({350, 850} at 600 Hz pitch);
    // replaying an old pair adds the BFO twice and silences the marker. Every
    // producer builds a CW passband that contains the carrier, so a pair wholly
    // on one side of zero is an old document. pushInitialState() derives the
    // default and the next capture heals the document.
    if (cwBfoOffsetHz(valid.mode, m_cwPitchHz) != 0.0
        && !(valid.filterLowHz < 0.0 && valid.filterHighHz > 0.0))
    {
        const auto [lo, hi] = defaultPassbandForMode(valid.mode);
        qCInfo(lcHl2) << "HL2: dropping pre-#4914 CW passband"
                      << valid.filterLowHz << ".." << valid.filterHighHz
                      << "for" << valid.mode << "-> mode default" << lo << ".." << hi;
        valid.filterLowHz  = static_cast<double>(lo);
        valid.filterHighHz = static_cast<double>(hi);
    }
    if (state.sampleRateHz > 0)
        valid.sampleRateHz = nearestIqSampleRateHz(state.sampleRateHz);
    // AGC: mode and threshold are validated INDEPENDENTLY, unlike the passband
    // pair. They are two separate controls whose values do not constrain each
    // other — a threshold of 40 means the same thing under "slow" as under
    // "fast" — so a document with one bad field has no reason to lose the good
    // one. The threshold's bound is SliceModel's own 0..100, and a value
    // outside it is DROPPED rather than clamped: clamping would invent a
    // setpoint the operator never chose and then persist it back.
    if (isKnownAgcModeString(state.agcMode))
        valid.agcMode = state.agcMode.trimmed().toLower();
    if (state.agcThreshold >= 0 && state.agcThreshold <= 100)
        valid.agcThreshold = state.agcThreshold;

    // Per-band maps ride the typed extension's domain sub-objects
    // (RestoredRadioState.h). Values clamp to the hardware's own ranges.
    const QJsonObject rfGain =
        state.extension.value(QStringLiteral("rfGain")).toObject();
    // rfGain.defaultDb is not read (#5829): nothing can set it, so a stored
    // value would freeze first-visit gain forever. Unvisited bands use
    // hl2::kLnaDefaultGainDb. The capture drops the key, and ignoring it is
    // logged with both values so a support log traces back to this.
    if (rfGain.contains(QStringLiteral("defaultDb"))) {
        qCInfo(lcHl2) << "HL2: ignoring stale restored LNA default (#5829)"
                      << rfGain.value(QStringLiteral("defaultDb")).toVariant()
                      << "— unvisited bands come up on"
                      << hl2::kLnaDefaultGainDb << "dB";
    }

    // Records the wish only; the connect edge arms it once the restored
    // baseline is applied. Absent means off: default-on for a profile that
    // never expressed a wish is a separate decision (#5535). A stored
    // `autoEnabled: true` arms, including one saved while arming was refused.
    m_autoRfGainWanted =
        rfGain.value(QStringLiteral("autoEnabled")).toBool(false);
    const QJsonObject lnaByBand =
        rfGain.value(QStringLiteral("lnaDbByBand")).toObject();
    for (auto it = lnaByBand.constBegin(); it != lnaByBand.constEnd(); ++it)
        m_lnaDbByBand.insert(it.key(),
                             qBound(kLnaGainMinDb, it.value().toInt(),
                                    kLnaGainMaxDb));

    const QJsonObject txSetpoints =
        state.extension.value(QStringLiteral("txSetpoints")).toObject();
    if (txSetpoints.contains(QStringLiteral("defaultPercent")))
        m_driveDefaultPercent = qBound(
            0, txSetpoints.value(QStringLiteral("defaultPercent")).toInt(), 100);
    const QJsonObject driveByBand =
        txSetpoints.value(QStringLiteral("driveByBand")).toObject();
    for (auto it = driveByBand.constBegin(); it != driveByBand.constEnd(); ++it)
        m_driveByBand.insert(it.key(), qBound(0, it.value().toInt(), 100));

    // Mic level is dropped, not clamped, when out of range: the floor is mute,
    // so a clamped bad value would silently take the operator off the air.
    // Leaving m_restoredMicLevel at -1 lets setupBackend()'s live slider stand.
    // contains() first: toInt() returns 0 (mute) for a missing key.
    if (txSetpoints.contains(QStringLiteral("micLevel"))) {
        const QJsonValue raw = txSetpoints.value(QStringLiteral("micLevel"));
        const int level = raw.toInt(-1);
        if (raw.isDouble() && level >= 0 && level <= 100) {
            m_restoredMicLevel = migrateRestoredMicLevel(level, txSetpoints);
        } else {
            qCInfo(lcHl2) << "HL2: dropping invalid restored mic level"
                          << raw.toVariant();
        }
    }

    // TX passband: both keys required and validated as a pair, else dropped
    // whole (m_txFilterFromOperator stays false). Bounds match setTxFilter():
    // 12 kHz ceiling from 24 kHz TX audio, edges at least 50 Hz apart.
    if (txSetpoints.contains(QStringLiteral("filterLowHz"))
        && txSetpoints.contains(QStringLiteral("filterHighHz")))
    {
        const int lowHz = txSetpoints.value(QStringLiteral("filterLowHz")).toInt();
        const int highHz = txSetpoints.value(QStringLiteral("filterHighHz")).toInt();
        if (lowHz >= 0 && highHz <= kTxAudioMaxHz && lowHz + 50 <= highHz) {
            m_txFilterLowHz = lowHz;
            m_txFilterHighHz = highHz;
            m_txFilterFromOperator = true;
        } else {
            qCWarning(lcHl2) << "HL2 restore: dropping out-of-range TX passband"
                             << lowHz << ".." << highHz;
        }
    }

    m_restoredState = valid;
    m_haveRestoredState = true;
    // Capture-side AGC pair only, reset so a same-family swap cannot write
    // radio A's AGC under radio B. Receivers are not touched: this runs before
    // every connect including reconnects, and live per-receiver AGC must
    // survive a dropped link. Receiver seeding is in connectRadio(), which
    // knows the serial (#4909).
    const Receiver defaults;   // the constructed med/65, named once
    m_agcMode = m_restoredState.agcMode.isEmpty() ? defaults.agcMode
                                                  : m_restoredState.agcMode;
    m_agcThresholdDb = m_restoredState.agcThreshold >= 0
                           ? m_restoredState.agcThreshold
                           : defaults.agcThresholdDb;
    qCInfo(lcHl2) << "HL2 restore: freq" << valid.rfFrequencyHz << "mode"
                  << valid.mode << "filter" << valid.filterLowHz << ".."
                  << valid.filterHighHz << "rate" << valid.sampleRateHz
                  << "agc" << valid.agcMode << valid.agcThreshold
                  << "lna bands" << m_lnaDbByBand.size() << "drive bands"
                  << m_driveByBand.size();
}

// Seeds every receiver's AGC pair (unlike mode/passband, which are per-slice):
// AGC is captured as one flat setting. With no memory the defaults are written
// rather than skipped, since buildReceivers() carries state across rebuilds.
// Called only from connectRadio() when the radio identity changed or receivers
// were rebuilt, never on a reconnect. Mode and threshold apply independently.
// Seeds the struct only; beginDspSetup()/pushInitialState() push it to the DSP.
void Hl2Backend::seedReceiverAgc()
{
    const Receiver defaults;   // the constructed med/65, named once
    const bool haveMode =
        m_haveRestoredState && !m_restoredState.agcMode.isEmpty();
    const bool haveThreshold =
        m_haveRestoredState && m_restoredState.agcThreshold >= 0;
    for (Receiver& r : m_rx) {
        r.agcMode = haveMode ? m_restoredState.agcMode : defaults.agcMode;
        r.agcThresholdDb = haveThreshold ? m_restoredState.agcThreshold
                                         : defaults.agcThresholdDb;
    }
    // Prime the remembered pair from what was just seeded, so a capture taken
    // before the operator touches the control records the restored value rather
    // than falling back through an empty member.
    if (!m_rx.empty()) {
        m_agcMode = m_rx.front().agcMode;
        m_agcThresholdDb = m_rx.front().agcThresholdDb;
    }
}

RestoredRadioState Hl2Backend::currentOperatingState() const
{
    RestoredRadioState state;
    if (const Receiver* txRx = rx(m_txDdc)) {
        state.rfFrequencyHz = txRx->sliceFreqHz;
        state.mode = txRx->mode;
        state.filterLowHz = txRx->filterLowHz;
        state.filterHighHz = txRx->filterHighHz;
    }
    // The AGC pair, from the LAST RECEIVER THE OPERATOR TOUCHED rather than
    // from the transmit one — see setSliceAgc(). FLAT, not per-band: unlike
    // drive and LNA — where the right value is a property of the band — an
    // operator's AGC is a property of how they like to listen, and making it
    // jump on a band change would be the same surprise the TX passband comment
    // above rejects. Falls back to the transmit receiver before the operator
    // has set anything this session, so a capture taken on a fresh connect
    // still records a real value rather than a default.
    if (!m_agcMode.isEmpty()) {
        state.agcMode = m_agcMode;
        state.agcThreshold = m_agcThresholdDb;
    } else if (const Receiver* txRx = rx(m_txDdc)) {
        state.agcMode = txRx->agcMode;
        state.agcThreshold = txRx->agcThresholdDb;
    }
    state.sampleRateHz = m_sampleRateHz;

    // The maps plus the live values under the current band key — so a capture
    // between band changes still records the operator's latest tweaks.
    QJsonObject lnaByBand;
    for (auto it = m_lnaDbByBand.constBegin(); it != m_lnaDbByBand.constEnd(); ++it)
        lnaByBand.insert(it.key(), it.value());
    QJsonObject driveByBand;
    for (auto it = m_driveByBand.constBegin(); it != m_driveByBand.constEnd(); ++it)
        driveByBand.insert(it.key(), it.value());
    if (!m_currentBandKey.isEmpty()) {
        // Same preservation rule as rememberCurrentBandState(): this capture
        // runs on any debounced store, so it must not persist the session pin
        // either (#5402).
        lnaByBand.insert(
            m_currentBandKey,
            AetherSDR::hl2::bandMemoryWriteback(
                m_lnaGainDb, m_lnaSessionPin,
                m_lnaDbByBand.contains(m_currentBandKey),
                m_lnaDbByBand.value(m_currentBandKey)));
        driveByBand.insert(m_currentBandKey, m_rfPowerPercent);
    }

    // The auto-gain switch is persisted here, not in AppSettings (docs/HERMES.md:
    // values the radio cannot store go in OperatingState). Only the switch: the
    // loop's offset is transient (see m_lnaAutoOffsetDb). No "defaultDb" key
    // (#5829); see applyRestoredState().
    QJsonObject rfGain{{QStringLiteral("lnaDbByBand"), lnaByBand},
                       // The wish (m_autoRfGainWanted), not the running flag:
                       // they differ when arming was declined, and persisting
                       // false then would stop it ever arming again.
                       {QStringLiteral("autoEnabled"), m_autoRfGainWanted}};
    QJsonObject txSetpoints{{QStringLiteral("driveByBand"), driveByBand}};
    if (m_driveDefaultPercent >= 0)
        txSetpoints.insert(QStringLiteral("defaultPercent"), m_driveDefaultPercent);

    // The TX passband is flat, not per band or mode: it is one pair of
    // sliders, and per-band memory would move them on their own. Written only
    // once the operator has chosen; persisting the mode default would look like
    // an override and suppress the per-mode derivation.
    if (m_txFilterFromOperator) {
        txSetpoints.insert(QStringLiteral("filterLowHz"), m_txFilterLowHz);
        txSetpoints.insert(QStringLiteral("filterHighHz"), m_txFilterHighHz);
    }

    // Mic level, flat: it depends on voice and mic, not band. The HL2 has no
    // mic-gain register, so the client is the only memory. Radios that store
    // it themselves don't declare ClientSettingsDomain::TxSetpoints, so this
    // extension field never reaches them. Unconditional: 0 (mute) must
    // round-trip; see the -1 sentinel on m_restoredMicLevel.
    txSetpoints.insert(QStringLiteral("micLevel"), m_micLevel);
    // The curve that level is a position ON, so a document written by one build
    // is not silently re-interpreted by another. Absent means curve 1, the
    // +20 dB upper leg this radio shipped with before the ALC's makeup gain was
    // removed; see migrateRestoredMicLevel(). Written unconditionally beside the
    // level, because a level without its curve is the ambiguity this key exists
    // to end — and written as a NUMBER rather than a build string, so the next
    // change to the mapping is a comparison rather than a table of versions.
    txSetpoints.insert(QStringLiteral("micLevelCurve"), kMicLevelCurve);
    state.extension = QJsonObject{{QStringLiteral("rfGain"), rfGain},
                                  {QStringLiteral("txSetpoints"), txSetpoints}};
    state.extensionSchemaVersion = 1;
    return state;
}

// The one true LNA BASELINE application: it sets the operator's number and
// re-derives what the wire carries. Shared by the operator path (setPanRfGain)
// and the band-memory path so the two can't drift (PR #4619 review).
//
// This is deliberately the ONLY writer of m_lnaGainDb outside the connect seed,
// and it is deliberately not what an automatic control calls — see
// setLnaAutoOffsetDb() and Hl2GainSplit.h for why an automatic writer on this
// path destroys the per-band memory and the persisted operating state.
void Hl2Backend::applyLnaGainDb(int gainDb)
{
    m_lnaGainDb = gainDb;
    pushEffectiveLnaGain();
}

int Hl2Backend::lnaEffectiveDb() const noexcept
{
    return AetherSDR::hl2::effectiveLnaGain(m_lnaGainDb, m_lnaAutoOffsetDb,
                                            kLnaGainMinDb, kLnaGainMaxDb)
        .effectiveDb;
}

// The automatic attenuation below the operator's baseline. NON-NEGATIVE by
// construction: this axis has no representation for a gain above the number the
// operator set, so no automatic control built on it can make the radio louder
// than they asked (Hl2GainSplit.h).
//
// It does NOT touch m_lnaGainDb, m_lnaDbByBand, m_lnaSessionPin or
// notifyOperatingStateChanged(). That is the whole point: nothing here is the
// operator's intent, so nothing here may be persisted as if it were.
void Hl2Backend::setLnaAutoOffsetDb(int offsetDb)
{
    const int requested = offsetDb < 0 ? 0 : offsetDb;
    if (requested == m_lnaAutoOffsetDb)
        return;
    m_lnaAutoOffsetDb = requested;
    pushEffectiveLnaGain();
}

// Register write, dB-reference lockstep and every-pan echo all use the
// effective value, not the baseline: spectrum and S-meter render through
// m_dbRef, so the reference must move with every gain change, manual or
// automatic, or the trace slides.
// Arm or disarm the automatic control. Radio-wide: there is one AD9866.
void Hl2Backend::setAutoRfGain(bool on)
{
    // A declined arm is a third state (wanted true, enabled false), so the
    // guard checks both flags; an "off" from there must reach the disarm
    // branch, the only writer of wanted = false (#5828). Every disarm side
    // effect is inert from the refused state. on/enabled/!wanted is
    // unreachable: enabled = true is always followed by wanted = true.
    if (on == m_autoRfGainEnabled && on == m_autoRfGainWanted) {
        return;
    }
    if (on) {
        // REFUSED, NOT CLAMPED. See kAutoRfGainMaxBaselineDb. Moving the
        // operator's own number so the feature could be switched on would be a
        // UI reporting one value while the wire carried another.
        // Defensive: every writer of m_lnaGainDb clamps to the native range,
        // so no baseline above the ceiling exists today.
        if (m_lnaGainDb > kAutoRfGainMaxBaselineDb) {
            // The request survives the refusal and is persisted, so the next
            // connect from a trusted baseline arms without asking twice.
            m_autoRfGainWanted = true;
            // Kept for the GUI (#5817): shown on the panadapter and read by
            // screen readers, so translated and without an issue number.
            m_autoRfGainRefusal = tr(
                       "Auto RF gain declined — the RF Gain baseline is "
                       "%1 dB, above this radio's %2 dB maximum. Lower RF "
                       "Gain to %2 dB or below and try again. "
                       "Your setting has not been changed.")
                       .arg(m_lnaGainDb)
                       .arg(kAutoRfGainMaxBaselineDb);
            qWarning().noquote()
                << QStringLiteral("Hl2Backend: ") + m_autoRfGainRefusal;
            // SETTLED AS NOT ARMED, and said so. A refusal that only the
            // caller's own readback could discover was invisible on the two
            // routes that have no readback: the restore below and the bridge.
            emit autoRfGainArmSettled(false);
            // The surviving wish is persisted state, so republish. After
            // autoRfGainArmSettled to match the other branches. This writes on
            // every declined ask, bounded today by connects, clicks and bridge
            // verbs and debounced by scheduleOperatingStateSave(); a periodic
            // re-arm would need its own guard.
            notifyOperatingStateChanged();
            return;
        }
        // CLEARED ON SUCCESS. A reason that outlived the refusal it describes
        // would be shown against a later, unrelated failure.
        m_autoRfGainRefusal.clear();
        m_autoGainState = AetherSDR::hl2::AutoGainState{};
        m_autoGainBandKey = m_currentBandKey;
        m_autoGainBaselineDb = m_lnaGainDb;
        m_autoGainSampleRateHz = m_sampleRateHz;
        m_autoGainReason = AetherSDR::hl2::AutoGainReason::Warmup;
        // Not keyed since arming. A stale unkey stamp from earlier in the
        // session would hold the loop off for no reason, or -- worse -- fail to.
        m_sinceUnkey.invalidate();
        m_autoRfGainEnabled = true;
        m_autoRfGainWanted = true;
        // A law that releases on a measurement needs the stream that carries
        // it. Ordered AFTER m_autoRfGainEnabled, which is what it reads.
        applyBandscopeForAutoGain();
        qCInfo(lcHl2) << "HL2 auto RF gain: ARMED at baseline" << m_lnaGainDb
                      << "dB, floor" << m_autoGainConfig.maxOffsetDb << "dB below";
        emit autoRfGainArmSettled(true);
    } else {
        // WAS IT ACTUALLY RUNNING. Read before the flag is cleared, and used
        // only for the log: the two states that reach here with it false are a
        // decline still standing, and the window inside a connect after
        // applyRestoredState() has restored autoEnabled:true and before the
        // linkUp handler has tried to arm on it. Neither has a baseline to
        // restore, and the old sentence claimed one for both.
        const bool wasRunning = m_autoRfGainEnabled;
        m_autoRfGainEnabled = false;
        // The operator turning it OFF is a preference, and is persisted as one.
        // A REFUSAL ITSELF still does not reach here -- the arm branch returns
        // before this -- so a radio that declined to arm keeps the operator's
        // "on" recorded and arms on the next connect that allows it. What DOES
        // reach here is the operator's later, explicit withdrawal of that "on",
        // which is a different event and is what #5828 was about.
        m_autoRfGainWanted = false;
        // Cleared here because an off that took is a successful attempt, and
        // IAutoRfGainControl defines lastArmRefusalReason() as empty after one;
        // the bridge reply and MainWindow's accessible description both read it.
        // Must precede the emit: onAutoRfGainArmSettled reads it on entry.
        m_autoRfGainRefusal.clear();
        applyBandscopeForAutoGain();
        m_autoGainReason = AetherSDR::hl2::AutoGainReason::Disarmed;
        m_autoGainState = AetherSDR::hl2::AutoGainState{};
        // ONE ACTION, from any state. A switch that left the radio attenuated
        // after being turned off would be a control that does not undo itself.
        setLnaAutoOffsetDb(0);
        if (wasRunning) {
            qCInfo(lcHl2) << "HL2 auto RF gain: disarmed, baseline" << m_lnaGainDb
                          << "dB restored";
        } else {
            qCInfo(lcHl2) << "HL2 auto RF gain: request withdrawn; the loop was "
                             "not running, so there is no baseline to restore";
        }
        emit autoRfGainArmSettled(false);
    }
    // BOTH BRANCHES ABOVE MOVED m_autoRfGainWanted, which currentOperatingState()
    // publishes. Reached only past the opening guard, so an idempotent call -- the
    // operator re-asserting a switch that is already where they want it -- still
    // announces nothing.
    notifyOperatingStateChanged();
}

// The operator's floor. Applied live: pulling it in while the loop is holding
// more than the new floor makes the policy surrender the excess on its next
// evaluation, in one step rather than at the release rate, because that is a
// bound being enforced rather than the loop deciding to release.
void Hl2Backend::setAutoRfGainFloorDb(int floorDb)
{
    const int clamped = floorDb < 0 ? 0
                      : (floorDb > kAutoRfGainFloorMaxDb ? kAutoRfGainFloorMaxDb
                                                         : floorDb);
    if (clamped == m_autoGainConfig.maxOffsetDb) {
        return;
    }
    m_autoGainConfig.maxOffsetDb = clamped;
    qCInfo(lcHl2) << "HL2 auto RF gain: floor set to" << clamped
                  << "dB below the operator's baseline";
}

// The law a backend starts with, and the only place that says so: the
// constructor, applyRestoredState() and the name "default" all come here. Not a
// constant: the bias budget is computed from the gate period MetisClient
// actually runs, never from a literal, so the two cannot drift apart.
Hl2Backend::AutoGainLaw Hl2Backend::defaultAutoGainLaw()
{
    using namespace AetherSDR::hl2;
    return {QStringLiteral("bandscope"),
            bandscopeReleaseConfig(
                gatedPeakBiasDbForPeriod(MetisClient::bandscopeSamplePeriodMs()))};
}

// The name and the numbers together, so that no caller can install one without
// the other. Touches nothing else: no state, no gate. The constructor runs it
// before m_metis exists.
void Hl2Backend::installDefaultAutoGainLaw()
{
    const AutoGainLaw law = defaultAutoGainLaw();
    m_autoGainConfig = law.config;
    m_autoGainMode = law.name;
}

// Which of Hl2AutoGainPolicy.h's configurations the loop runs. Applied live:
// the state is NOT reset, because the offset the radio is actually holding is
// real whichever law asked for it, and the policy's own ceiling check gives
// back any excess on its next evaluation, in one step.
bool Hl2Backend::setAutoRfGainMode(const QString& mode)
{
    using namespace AetherSDR::hl2;
    const QString m = mode.trimmed().toLower();
    AutoGainLaw law;
    if (m == QLatin1String("bandscope") || m == QLatin1String("default")) {
        // The one law here whose release rests on a measurement. "default" is
        // whatever defaultAutoGainLaw() returns, not a second name for one law.
        law = defaultAutoGainLaw();
    } else if (m == QLatin1String("ramp")) {
        law = {QStringLiteral("ramp"), AutoGainConfig{}};
    } else if (m == QLatin1String("probe") || m == QLatin1String("probing")) {
        law = {QStringLiteral("probe"), probingReleaseConfig()};
    } else if (m == QLatin1String("binary")) {
        law = {QStringLiteral("binary"), binaryHighLowConfig()};
    } else {
        qWarning().noquote()
            << QStringLiteral("Hl2Backend: auto RF gain mode \"%1\" is not one of "
                              "bandscope|ramp|probe|binary. The law has not been "
                              "changed.")
                   .arg(mode);
        return false;
    }
    m_autoGainConfig = law.config;
    m_autoGainMode = law.name;
    // A law that needs the wideband reading needs the stream that carries it.
    // Called unconditionally so that switching AWAY from bandscope mode also
    // releases the gate, rather than leaving it running for a law that ignores
    // it.
    applyBandscopeForAutoGain();
    qCInfo(lcHl2) << "HL2 auto RF gain: law set to" << m_autoGainMode
                  << "- step" << m_autoGainConfig.attackStepDb
                  << "dB, floor" << m_autoGainConfig.maxOffsetDb
                  << "dB, probe confirm" << m_autoGainConfig.probeConfirmMs << "ms"
                  << ", headroom required" << m_autoGainConfig.requireHeadroomToRelease;
    return true;
}

// Arms the bandscope gate that a law with requireHeadroomToRelease needs
// (without it the offset never releases and reports HeadroomAbsent).
// Only releases a gate this function started (m_bandscopeOwnedByAutoGain), so
// an operator's manual `bandscope.enable` is never stomped.
void Hl2Backend::applyBandscopeForAutoGain()
{
    const bool wanted = m_connected && m_autoRfGainEnabled
                     && m_autoGainConfig.requireHeadroomToRelease;
    // m_bandscopeEnabled IS READ HERE AND NEVER WRITTEN. Since #5650 it mirrors
    // LinkCounters::bandscopeEnabled — the gate as MetisClient actually has it,
    // not the request this backend made. That makes it the honest answer to
    // "is it already running", which is the only question asked of it below;
    // writing it would put a request where the health row expects an
    // observation, which is exactly what #5650's review removed.
    if (wanted) {
        if (m_bandscopeEnabled) {
            // Already running. If the operator started it, it stays theirs and
            // the loop simply reads what is there; claiming it here would mean
            // disarming the loop later switched off a stream we never started.
            return;
        }
        m_bandscopeOwnedByAutoGain = true;
    } else {
        if (!m_bandscopeOwnedByAutoGain) {
            return;             // never ours, so never ours to release
        }
        m_bandscopeOwnedByAutoGain = false;
    }
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setBandscopeEnabled",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, wanted));
    }
    qCInfo(lcHl2) << "HL2 auto RF gain: wideband bandscope"
                  << (wanted ? "enabled" : "released")
                  << "for the measured release";
}

// One evaluation of the control law, on the telemetry publish that carried the
// observation. Every decision is in Hl2AutoGainPolicy.h; this function only
// gathers the inputs, applies the instruction, and owns the clocks.
void Hl2Backend::stepAutoGain(const Hl2Telemetry& t)
{
    using namespace AetherSDR::hl2;
    if (!m_autoRfGainEnabled) {
        return;
    }
    AutoGainObservation obs;
    obs.samples = t.adcSamples;
    obs.overloadSamples = t.adcOverloadSamples;
    // The window length is an INPUT, not an assumption. The publish interval is
    // a floor rather than a period -- a late I/O thread makes it longer -- and a
    // cooldown measured in windows rather than milliseconds would drift with it.
    obs.elapsedMs = t.adcWindowMs > 0 ? t.adcWindowMs : 0;
    // Both keying sources. The application's own m_keyed covers the instant
    // before the radio has echoed anything back; the radio's PTT bit covers a
    // key this application did not originate.
    obs.keyed = m_keyed || t.ptt;
    obs.msSinceUnkey = m_sinceUnkey.isValid() ? m_sinceUnkey.elapsed() : -1;
    // How much attenuation physically exists below the operator's baseline. The
    // policy does not know the register geometry and must not.
    obs.availableOffsetDb = m_lnaGainDb - kLnaGainMinDb;
    obs.bandChanged = (m_currentBandKey != m_autoGainBandKey);
    obs.baselineMoved = (m_lnaGainDb != m_autoGainBaselineDb);
    // The denominator changed, so the old windows are not comparable.
    obs.resetWarmup = obs.bandChanged || obs.baselineMoved
                   || (m_sampleRateHz != m_autoGainSampleRateHz);
    m_autoGainBandKey = m_currentBandKey;
    m_autoGainBaselineDb = m_lnaGainDb;
    m_autoGainSampleRateHz = m_sampleRateHz;

    // Wideband headroom from the newest bandscope block. Classified here, not in
    // the policy, because age is a clock reading and Hl2AutoGainPolicy.h owns no
    // clock. "Never observed" (samples == 0 or negative age) and "too old" are
    // both Absent, never a headroom of zero.
    obs.headroom = bandscopeHeadroom(m_bandscopeBlock, bandscopeBlockAgeMs());

    const AutoGainAction a = autoGainStep(m_autoGainState, obs, m_autoGainConfig);
    m_autoGainState = a.next;
    m_autoGainReason = a.reason;
    publishFrontEndOverload();
    if (a.warnFloorOnce) {
        qWarning().noquote()
            << QStringLiteral(
                   "Hl2Backend: auto RF gain is at its floor (%1 dB below your "
                   "setting) and the converter is STILL clipping. No amount of LNA "
                   "will fix this — the front end needs attenuation ahead of the "
                   "radio, or a band-pass filter for whatever is outside the "
                   "passband.")
                   .arg(m_autoGainState.offsetDb);
    }
    if (a.deltaDb != 0) {
        setLnaAutoOffsetDb(m_autoGainState.offsetDb);
        qCDebug(lcHl2) << "HL2 auto RF gain:" << (a.deltaDb > 0 ? "attack" : "release")
                       << a.deltaDb << "dB -> offset" << m_autoGainState.offsetDb
                       << "dB, window" << obs.overloadSamples << "/" << obs.samples
                       << ", headroom"
                       << (obs.headroom.isMeasurement()
                               ? QString::number(obs.headroom.headroomDb, 'f', 1)
                               : QStringLiteral("absent"));
    }
}

void Hl2Backend::pushEffectiveLnaGain()
{
    const int effective = lnaEffectiveDb();
    m_dbRef.setLnaGainDb(effective);
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setLnaGainDb", Qt::QueuedConnection,
            Q_ARG(int, effective));
    // The AGC ceiling moves in lockstep with the LNA: WDSP's max gain applies
    // post-LNA, so Hl2DbReference::agcCeilingDb() refers the operator's
    // antenna-referred setting. The operator's 0..100 is not touched, so no
    // slice change is emitted. Queued per receiver because the DSP lives on the
    // I/O thread; receivers without DSP pick it up when their Config is built.
    for (const auto& r : m_rx) {
        if (!r.dsp)
            continue;
        QMetaObject::invokeMethod(r.dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgcMode(r.agcMode)),
            Q_ARG(double, m_dbRef.agcCeilingDb(r.agcThresholdDb)));
    }
    // Echo what the hardware actually took, to every pan — a slider that
    // asked for something outside the register's range finds out here, and so
    // does an operator whose gain is being held down by an automatic control.
    for (const auto& ids : m_ids.all())
        emit panRfGainChanged(ids.panId, effective);
}

void Hl2Backend::rememberCurrentBandState()
{
    if (m_currentBandKey.isEmpty())
        return;
    m_lnaDbByBand.insert(
        m_currentBandKey,
        AetherSDR::hl2::bandMemoryWriteback(
            m_lnaGainDb, m_lnaSessionPin,
            m_lnaDbByBand.contains(m_currentBandKey),
            m_lnaDbByBand.value(m_currentBandKey)));
    m_driveByBand.insert(m_currentBandKey, m_rfPowerPercent);
}

void Hl2Backend::applyPerBandStateFor(double freqHz, const char* reason)
{
    const QString newBand = hl2::bandKeyForHz(freqHz);
    if (newBand == m_currentBandKey) {
        return;
    }
    // Leaving a band records the operator's values under the OLD key; the new
    // band gets what it remembered (or the defaults). nigelfenton's RFC
    // review: the drive that makes 5 W on 80 m is not polite on 10 m, so a
    // band change must never carry the old band's drive along.
    rememberCurrentBandState();
    // Clear only AFTER writeback preserves the start band. Later bands must
    // record their own gains normally.
    m_lnaSessionPin = false;
    const QString oldBand = m_currentBandKey;
    m_currentBandKey = newBand;

    // A band with no entry of its own comes up on the SHIPPED default, never on
    // a persisted one (#5829): the document key that used to sit here could not
    // be written by anything and so could never be corrected either.
    const int lna = qBound(kLnaGainMinDb,
                           m_lnaDbByBand.value(newBand, hl2::kLnaDefaultGainDb),
                           kLnaGainMaxDb);
    if (lna != m_lnaGainDb)
        applyLnaGainDb(lna);

    // NEVER inherit the previous band's drive (nigelfenton's RFC rationale:
    // the drive that makes 5 W on 80 m is amplifier-input-unsafe on 10 m).
    // Remembered value first, then the operator's baseline default, and — for
    // a band never visited before any baseline exists — a deliberate 0:
    // conservative once per band per radio, instead of hot once per mistake.
    int drive = m_driveByBand.value(newBand, m_driveDefaultPercent);
    if (drive < 0) {
        drive = 0;
        qCInfo(lcHl2) << "HL2 band memory: first visit to" << newBand
                      << "with no drive baseline — drive set to 0 until the"
                         " operator chooses one";
    }
    if (drive != m_rfPowerPercent) {
        // A drive SETPOINT — applyDrive() itself stays behind the TX gate, so
        // a transmit-blocked session records the value without touching the PA.
        m_applyingBandMemory = true;
        setTxPower(drive);
        m_applyingBandMemory = false;
        TransmitDelta delta;
        delta.rfPower = drive;
        emit transmitChanged(delta);
    }

    qCInfo(lcHl2) << "HL2 band memory (" << reason << "):" << oldBand << "->"
                  << newBand << "lna" << m_lnaGainDb << "dB drive"
                  << m_rfPowerPercent << '%';
    notifyOperatingStateChanged();
}

void Hl2Backend::notifyOperatingStateChanged()
{
    emit operatingStateChanged();
}

void Hl2Backend::pushInitialState()
{
    // The HL2 has no readable state: every register keeps whatever the last
    // session wrote. Anything the radio cannot be asked for must be asserted
    // here, or the UI shows something the hardware is not doing (e.g. the TX
    // NCO must follow the TX receiver, not the previous session's frequency).
    if (const Receiver* txRx = rx(m_txDdc))
        setTxFrequency(txRx->sliceFreqHz);

    // NOT the drive level. connectRadio() already asserts a safe 0 before the
    // link comes up, and by the time this runs RadioModel has pushed the
    // operator's actual RF power — emit connected() above is synchronous, so
    // resetting here silently undid it and the radio transmitted at drive 0
    // with the PA disabled. Caught by measurement: forward power went to 0.

    // Derive each receiver's passband from its mode (#4484): the 150..3000
    // defaults match no mode, so connect would leave USB with DIGU's passband.
    // Outside the dsp guard because these are the backend's own published
    // values. Every receiver, since each has its own mode. Once per connect,
    // not per linkUp: MetisClient re-emits linkUp after EP6 silence, and
    // re-deriving then would discard an operator's filter edit.
    if (!m_passbandDerivedThisConnect) {
        for (Receiver& r : m_rx) {
            const auto [pbLowHz, pbHighHz] = defaultPassbandForMode(r.mode);
            r.filterLowHz = pbLowHz;
            r.filterHighHz = pbHighHz;
        }
        // RFC #4603 PR 3, reconciled with #4484 (Ozy311's review catch): a
        // RESTORED mode+passband overrides the derivation — but only as a
        // pair. Restoring the mode alone re-derives its passband, and a
        // restored passband applies on top of its restored mode, so mode and
        // passband can never disagree — the invariant #4484 exists for. Runs
        // under the same once-per-connect guard, so an EP6 glitch's re-linkUp
        // cannot re-assert day-old state over the operator's live edits.
        if (m_haveRestoredState) {
            if (Receiver* txRx = rx(m_txDdc)) {
                if (!m_restoredState.mode.isEmpty()) {
                    txRx->mode = m_restoredState.mode;
                    const auto [pbLowHz, pbHighHz] =
                        defaultPassbandForMode(txRx->mode);
                    txRx->filterLowHz = pbLowHz;
                    txRx->filterHighHz = pbHighHz;
                }
                if (m_restoredState.filterLowHz != 0.0
                    || m_restoredState.filterHighHz != 0.0) {
                    txRx->filterLowHz =
                        static_cast<int>(m_restoredState.filterLowHz);
                    txRx->filterHighHz =
                        static_cast<int>(m_restoredState.filterHighHz);
                }
            }
            // The start band's remembered drive, as a SETPOINT (never keys —
            // applyDrive() stays behind the TX gate). Echoed upward as a
            // normalized delta so TransmitModel and the UI agree with the
            // register. After RadioModel's own connect-time power push, so
            // the remembered per-band value wins over the session default.
            const int drive =
                m_driveByBand.value(m_currentBandKey, m_driveDefaultPercent);
            if (drive >= 0) {
                m_applyingBandMemory = true;
                setTxPower(drive);
                m_applyingBandMemory = false;
                TransmitDelta delta;
                delta.rfPower = drive;
                emit transmitChanged(delta);
            }
        }
        m_passbandDerivedThisConnect = true;
    }

    for (Receiver& r : m_rx) {
        if (!r.dsp)
            continue;
        const auto [dspLo, dspHi] = dspFilterHz(r);
        QMetaObject::invokeMethod(r.dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(r.mode)));
        QMetaObject::invokeMethod(r.dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, dspLo), Q_ARG(double, dspHi));
        // Restoring straight into CW gets its BFO here. addReceiver() already
        // set a shift, but from the mode the receiver was CONSTRUCTED with —
        // and the restored mode arrives after that.
        QMetaObject::invokeMethod(r.dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(r)));
        // The AGC, for the same reason as the three above: the channel was
        // opened by buildReceivers() BEFORE the restore ran, so it is sitting
        // on Config's med/65 whatever the receiver now says. Pushing r's own
        // live values (not m_restoredState) makes this idempotent — a
        // mid-session linkUp after an EP6 glitch re-asserts what the operator
        // currently has rather than replaying day-old state over their edits,
        // which is the rule the passband derivation guard exists to enforce.
        QMetaObject::invokeMethod(r.dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgcMode(r.agcMode)),
            Q_ARG(double, m_dbRef.agcCeilingDb(r.agcThresholdDb)));
        // Unmute, and stamp the gate for the same reason the two setters do:
        // this is a third site that asks a muted chain to start sampling again.
        // Harmless when nothing was muted — only the false->true edge moves it.
        m_sliceSampling.setRequested(true, hl2::steadyNowNs());
        QMetaObject::invokeMethod(r.dsp, "setAudioMuted", Qt::QueuedConnection,
            Q_ARG(bool, false));
        // The notch axis, which is measured from the NCO and defaults to ZERO.
        // Miss this and a notch is placed ~10 MHz outside the passband, where
        // WDSP finds no notch to apply and simply builds an unnotched filter —
        // no error, no notch, and a `notch list` that reports it as present.
        // createPanadapter() seeded the receivers it creates; the ones built at
        // connect need it too, which is the whole set on a normal session.
        seedNotches(r);
        // Same reasoning for the blanker: a fresh Hl2RxDsp opens with it off,
        // so a reconnect into a session that had it on would leave the slice's
        // NB button lit over a chain that is not blanking anything.
        pushNoiseBlanker(r);
        // And the panadapter averaging, for the same reason.
        pushPanAveraging(r);
        pushSquelch(r);
    }
    if (m_txDsp) {
        const Receiver* txRx = rx(m_txDdc);
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode,
                  modeFromString(txRx ? txRx->mode : QStringLiteral("USB"))));
        QMetaObject::invokeMethod(m_txDsp, "reset", Qt::QueuedConnection);
    }

    // This radio's remembered mic level, staged by applyRestoredState() for one
    // connect-time application. Applied here because m_txDsp is built by
    // connectRadio(), and after RadioModel::setupBackend()'s slider re-assert.
    // Sentinel -1 = nothing stored; the seam's value stands. transmitChanged
    // echoes it so the slider matches the modulator.
    if (m_restoredMicLevel >= 0) {
        // Consume before publishing anything. MetisClient may emit linkUp again
        // after transient EP6 silence without a new connectRadio(); replaying
        // this disk value then would overwrite the operator's newer live move.
        const int restoredMicLevel = m_restoredMicLevel;
        m_restoredMicLevel = -1;
        setMicGain(restoredMicLevel);
        TransmitDelta delta;
        delta.micLevel = restoredMicLevel;
        emit transmitChanged(delta);
        qCInfo(lcHl2) << "HL2: restored mic level" << restoredMicLevel;
    }
    // Pan zoom limits = the DDC rates this radio can run; nothing above the
    // seam can derive them (the FlexLib model table would allow 5.4 MHz). The
    // upper limit falls as receivers are added because span and receiver count
    // share the 100BASE-T budget, matching what applyPanBandwidth() accepts.
    const int running = m_ids.empty() ? 1 : m_ids.size();
    int widestHz = kIqSampleRatesHz[0];
    for (const int r : kIqSampleRatesHz) {
        if (r <= maxIqSampleRateHz() && maxReceiversAtRate(r, running) >= running)
            widestHz = std::max(widestHz, r);
    }
    for (const auto& ids : m_ids.all()) {
        emit panBandwidthLimitsChanged(
            ids.panId,
            static_cast<double>(kIqSampleRatesHz[0]) / 1.0e6,
            static_cast<double>(widestHz) / 1.0e6);
    }

    // What the RF Gain slider is allowed to ask for, and where it currently
    // sits. Pushed here for the same reason as everything else in this
    // function: the model's default is Flex's -8..+32 in 8 dB steps, learned
    // from a "display pan rfgain_info" command that answers nothing on this
    // radio, so without this the slider would misrepresent both the range and
    // the resolution of the AD9866's LNA.
    //
    // To every pan, because the LNA is radio-wide: each pane's slider must show
    // the same range and the same value, since they all drive one register.
    for (const auto& ids : m_ids.all()) {
        emit panRfGainInfoChanged(ids.panId,
                                  kLnaGainMinDb, kLnaGainMaxDb, kLnaGainStepDb);
        emit panRfGainChanged(ids.panId, lnaEffectiveDb());
    }

    // Keying state is ours, not the radio's: a reconnect must never come up
    // keyed because the previous session ended mid-transmission.
    m_keyed = false;
    m_tuning = false;
    // BELT AND BRACES, and named as such rather than dressed up: the bit lives
    // in MetisClient, which clears it in start(), so on the path that matters
    // this is redundant. It is here because this function is the one place that
    // states what a fresh transport starts as, and a reader checking whether
    // the tune request is part of that should find it listed with the rest.
    applyAtuTuneRequest(false);
    if (m_lastTxOperation.permitsCleanup()) {
        setKeying(false, m_lastTxOperation);
    }
    // Release the receive-audio hold too, AFTER the cleanup unkey above so the
    // unmute never posts ahead of a queued MOX-off (#5497). Unconditional
    // because the setKeying(false) above is conditional and nothing else would
    // reopen the gate; the T/R turnaround it covered belongs to the old link.
    if (m_unkeyUnmuteTimer) {
        m_unkeyUnmuteTimer->stop();
    }
    applyRxAudioMute(false);
    // A fresh transport starts unkeyed by construction; an old connection's
    // stop must not be queued into it with a newly acquired operation.
}

void Hl2Backend::defineMeters()
{
    // Indices are ours to choose — nothing on the HL2 assigns meter ids, unlike
    // Flex where they come from the radio's meter manifest. They only have to be
    // stable and unique within this backend.
    auto def = [this](int index, const QString& source, const QString& name,
                      const QString& unit, double low, double high,
                      const QString& desc) {
        MeterDef d;
        d.index = index;
        d.source = source;
        d.name = name;
        d.unit = unit;
        d.low = low;
        d.high = high;
        d.description = desc;
        emit meterDefined(d);
    };

    def(1, QStringLiteral("SLC"), QStringLiteral("LEVEL"),   QStringLiteral("dBm"),
        -140.0, 0.0,   QStringLiteral("Receive signal level"));
    // The two directional meters are labelled uncalibrated in their own
    // descriptions, which is where the honesty has to live: the VALUE looks
    // exactly like a calibrated one, so nothing about the number itself warns
    // the operator. See directionalWatts().
    //
    // The upper bound is 41 dBm rather than Flex's 50: that is ~12.6 W, a
    // little above the top of the reference curve. A 50 dBm (100 W) scale would
    // leave every real HL2 reading in the bottom tenth of the meter.
    def(2, QStringLiteral("TX"),  QStringLiteral("FWDPWR"),  QStringLiteral("dBm"),
        0.0, 41.0,     QStringLiteral("Forward power, peak estimate (uncalibrated)"));
    def(3, QStringLiteral("TX"),  QStringLiteral("REFPWR"),  QStringLiteral("dBm"),
        0.0, 41.0,     QStringLiteral("Reflected power (uncalibrated)"));
    def(4, QStringLiteral("TX"),  QStringLiteral("SWR"),     QStringLiteral("SWR"),
        1.0, 10.0,     QStringLiteral("Standing wave ratio"));
    def(5, QStringLiteral("RAD"), QStringLiteral("PATEMP"),  QStringLiteral("degC"),
        0.0, 100.0,    QStringLiteral("PA temperature"));
    def(6, QStringLiteral("TX"),  QStringLiteral("MICPEAK"), QStringLiteral("dBFS"),
        -100.0, 0.0,   QStringLiteral("Microphone peak"));
    // The post-ALC transmit level. Named ALC because that is the name MeterModel
    // binds to its swAlc() accessor, which is what the Phone/CW applet's ALC
    // gauges read — the meter is defined by what consumes it, not by which stage
    // happens to produce it.
    def(7, QStringLiteral("TX"),  QStringLiteral("ALC"),     QStringLiteral("dBFS"),
        -100.0, 0.0,   QStringLiteral("Post-ALC transmit peak"));
    // Speech-processor gain reduction, as a POSITIVE amount of compression in
    // dB — the sign convention MeterModel's COMPPEAK path already expects, and
    // the opposite of ClientComp::gainReductionDb()'s own (which is <= 0).
    //
    // sourceIndex is left at its default 0, which is below MeterModel's
    // kMinTxWaveformSourceIndex of 8, so this lands in the by-slice map under
    // the implicit slice rather than the explicit TX-waveform map. That is the
    // right bucket for a radio with one transmitter: the Flex form of this meter
    // is per-waveform-slice, and there is no such thing here.
    def(8, QStringLiteral("TX"),  QStringLiteral("COMPPEAK"), QStringLiteral("dB"),
        0.0, 25.0,     QStringLiteral("Speech processor compression"));
    // ALC gain, the companion to meter 7 (post-ALC level): how hard the stage
    // is working. Top is 0 dB because the ALC only reduces. Bottom -20 dB is a
    // presentation floor (reduction is unbounded; worst recorded is -21.41 dB
    // on a full-scale block). sourceIndex 0 as for COMPPEAK: one transmitter.
    def(9, QStringLiteral("TX"),  QStringLiteral("ALCGAIN"), QStringLiteral("dB"),
        -20.0, 0.0,    QStringLiteral("Gain the ALC is applying"));
}

void Hl2Backend::publishTelemetry(const Hl2Telemetry& t)
{
    // Fwd/rev power are uncalibrated ADC counts, so they are never published
    // as FWDPWR dBm. SWR is a ratio of linearized readings (swrFromRaw() via
    // detectorVolts(), #4578), and only meaningful with real forward power:
    // below kMinForwardCountsForSwr (MetisProtocol.h, shared with Radio Health)
    // noise drives the ratio to 255.99:1.
    if (t.forwardPowerRaw && t.reversePowerRaw
        && *t.forwardPowerRaw >= kMinForwardCountsForSwr) {
        if (const auto swr = swrFromRaw(*t.forwardPowerRaw, *t.reversePowerRaw))
            emit meterUpdate(QStringLiteral("TX:SWR"), *swr);
    }
    // Forward/reverse power through directionalWatts()'s uncalibrated reference
    // curve; raw counts are logged too, for future per-unit calibration.
    // MetisClient already paces telemetry at 10 Hz (kTelemetryMinIntervalMs).
    // Published through the peak hold; see kFwdPeakReleaseAlpha.
    if (t.forwardPowerRaw) {
        const double instantW = directionalWatts(*t.forwardPowerRaw);
        // The hold applies only while keyed. Unkeyed, the reading must fall to
        // zero on the same schedule REFPWR does — MeterModel snaps its own
        // forward-power filter to zero the moment a no-carrier sample arrives,
        // and a hold that outlived the transmission would keep re-arming it,
        // leaving the gauge claiming power out of a radio that has stopped.
        m_fwdPeakWatts = fwdPeakHoldStep(m_fwdPeakWatts, instantW, m_keyed,
                                         kFwdPeakReleaseAlpha);
        emit meterUpdate(QStringLiteral("TX:FWDPWR"), wattsToDbm(m_fwdPeakWatts));
    }
    if (t.reversePowerRaw)
        emit meterUpdate(QStringLiteral("TX:REFPWR"),
                         wattsToDbm(directionalWatts(*t.reversePowerRaw)));
    if (t.forwardPowerRaw && (*t.forwardPowerRaw != m_lastFwdRaw)) {
        m_lastFwdRaw = *t.forwardPowerRaw;
        qCDebug(lcHl2Tx) << "HL2 directional: fwd" << *t.forwardPowerRaw
                         << "rev" << t.reversePowerRaw.value_or(-1)
                         << "-> fwd" << directionalWatts(*t.forwardPowerRaw) << "W"
                         << "(uncalibrated reference curve)";
    }
    // The radio's TX IQ FIFO: `fill` is the top 7 bits of the gateware's DSIQ
    // level (0-127, not a sample count); `pacingFault` is the one flag for both
    // underrun and blocked writes. It cannot see a dry client queue
    // (MetisClient::m_txIq): EP2 frames are still full-size, zero-filled. That
    // fault has its own counters (MetisClient::txUnderflowPackets,
    // ::txUnderflowSamples, ::txOverflowSamples).
    if (m_keyed && t.txFifoFillMsbs)
        qCDebug(lcHl2Tx) << "HL2 fifo: fill" << *t.txFifoFillMsbs << "/127"
                         << "pacingFault" << t.txFifoRecovery.value_or(false);
    if (t.temperatureRaw) {
        const double c = temperatureCelsius(*t.temperatureRaw);
        // The instrumentation ADC's low bits are noisy enough that the displayed
        // temperature flickered by a degree with the radio sitting idle. A
        // single pole settles it; heating and cooling are both slow, so unlike
        // the S-meter this one has no reason to attack faster than it decays.
        m_paTempC = m_havePaTemp ? (kPaTempAlpha * c + (1.0 - kPaTempAlpha) * m_paTempC)
                                 : c;
        m_havePaTemp = true;
        emit meterUpdate(QStringLiteral("RAD:PATEMP"), m_paTempC);
    }

    m_telemetry = t;
    // THE RATE, ACCUMULATED RATHER THAN SAMPLED. The edge counter below sees
    // this flag at 10 Hz; these two came off every EP6 frame in MetisClient's
    // receive loop, which is the only place the ~190 Hz observation still
    // exists. Nothing here drives anything -- they are published and that is
    // all -- but they are what an operator needs to watch the input on a live
    // antenna before deciding whether an automatic loop is worth arming.
    m_adcWindowSamples = t.adcSamples;
    m_adcOverloadWindowSamples = t.adcOverloadSamples;
    m_adcWindowMs = t.adcWindowMs;
    publishFrontEndOverload();
    if (t.adcSamples > 0) {
        m_adcTotalSamples += static_cast<quint64>(t.adcSamples);
        m_adcTotalOverloadSamples += static_cast<quint64>(t.adcOverloadSamples);
        // Restarted only on a window that CARRIED observations. A telemetry
        // update with an empty denominator is not evidence that the converter
        // was looked at, so it must not refresh the age of the last look.
        m_adcWindowClock.start();
    }
    // THE CONTROL LAW, on the tick that carried the observation. Deliberately
    // above the warning below rather than beside it: the warning reports what
    // was seen, the loop acts on it, and an operator reading the log should see
    // the action attributed to the window that caused it.
    stepAutoGain(t);
    if (t.adcOverload && *t.adcOverload != m_adcOverload) {
        m_adcOverload = *t.adcOverload;
        if (m_adcOverload)
            ++m_adcOverloadAssertions;
    }
    // Rate-limited, not just edge-gated: the comparator chatters on a strong
    // band, so nearly every sample is an edge. Outside the edge test so a burst
    // that stops still reports its tally on time. The count is reported because
    // the rate is the severity.
    const AetherSDR::hl2::AdcOverloadWarn w = AetherSDR::hl2::adcOverloadWarn(
        m_adcOverloadAssertions,
        m_adcOverloadClock.isValid(),
        m_adcOverloadClock.isValid() ? m_adcOverloadClock.elapsed() : 0,
        kAdcOverloadWarnIntervalMs);
    if (w.warn) {
        // What the aggregate branch does NOT mean. It is not "this is the first
        // overload ever" — it is "exactly one assertion was seen in this
        // window". That lone assertion may have arrived at any point since the
        // window opened, so a bare message can lag the event by up to
        // kAdcOverloadWarnIntervalMs. Accepted deliberately: it is the cost of
        // the rate limit, one assertion is a hint rather than an emergency, and
        // an isolated overload after a quiet period still reports immediately
        // because the clock is long expired by then.
        if (w.aggregate) {
            // noquote + one composed string: streaming "(" as its own item makes
            // QDebug insert a space after it and print "( 51 times in 10000 ms)".
            qWarning().noquote()
                << "Hl2Backend: ADC OVERLOAD — reduce LNA gain or attenuate"
                << QStringLiteral("(%1 times in %2 ms)")
                       .arg(w.count)
                       .arg(m_adcOverloadClock.elapsed());
        } else {
            qWarning() << "Hl2Backend: ADC OVERLOAD — reduce LNA gain or attenuate";
        }
        if (w.restartClock) {
            // start(), NOT restart(). restart() reads the elapsed time first,
            // and reading it on a timer that was never started is undefined —
            // which is exactly the first-assertion path, where the clock is
            // invalid by construction. start() is defined on both, and the
            // value restart() returns was discarded anyway. (#5381 review.)
            m_adcOverloadClock.start();
        }
        m_adcOverloadAssertions = 0;
    }
}

double Hl2Backend::wattsToDbm(double watts)
{
    // The meter seam carries dBm (MeterDef unit), and MeterModel converts back
    // to watts for display. Floored at the meter's own low bound so 0 W becomes
    // "nothing" rather than -inf, which would propagate as NaN through the
    // widget's scaling.
    constexpr double kFloorDbm = 0.0;   // 1 mW; matches MeterDef low
    if (!(watts > 0.0))
        return kFloorDbm;
    const double dbm = 10.0 * std::log10(watts * 1000.0);
    return dbm < kFloorDbm ? kFloorDbm : dbm;
}

double Hl2Backend::temperatureCelsius(int raw)
{
    // MOVED, NOT COPIED. The formula is in MetisProtocol.h beside the decode
    // that produces `raw`, because the stream-free poller needs the same
    // conversion and shares no other header with this class. This stays as a
    // forwarder so the existing callers and the tests that pin them keep
    // naming one function rather than a re-typed twin.
    return hl2TemperatureCelsius(raw);
}

void Hl2Backend::applyIoBoardFrequency()
{
    if (!m_metis || m_rx.empty())
        return;

    // The TRANSMIT receiver's frequency — NOT the agree-or-bypass answer the
    // filter board gets. The IO board switches amplifiers, antenna relays and
    // transverters, all of which must follow where the operator will RADIATE.
    // Receive slices parked on other bands are irrelevant to that, and the
    // bypass result (kOcNone) is a relay pattern with no frequency to offer.
    const Receiver* txRx = rx(m_txDdc);
    const double hz = txRx ? txRx->sliceFreqHz : m_rx[0].sliceFreqHz;
    if (!std::isfinite(hz) || hz <= 0.0 || hz > static_cast<double>(0xFF'FF'FF'FF'FFULL)) {
        return;                 // validate before converting to the 40-bit field
    }

    // sliceFreqHz is TRUE-RF and the board's field wants true RF: it compares
    // against band edges to pick a relay. The frequency-calibration scaling in
    // ncoCommandHz() exists to correct the HL2's own reference and belongs only
    // on values going to an NCO register — applying it here would hand the
    // board a slightly wrong frequency for no reason.
    const auto target = static_cast<quint64>(hz + 0.5);

    // The band, from the same bandKeyForHz() table the per-band memory uses, so
    // "which band is this" has exactly one answer in this backend.
    const QString targetBand = bandKeyForHz(hz);
    const bool bandChanged = (targetBand != m_ioBoardBandKey);

    if (!m_ioBoardThrottle) {
        m_ioBoardThrottle = new QTimer(this);
        m_ioBoardThrottle->setSingleShot(true);
        m_ioBoardThrottle->setInterval(kIoBoardThrottleMs);
        connect(m_ioBoardThrottle, &QTimer::timeout, this, [this] {
            const quint64 pending = m_ioBoardSchedule.takePending();
            if (pending == 0) {
                return;                  // cooldown expired with nothing waiting
            }
            if (!sendIoBoardFrequency(pending))
                return;                  // disconnected: nothing to re-arm for
            // Re-arm: a tune still in progress must keep coalescing.
            m_ioBoardThrottle->start();
        });
    }

    // Neither MOX nor TUNE defers the amplifier alone: the TX NCO/filter
    // already follow the requested band. Immediate sends also discard an older
    // coalesced value so the timeout cannot send the board back to that band.
    switch (m_ioBoardSchedule.request(m_connected, m_ioBoardThrottle->isActive(),
                                      bandChanged, target)) {
    case IoBoardAction::DropDisconnected:
    case IoBoardAction::Coalesce:
        return;
    case IoBoardAction::Send:
        break;
    }

    if (!sendIoBoardFrequency(target))
        return;
    m_ioBoardBandKey = targetBand;
    // Restarted rather than left running, so the cooldown is measured from the
    // push that actually went out — a band change mid-sweep resets the window
    // instead of inheriting the remainder of the previous one.
    m_ioBoardThrottle->start();
}

bool Hl2Backend::sendIoBoardFrequency(quint64 hz)
{
    // The only path from either throttle edge to the wire, so the connected
    // guard applies to both. A disconnected tune must not enqueue work for a
    // future session (MetisClient's guard and stop-time purge also enforce it).
    if (!m_connected) {
        m_ioBoardSchedule.reset();
        return false;
    }
    QMetaObject::invokeMethod(m_metis, "setIoBoardTxFrequencyHz",
                              Qt::QueuedConnection, Q_ARG(quint64, hz));
    return true;
}

void Hl2Backend::resetIoBoardSchedule()
{
    // Called on linkDown. A timer left running would make a reconnect coalesce
    // its connect-time frequency as pending instead of pushing it immediately.
    // The band key is cleared so the next session's first push is a band change
    // and takes the leading edge.
    if (m_ioBoardThrottle)
        m_ioBoardThrottle->stop();
    m_ioBoardSchedule.reset();
    m_ioBoardBandKey.clear();
}

// Bandscope block age, negative meaning "never observed" (which
// bandscopeBlockIsCurrent() and bandscopeHeadroom() treat as Absent). The one
// definition for all readers.
std::int64_t Hl2Backend::bandscopeBlockAgeMs() const
{
    return m_bandscopeBlockClock.isValid() ? m_bandscopeBlockClock.elapsed() : -1;
}

void Hl2Backend::resetBandscopeMirrors()
{
    m_bandscopeEnabled = false;
    // The gate does not survive a link edge, so neither does the auto-gain
    // loop's claim on it. Left set, a disarm after the link came back would
    // send a disable for a stream nothing had started.
    m_bandscopeOwnedByAutoGain = false;
    m_ep4Packets = 0;
    m_ep4Drops = 0;
    m_ep4Rewinds = 0;
    m_ep4Blocks = 0;
    m_ep4Timeouts = 0;
    // Back to "never seen", which is what makes the level rows go ABSENT again
    // rather than keep showing the previous session's last reading.
    m_bandscopeBlock = AetherSDR::hl2::Ep4Stats{};
    m_bandscopeBlockClock.invalidate();
    // Any frame request from the previous link is answered by MetisClient's own
    // teardown; dropping the id here only stops a stale one blocking the next
    // caller. It belongs in here rather than at the connect edge alone, for the
    // same reason the counters above do: a link that went down and came back
    // without a start() behind it is still a new link to whoever is asking.
    m_bandscopeFrameRequest = 0;
}

void Hl2Backend::applyBandFilter(const char* reason)
{
    if (!m_metis || m_rx.empty())
        return;

    // BEFORE the filter-byte comparison below, deliberately. The relay pattern
    // is unchanged across a move from 7.100 to 7.200 MHz and this function
    // returns early for it, but the IO board still needs the new frequency.
    applyIoBoardFrequency();

    // One filter board, N receivers: the J16 open-collector byte is
    // radio-wide. Agree-or-bypass: if every receiver wants the same filter,
    // engage it; otherwise release all relays (kOcNone) so no receiver is
    // attenuated by another's band choice. Bypass drops the AM-broadcast HPF
    // and can raise the floor near a broadcaster; it is logged below.
    // Keyed or tuning uses the transmit byte (Hl2HardwareOptions::FilterBoard
    // decides which path the relays sit in), and the TX receiver's filter wins
    // when spanned.
    const bool transmitting = m_keyed || m_tuning;

    int oc = -1;
    bool spanned = false;
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        const int want = static_cast<int>(
            transmitting ? m_hw.ocTransmitByteForHz(m_rx[i].sliceFreqHz)
                         : m_hw.ocReceiveByteForHz(m_rx[i].sliceFreqHz));
        if (oc < 0)
            oc = want;
        else if (want != oc)
            spanned = true;
    }
    if (oc < 0)
        return;

    if (spanned) {
        // While KEYED the transmit receiver's filter wins outright. Radiating
        // through a bypassed filter bank because a second receiver happened to
        // be parked on another band would put harmonics on the air, and no
        // receive-side convenience justifies that.
        if (transmitting) {
            const Receiver* txRx = rx(m_txDdc);
            oc = txRx ? static_cast<int>(m_hw.ocTransmitByteForHz(txRx->sliceFreqHz))
                      : static_cast<int>(kOcNone);
        } else {
            oc = static_cast<int>(kOcNone);
        }
    }

    if (oc == m_ocFilterByte)
        return;
    const int previous = m_ocFilterByte;
    m_ocFilterByte = oc;

    // INFO, not debug. There is no readback: the gateware forwards this byte to
    // the filter board over I2C and nothing comes back, so this log line is the
    // ONLY evidence of what the relays were told to do. A support log captured
    // after the fact has to already contain it. (aether.hl2)
    //
    // The spanned case names itself, because "why did my noise floor rise when I
    // opened a second receiver" is otherwise an unanswerable support question.
    QString forWhat;
    if (spanned) {
        QStringList mhz;
        for (const Receiver& r : m_rx)
            mhz << QString::number(r.sliceFreqHz / 1.0e6, 'f', 3);
        forWhat = QStringLiteral("receivers spanning bands (%1 MHz)%2")
                      .arg(mhz.join(QStringLiteral(", ")),
                           (m_keyed || m_tuning)
                               ? QStringLiteral(" — TX receiver's filter forced")
                               : QStringLiteral(" — BYPASSED, AM-broadcast HPF is out"));
    } else {
        forWhat = QStringLiteral("%1 MHz")
                      .arg(QString::number(m_rx[0].sliceFreqHz / 1.0e6, 'f', 6));
    }

    qCInfo(lcHl2).nospace()
        << "HL2 band filter: " << QString::asprintf("0x%02X", oc)
        << " (" << ocFilterName(static_cast<std::uint8_t>(oc)) << ") for "
        << forWhat
        << " — was "
        << (previous < 0 || previous > 0x7F
                ? QStringLiteral("unset")
                : QString::asprintf("0x%02X", previous))
        << ", trigger=" << reason;

    QMetaObject::invokeMethod(m_metis, "setBandFilter", Qt::QueuedConnection,
        Q_ARG(int, oc));
}

void Hl2Backend::emitSliceState(int ddc)
{
    const Receiver* r = rx(ddc);
    const auto* ids = m_ids.byDdc(ddc);
    if (!r || !ids)
        return;

    SliceDelta d;
    d.panId = ids->panId;
    // Publish only modes this radio demodulates: modeFromString() falls back
    // to USB, so an unsupported mode would play USB while reading back as
    // chosen. publishedModeStrings() is a subset of knownModeStrings(), so the
    // menu never offers a mode the restore boundary rejects.
    d.modeList = publishedModeStrings();
    d.frequency = r->sliceFreqHz / 1.0e6;   // MHz
    d.mode = r->mode;
    d.filterLow = r->filterLowHz;
    d.filterHigh = r->filterHighHz;
    d.audioGain = qRound(r->audioGain * 100.0f);
    d.audioMute = r->audioMuted;
    // The AGC pair the DSP is running, so a restored AGC is visible (#4909).
    // Safe to echo: SliceModel::applyDelta() assigns these without emitting
    // agcCommandIssued.
    d.agcMode = r->agcMode;
    d.agcThreshold = r->agcThresholdDb;
    // The squelch pair the receiver holds and has pushed to its chain, so the
    // SQL control shows the receiver's state. Safe to echo:
    // SliceModel::applyChanges() does not emit squelchCommandIssued.
    d.squelchOn = r->squelchOn;
    d.squelchLevel = r->squelchLevel;
    // Exactly one slice is the TX slice (the one on m_txDdc). Unset, txSlice()
    // is null and RadioModel's interlock refuses every key; set on all, the
    // operator could key from a receiver the TX NCO is not following.
    d.txSlice = (ddc == m_txDdc);
    // Exactly one slice is active, so consumers (e.g. the RX Controls applet)
    // resolve the receiver the operator is working on.
    d.active = (ddc == m_activeDdc);
    emit sliceChanged(ids->uiNumber, d);
}

void Hl2Backend::emitPanState(int ddc)
{
    const Receiver* r = rx(ddc);
    const auto* ids = m_ids.byDdc(ddc);
    if (!r || !ids)
        return;
    // The pan centre is the NCO, NOT the slice. This is the whole point of the
    // decoupling: the display describes where the receiver's window is, and the
    // slice moves inside it.
    //
    // The SPAN is radio-wide (0x00[25:24] is one field), so every pan reports
    // the same bandwidth and a different centre.
    emit panCenterBandwidthChanged(ids->panId, r->ncoHz / 1.0e6,
                                   static_cast<double>(m_sampleRateHz) / 1.0e6);
}

void Hl2Backend::emitAllSliceState()
{
    for (const auto& ids : m_ids.all())
        emitSliceState(ids.ddcIndex);
}

void Hl2Backend::emitAllPanState()
{
    for (const auto& ids : m_ids.all())
        emitPanState(ids.ddcIndex);
}

}  // namespace AetherSDR::hl2
