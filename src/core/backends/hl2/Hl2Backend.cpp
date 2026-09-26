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

// Hl2Backend.h seeds m_alcTargetPeak with a literal because it can only
// forward-declare Hl2TxDsp. This is what stops the two drifting: the seed has
// to keep meaning "the modulator's own default" for a pre-connect snapshot to
// be honest, and a silent divergence is exactly the class of readout error this
// backend's health section exists to eliminate.
//
// This pinned alcHoldBelowDbfs == -45.0 until the ALC's makeup half was removed
// and the hold with it. Re-pointing it rather than deleting it is deliberate:
// the compile-time link is the only thing that made the deletion of the Config
// field surface here as a build failure instead of a stale readout, and the
// target is what a mic peak is now measured against.
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

// Snap a requested span (Hz) to the rate that best matches it.
//
// Nearest in the LOG domain, not the linear one: the rates are octave-spaced, so
// linear-nearest is biased toward the wider neighbour everywhere (a request for
// 100 kHz is 4 kHz from 96k and 92 kHz from 192k linearly, but almost exactly
// halfway between them by ratio). Zoom is a multiplicative gesture — each wheel
// step scales the span — so the operator's sense of "closer" is the ratio, and
// matching that is what makes a zoom step land on the neighbouring rate rather
// than skipping one.
// The widest rate this session will offer.
//
// "Use low bandwidth mode" is an explicit statement that the link cannot carry
// much, and on this radio the span IS the data rate: 384 kHz is 25.2 Mbps of
// sustained UDP at 3048 packets/second. Offering it on a link the operator has
// already told us is constrained would produce a connection that drops rather
// than a display that is wide, so the ceiling comes down to 96 kHz (6.3 Mbps).
//
// Applied to the ADVERTISED limits as well as to requests, so the zoom control
// stops at the real ceiling instead of letting the operator drag into a span
// that will be silently refused.
int maxIqSampleRateHz() noexcept
{
    constexpr int kLowBandwidthCeilingHz = 96000;
    if (!Hl2Settings::lowBandwidth())
        return kIqSampleRatesHz[std::size(kIqSampleRatesHz) - 1];
    return kLowBandwidthCeilingHz;
}

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

// The same question for the AGC vocabulary, and it needs asking for the same
// reason: wdspAgcMode() FALLS BACK to medium for anything it does not
// recognise, so a corrupt or hand-edited document would otherwise turn into a
// silent "med" that capture then writes back as though the operator had chosen
// it. Dropping the field instead leaves the receiver on its own default, which
// is a value nobody is pretending was chosen.
//
// "med" and not "medium": this is SliceModel's four-way vocabulary
// (SliceModel::m_agcMode), and the restore boundary must speak exactly what
// the control produces or a round-trip would fail on the string alone.
bool isKnownAgcModeString(const QString& mode) noexcept
{
    const QString m = mode.trimmed().toLower();
    return m == QLatin1String("off") || m == QLatin1String("slow")
           || m == QLatin1String("med") || m == QLatin1String("fast");
}

// Default RX passband per mode, in Hz relative to the carrier. Sign carries the
// sideband, matching SliceModel's convention (USB-family positive, LSB-family
// negative, carrier-straddling modes symmetric) -- a table with the wrong sign
// here would be silently "corrected" by SliceModel::normalizeFilterPolarity and
// the mistake would never surface.
//
// The digital entries are deliberately the widest of the set. DIGU is the mode
// WSJT-X selects, and it must pass the whole 3 kHz audio window the decoder
// expects; a snug SSB passband would clip the top of the FT8 sub-band and drop
// exactly the signals at the edges.
std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    // CW: 500 Hz CENTRED ON THE CARRIER, both sidebands, because in CW the
    // operator-facing passband is measured from the signal and not from the
    // audio it becomes. The pitch offset lives in the BFO (cwBfoHz) instead —
    // see the note there — so CWU and CWL share one table entry and differ only
    // in which way the BFO leans.
    //
    // This is the convention the rest of the app already assumes:
    // VfoWidget::applyFilterPreset builds every CW preset as {-w/2, +w/2}
    // ("centred on carrier — radio's BFO handles pitch offset"), and a Flex
    // reports CW cuts the same way (FlexLib Slice.cs clamps them to
    // ±12000 - CWPitch, which only makes sense for cuts measured from the
    // carrier). Returning {350, 850} here put the passband skirt a whole pitch
    // to the RIGHT of the marker on the panadapter and, worse, meant the
    // gateware transmitted a CW carrier at the marker while the receiver
    // listened 600 Hz above it.
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

// The CW BFO offset for `mode`, in Hz of audio: where a signal sitting exactly
// on the marker should come out. Positive for upper-sideband CW, negative for
// lower, zero for every mode that has no BFO.
//
// WDSP has no CW mode in the sense a superhet does. SetRXAMode(CWU) does not
// insert a beat oscillator -- in this chain the NBP edges are what select the
// sideband (see WdspChannel::setFilter), and the demodulator is a plain
// direct-conversion detector for every mode. So the pitch has to be produced
// the same way a real BFO produces it: by offsetting the frequency the detector
// treats as zero. Everything else follows from that one offset --
// dspFilterHz() translates the operator's carrier-relative cuts into the audio
// window, and rxShiftHz() moves the detector's zero so the marker lands on the
// pitch instead of on DC.
double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return static_cast<double>(pitchHz);
    if (u == QLatin1String("CWL"))
        return -static_cast<double>(pitchHz);
    return 0.0;
}

// Default TRANSMIT passband per mode, in Hz. POSITIVE for every mode, and that
// is not an oversight — TX and RX use opposite conventions and mixing them up
// transmits on the wrong sideband:
//
//   RX (RXANBPSetFreqs): the SIGN of the passband selects the sideband. The
//                        mode does not.
//   TX (Hl2TxDsp):       the MODE selects the sideband -- isLowerSideband()
//                        negates Q -- and the bandpass is an audio-domain
//                        magnitude. Handing it a negative pair flips LSB and
//                        DIGL onto the upper sideband.
//
// Measured, not assumed: hl2_txdsp_test drives a 1 kHz tone through the real
// modulator and reads the sideband off the emitted IQ. With a negative pair,
// LSB lands on the same wire bin as USB.
//
// THE TX RULE ABOVE IS Hl2TxDsp'S, NOT WDSP'S. SetTXABandpassFreqs is signed
// exactly like RXA: TXASetupBPFilters handles TXA_LSB and TXA_USB in one
// fall-through case with identical CalcBandpassFilter arguments, and create_txa
// defaults TXA_LSB to f_low = -5000, f_high = -100. Give a TXA channel this
// table's positive pairs and LSB comes out on the SAME sideband as USB. Read
// docs/HERMES.md section 5 before migrating transmit onto a WDSP TXA channel.
//
// Voice stays at the established 300..2700 rather than inheriting the wider RX
// window — that width is deliberate (see Hl2TxDsp::Config), and widening every
// SSB transmission is not part of making WSJT-X work. The digital modes get the
// full window because that is the one the decoder occupies.
// A QString adapter over hl2::defaultTxPassbandForModeName, which is where the
// mapping itself now lives. It moved because hl2_txdsp_test's characterisation
// sweep was mirroring these pairs by hand and had no way to notice if they
// changed; see the note on the policy function for the argument. Behaviour is
// unchanged -- the policy ASCII-uppercases its own input, and every literal it
// compares against is ASCII, so the toUpper() this used to do could only ever
// have altered characters that could not match either way.
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
    // therefore not a constant expression. See setAutoRfGainMode.
    //
    // Setting a law is not arming one: m_autoRfGainEnabled stays false, no
    // stream starts, and nothing about this installation's behaviour changes
    // until an operator switches the control on.
    m_autoGainConfig = AetherSDR::hl2::bandscopeReleaseConfig(
        AetherSDR::hl2::gatedPeakBiasDbForPeriod(
            MetisClient::bandscopeSamplePeriodMs()));

    // No parent: moveToThread() refuses an object that has one, and both of
    // these belong on the I/O thread rather than the GUI thread. They are
    // destroyed explicitly in the destructor after the thread is joined.
    m_metis = new MetisClient(nullptr);
    m_txDsp = new Hl2TxDsp(nullptr);
    // One receiver's STATE exists from construction; its DSP chain does not.
    //
    // How many receivers run is a property of the radio (discovery byte 0x13)
    // and of the link budget at the chosen sample rate, so the DSP chains cannot
    // be built until connectRadio(). But the slice state has to exist before
    // then, because mode, passband and AGC are pushed at the seam BEFORE a radio
    // is connected — RadioModel does it, and so does anything restoring a
    // session. With no receiver to hold them those calls would be silently
    // dropped and the radio would come up on defaults instead.
    //
    // buildReceivers() preserves this state and only replaces the DSP.
    m_ids.reset(1);
    m_rx.assign(1, Receiver{});

    // Transmit availability, decided once here rather than per-key so the answer
    // cannot change under a running key.
    //
    // A normal interactive run can transmit: this is a transceiver, and an
    // operator at the keyboard keying their own radio needs no special flag.
    //
    // An AUTOMATION run defers to the bridge's existing TX gate
    // (AETHER_AUTOMATION_ALLOW_TX). That gate already exists precisely because
    // a scripted client that can key is a different risk from a human at the
    // controls, and adding a second, HL2-specific variable alongside it would
    // have meant two things to get right instead of one -- and a script that
    // satisfied the automation gate but silently could not key.
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
        // applyKeying() DIRECTLY, with cwBreakIn TRUE, and that argument is the
        // whole point of this line — see the QSK branch in applyKeying().
        //
        // THIS USED TO CALL setKeying(), WHICH HARD-CODES cwBreakIn=false.
        // That made the flag unreachable on the only edge that arms the unkey
        // hold: setCwKeying() passes true on the key-DOWN and nowhere else, so
        // "applyKeying already receives cwBreakIn" was true of the class and
        // false of the key-UP. A hold gated on the parameter as it stood would
        // have been dead code in full break-in, which is exactly the case the
        // gate exists for.
        //
        // NOTHING ELSE MOVES ON THE WIRE. applyKeying()'s only other use of the
        // argument picks setCwMox() over setMox(), and MetisClient::setMoxImpl()
        // reads cwBreakIn only in its `keyed ?` arm: on an UNKEY the two are the
        // same function. Verified against the source rather than assumed,
        // because this is a transmit path.
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
        // The bandscope belongs to the SESSION: MetisClient::start() comes up
        // with wide_spectrum clear and ep4_seq_no restarted, so carrying the
        // previous link's state here would report a stream nothing enabled.
        //
        // That premise holds on EVERY linkUp edge, but only because the silence
        // watchdog was made to end the gate's intent too. Not every linkUp has a
        // start() behind it: onWatchdogTick() emits linkDown after 2 s of EP6
        // silence WITHOUT calling stop(), and if EP6 resumes before RadioModel's
        // reconnect timer fires, handleDatagram emits linkUp again on the same
        // session. Before that fix this line reported OFF beside a gate that was
        // still cycling. (PR #5650 review, K5PTB.)
        resetBandscopeMirrors();
        m_linkStatsTimer->start();
        emit connected();
        // Publish initial slice/pan state AFTER connected(), not in connectRadio():
        // RadioModel::onConnected() stages every existing model as "previous
        // session" leftovers, so anything emitted earlier is wiped before the UI
        // ever sees it (slice panel stuck empty / 0.000000).
        // ORDER MATTERS, in two directions, and they pull against each other.
        //
        // pushInitialState() derives each receiver's passband from its mode and
        // updates the state that emitAllSliceState() then publishes. Publish
        // before deriving and the slice is told the stale values, so a fresh USB
        // connect showed DIGU's 150..3000 while the backend itself had corrected
        // to 100..2900. The radio was right and the UI was wrong, which is the
        // harder direction to notice. (#4484)
        //
        // But pushInitialState() ALSO reports each pan's zoom limits, and
        // RadioModel drops that report when no PanadapterModel resolves: its
        // panBandwidthLimitsChanged handler does `if (!pan) return;` with no
        // materialisation, and onConnected() — synchronous inside emit
        // connected() above — just ran stageSessionModelsForReconnect(), which
        // clears m_panadapters AND m_activePanId. Only emitPanState()'s
        // panCenterBandwidthChanged materialises our pans.
        //
        // So emitAllPanState() has to come FIRST: the pans must exist before
        // anything describes them. Otherwise the limits are dropped for the whole
        // session, nothing re-emits them, and SpectrumWidget keeps the FlexLib
        // fallback of 5.4 MHz — fourteen times the widest window this receiver
        // has, which is the black-bar over-zoom #4470 fixed.
        //
        // emitPanState() reads only each receiver's ncoHz and m_sampleRateHz,
        // neither of which pushInitialState() touches, so hoisting it is safe.
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
        // THE OPERATOR'S AUTOMATIC-GAIN SWITCH, restored last.
        //
        // Last because setAutoRfGain() refuses to arm from a baseline above
        // kAutoRfGainMaxBaselineDb, and the restored baseline only reaches
        // m_lnaGainDb in pushInitialState() above. Arming earlier would consult
        // a number that had not been restored yet and refuse — or worse, not.
        //
        // A REFUSAL HERE IS CORRECT AND IS LOGGED BY setAutoRfGain: the control
        // stays off, the operator's preference stays recorded, and they are
        // told why. What must not happen is arming silently against a gain axis
        // this radio is not trusted on.
        if (m_autoRfGainWanted && !m_autoRfGainEnabled) {
            setAutoRfGain(true);
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
    // The GAIN the ALC is applying, which is a second meter and not a second
    // view of the first: TX:ALC above is a LEVEL fed from alcPeak, for the
    // reason given there, and the two move in opposite directions.
    //
    // This is the number that answers "is the ALC holding, and by how much".
    // It was mirrored into m_alcGainDb for healthSnapshot() and the bridge but
    // did not enter MeterModel, so neither radiocert nor a future UI consumer
    // could use it. TX:ALCGAIN publishes that producer-side feed; the proposed
    // operator-facing gauge is deliberately a separate change (#5636).
    //
    // Both paths stay: the mirror is a plain read on this thread for the
    // snapshot, and the meter is the normalized model/certification feed.
    // Publishing one does not make the other redundant, and dropping the
    // mirror would put healthSnapshot() back to re-deriving a value it is
    // already handed.
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

    // Stream-free telemetry (#15) is owned by RadioModel and injected via
    // setTelemetryService(). This backend only tells it what the IQ path is
    // doing; it must not own it, because it has to answer when no backend
    // exists at all.
    //
    // TELLING IT IS THIS TIMER, and it is the whole of the wire.
    //
    // Started here and NEVER stopped, deliberately not the link-stats timer:
    // that one stops on linkDown and connectFailed, which would silence the
    // poll state in the three cases the poller exists for.
    //
    // This tick was deleted once, by the refactor that moved the poller out of
    // this class -- the regex removing the poller's construction took the timer
    // with it. Nothing failed: the cadence rule was still correct and its unit
    // test still passed, and the only symptom was a live connect reporting
    // `connected=True pollMs=1000`, the poller still polling 1025 through a
    // healthy stream. hl2_telemetry_wire_test now asserts this timer's effect
    // rather than the rule's correctness, because the rule was never the part
    // that broke.
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
    // Fresh packets since the last tick — the transport-level proof of life the
    // heartbeat runs on. Computed here rather than in linkStats() because the
    // comparison CONSUMES the previous value, and linkStats() is a const getter
    // any caller may poll at any rate.
    //
    // STORED ON m_link, not on the outgoing copy. It used to be written only to
    // the local `s` that is emitted, so the SIGNAL path carried liveness and the
    // GETTER path never did: linkStats() returned m_link.alive, which nothing
    // had ever written, so every poller — the `liveness` automation verb among
    // them — read a healthy radio as dead. Two paths, one of them silently
    // wrong, and the wrong one is the one a diagnostic uses.
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

    // HELD BY ANOTHER CLIENT, from the RADIO rather than from a cached scan.
    // The header named the picker as the caller that would pass this in, and
    // nothing ever did: the only call site passed false, so HeldByOther was
    // unreachable and the situation the `telemetry` verb's rationale is built
    // around could not be entered. Every discovery reply carries the same bit
    // the picker would have read (DiscoveryReply::streaming, status byte 0x03),
    // it is fresher than a scan, and it already arrives on this path.
    //
    // Only meaningful while WE are not the ones streaming — our own session
    // sets that bit too, and calling our own stream "somebody else's" would
    // invert the reading entirely. hl2LinkStateFor() consults it only when
    // disconnected for the same reason; the guard is repeated here so the
    // argument we pass is true on its own terms.
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

    // AND THE PA TEMPERATURE METER, when the in-band path is not delivering it.
    //
    // `RAD:PATEMP` was published from exactly one place -- publishTelemetry(),
    // which runs only while EP6 is arriving. So once the stream stopped, the
    // needle held whatever it last showed, indefinitely, while the health row
    // beside it correctly said nothing. A meter that keeps displaying a reading
    // from a session that ended is the same frozen-reading failure this whole
    // feature exists to expose, one surface over (#5642 review).
    //
    // The poller's reading is used RAW rather than fed into m_paTempC: that
    // pole belongs to the in-band session and linkDown() clears it, so blending
    // a 1 Hz out-of-band sample into it would re-seed the filter the next
    // stream is supposed to start clean. This is one poll, published as itself.
    //
    // WHAT THIS DOES NOT FIX, stated rather than implied: the meter seam carries
    // a bare double and has no way to spell "unknown", so with NO stream-free
    // reading either -- disconnected with nobody watching, which stops the
    // polling by design -- the needle still holds its last value. The health row
    // beside it says nothing, correctly, and that remains the surface that can
    // tell the two apart.
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

    // CAPTURE THE UI NUMBER, NOT THE DDC INDEX.
    //
    // A DDC index is not stable for the life of a receiver: closing the middle
    // of three renumbers every receiver after it, because the gateware streams
    // numRx CONTIGUOUS receivers and the index IS the slot in the EP6 round.
    // A lambda holding the old index would resolve to the wrong receiver — or,
    // at the end of the list, to none at all, and that receiver's spectrum would
    // simply stop arriving with nothing logged.
    //
    // The UI number never changes (Hl2ReceiverMap::remove), so resolving through
    // it at signal time survives any renumbering with no rewiring at all.
    const int ui = ids->uiNumber;

    connect(dsp, &Hl2RxDsp::spectrumReady, this,
            [this, ui](const std::vector<float>& bins) {
        if (!m_ids.byUi(ui))
            return;
        // dBFS -> dBm through the one object that owns the reference. Two
        // terms now: the DERIVED full-scale figure, and -lnaGain. The second
        // is the part that is exactly right whatever the first is worth --
        // it holds the trace still across a gain change instead of letting
        // the whole display jump. The first moves the floor once, to a
        // figure that can be checked, and never again.
        //
        // WHICH IS WHY THE off == 0.0 FAST PATH NOW RARELY FIRES: at the
        // default 0 dB of gain the offset is the constant +3, not zero. The
        // branch stays because an operator at +3 dB of LNA gain still hits
        // it, and because it is the same test either way.
        //
        // The reference is SHARED because the LNA it describes is shared —
        // one AD9866 behind every DDC — so a gain change moves all four
        // traces together, which is what actually happened to the signals.
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

    // Build the new receiver's state outside m_rx, then append it. Derived from
    // the EXISTING receivers, which is why it is assembled before the push rather
    // than patched up after it.
    //
    // Inherit the first receiver's settings, not construction defaults: a new
    // pane opening on 10 MHz USB when the operator is working 40 m would look
    // like the radio changed band on its own. Same reasoning as at connect.
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

    // The WIRE FIRST, so the radio is already streaming the new layout before the
    // DSP that consumes it exists. The reverse order would leave a configured
    // chain briefly reading a payload with one fewer receiver in it. The extra
    // slot the radio now sends goes nowhere until publishIoDsps() below — the
    // fan-out is still working from a list one shorter and clamps to it.
    // This restarts the EP6 stream — see MetisClient::setReceiverCount.
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::BlockingQueuedConnection,
            Q_ARG(int, static_cast<int>(m_rx.size())));
    }

    Receiver& r = m_rx.back();

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

    Hl2RxDsp::Config dc;
    // THE COMMITTED RATE, NOT m_sampleRateHz. A receiver can be opened while a
    // rate change is still building, and m_sampleRateHz has already moved to the
    // rate being ATTEMPTED. Building this chain for that rate would hand it a
    // decimation geometry for IQ the radio is not producing yet — and if the
    // build in flight then fails, never will. The chain is built for what the
    // wire is actually carrying; finishRateChange() rebuilds it if and when the
    // crossing commits.
    dc.inputSampleRateHz = m_rateLedger.committed();
    dc.audioSampleRateHz = 24000;
    dc.mode = modeFromString(r.mode);
    std::tie(dc.filterLowHz, dc.filterHighHz) = dspFilterHz(r);
    dc.agcMode = wdspAgcMode(r.agcMode);
    dc.maximumAgcGainDb = m_dbRef.agcCeilingDb(r.agcThresholdDb);
    bool ok = false;
    Hl2RxDsp* dsp = r.dsp;
    QMetaObject::invokeMethod(dsp, [dsp, &dc, &err, &ok] {
        ok = dsp->configure(dc, &err);
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        qCWarning(lcHl2) << "HL2: receiver" << ddc << "DSP failed —"
                         << QString::fromStdString(err);
        dsp->disconnect(this);
        dsp->deleteLater();
        // Safe to destroy without withdrawing it first: this chain was never
        // published, so the fan-out has never held a pointer to it.
        m_rx.pop_back();
        m_ids.remove(ddc);
        if (m_metis) {
            QMetaObject::invokeMethod(m_metis, "setReceiverCount", Qt::QueuedConnection,
                Q_ARG(int, static_cast<int>(m_rx.size())));
        }
        return false;
    }

    // What this chain was actually built for, so a rate change that commits
    // while it was being opened can tell that it still needs rebuilding.
    r.configuredRateHz = dc.inputSampleRateHz;

    int channelId = -1;
    QMetaObject::invokeMethod(dsp, [dsp, &channelId] {
        channelId = dsp->wdspChannelId();
    }, Qt::BlockingQueuedConnection);
    if (auto* ids = m_ids.mutableByDdc(ddc)) {
        ids->dspChannel = channelId;
        ids->analyzerId = ids->uiNumber;
    }

    // Put the NCO where this receiver's state says it should be. setReceiverCount
    // starts a new receiver on RX1's frequency, which is only right if nothing
    // moved it since.
    if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
            Q_ARG(int, ddc),
            Q_ARG(std::uint32_t, ncoCommandHz(r.ncoHz)));
    }
    QMetaObject::invokeMethod(r.dsp, "setShift", Qt::QueuedConnection,
        Q_ARG(double, rxShiftHz(r)));
    // Notches are radio-wide, so a receiver that appears after them has to be
    // brought up to date. Otherwise adding a second panadapter gives you one
    // receiver with the interferer notched out and one without.
    seedNotches(r);
    // Per-receiver, so a new panadapter starts from ITS OWN state rather than
    // inheriting RX1's — which for a receiver that has never been configured is
    // the default of off.
    pushNoiseBlanker(r);

    // AND ONLY NOW does the sample path learn about it. Last, after the chain is
    // configured, tuned and shifted — so the first block it is ever handed lands
    // in a receiver that is fully set up, rather than one still being assembled.
    publishIoDsps();

    const auto* ids = m_ids.byDdc(ddc);
    qCInfo(lcHl2) << "HL2: added receiver — DDC" << ddc << "pan" << (ids ? ids->panId : QString())
                  << "WDSP channel" << channelId
                  << "; running" << m_rx.size() << "of" << ceiling;

    // Publish it exactly as at connect, in the same order: pan geometry first so
    // the model can materialise the pane, then the slice that lives in it.
    emitPanState(ddc);
    emitSliceState(ddc);
    if (ids) {
        emit panBandwidthLimitsChanged(
            ids->panId,
            static_cast<double>(kIqSampleRatesHz[0]) / 1.0e6,
            static_cast<double>(m_sampleRateHz) / 1.0e6);
        emit panRfGainInfoChanged(ids->panId, kLnaGainMinDb, kLnaGainMaxDb, kLnaGainStepDb);
        // EFFECTIVE, not baseline: a pan created while an automatic control is
        // holding gain down must come up showing the same number as its
        // siblings, not the operator's untouched baseline (Hl2GainSplit.h).
        emit panRfGainChanged(ids->panId, lnaEffectiveDb());
    }
    // A new receiver can change whether the set spans bands.
    applyBandFilter("add receiver");
    publishWideState();
    return true;
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
    // ---- the two roles that point AT a DDC index have to survive the removal ----
    //
    // Both are stored as DDC indices, and removal RENUMBERS every index after
    // the closed one (Hl2ReceiverMap::remove, because the gateware needs them
    // contiguous). So there are two distinct cases and only handling the first
    // is a silent misdirection:
    //
    //   the role was ON the closing receiver   -> move it somewhere that exists
    //   the role was AFTER the closing one     -> its index just shifted down
    //
    // Miss the second and, closing DDC 0 of three, transmit "on DDC 2" ends up
    // naming a receiver that is now something else — and nothing reads a TX NCO
    // back to contradict it.
    if (ddc == m_txDdc) {
        // Transmit has to live SOMEWHERE. Move it to the first surviving
        // receiver rather than leaving m_txDdc pointing at a receiver that no
        // longer exists — which would make txSlice() null and every later key
        // attempt die in the interlock with no explanation.
        //
        // DDC 0 in POST-removal numbering, which always exists because closing
        // the last receiver is refused above. An earlier version picked
        // `ddc == 0 ? 1 : 0` in PRE-removal numbering — and old DDC 1 becomes
        // DDC 0 the moment the map renumbers, so closing the transmitting
        // receiver 0 left m_txDdc naming a receiver one past where transmit
        // actually went.
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

    // Tear the DSP down BEFORE the wire shrinks, so nothing is left consuming a
    // slot the radio has stopped sending. The reverse order feeds the surviving
    // receivers' samples into a chain that thinks it is still receiver N.
    //
    // WITHDRAW, THEN DESTROY, and never the other way round. publishIoDsps()
    // blocks until the I/O thread has taken the shortened list, so by the time the
    // destruction below is posted the fan-out has already stopped feeding this
    // chain. Destroying first would leave the I/O thread holding a pointer to an
    // object queued for deletion, with a real-time path dereferencing it.
    // WITHDRAW EVERYTHING, not just the doomed chain, and hold that across the
    // receiver-count change.
    //
    // Publishing the SHORTENED list here was wrong in the window that follows:
    // erase() shifts the survivors down, but the wire is still sending the old
    // number of slots, so the fan-out mapped slot k to the chain that had just
    // moved into index k. Every receiver above the closed one was fed the slot
    // below it — including the closed receiver's own IQ, landing in whichever
    // chain shifted into its place. That is precisely the misfeed the comment
    // above says this ordering exists to avoid.
    //
    // The compaction of m_ioDsps and the wire's setReceiverCount cannot be made
    // atomic with respect to each other, so no shortened list is safe to publish
    // between them. The empty list is, because it is correct whatever arrives.
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
    int want = -1;
    bool spanned = false;
    for (const Receiver& r : m_rx) {
        const int w = static_cast<int>(ocFilterByteForHz(r.sliceFreqHz));
        if (want < 0)
            want = w;
        else if (w != want)
            spanned = true;
    }
    for (const auto& ids : m_ids.all())
        emit panWideChanged(ids.panId, spanned);
}

// AFFINITY, ENFORCED RATHER THAN TRUSTED — AND IN THE BUILD THAT SHIPS.
//
// Every caller of the two helpers below was traced to the backend's own thread:
// applyKeying(), setTxAudioMonitor(), pushInitialState(), and the unkey timer,
// which is parented to the backend and therefore fires there. The check exists
// because the cost of being wrong is SILENT: m_rxAudioMuted is read by
// mixReceiverAudio() with no synchronisation, on the strength of both being on
// this thread, and a caller arriving from the I/O thread would produce a torn
// gate that only misbehaves under load.
//
// WHY NOT Q_ASSERT ALONE, which is what this was. Qt defines QT_NO_DEBUG for
// every non-Debug configuration, so Q_ASSERT compiles to nothing in
// RelWithDebInfo — the configuration this project builds, measures and ships.
// A check that disappears in exactly the build where it matters makes the
// paragraph above true of the check as well as of the defect (ten9876, #5850
// review). So: a warning in every build, and Q_ASSERT_X on top of it so a debug
// run stops AT the offending call instead of logging past it.
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
    // THE SAME FLAG THE DEMODULATOR IS MUTED ON, not a second expression that
    // happens to agree with it most of the time (#5497).
    //
    // It used to read `m_keyed && !m_txMonitor`, evaluated here, while the
    // demodulator was muted on `key && !m_txMonitor` evaluated in
    // applyKeying(). Those agreed exactly until the unmute was deferred past
    // the radio's T/R — after which this gate would have re-opened 70 ms before
    // the demodulator did, and those 70 ms would have been DIGITAL ZEROS pushed
    // into the engine. Gating both on m_rxAudioMuted makes the hold a continued
    // GAP in audioFrameReady() instead, so the engine's presentation prebuffer
    // refills with real audio when the hold ends rather than with silence.
    //
    // Belt and braces with the demodulator mute, as before: this drops any
    // block that was already in flight when the key went down, while
    // Hl2RxDsp::setAudioMuted stops the pipeline FILLING with our own
    // transmission.
    //
    // THE MONITOR EXCEPTION IS STILL HONOURED HERE, and it has to be.
    // audioFrameReady() is emitted from this function and nowhere else, and it is
    // what feeds the engine's "output" capture — so an early return here silences
    // the capture no matter what the DSP is doing. Porting setTxAudioMonitor as a
    // demodulator-mute change alone would leave it a no-op on this backend, and a
    // diagnostic that reads silence draws a confident wrong conclusion from it
    // (#4487 review, finding 1). Off by default; only a measurement turns it on.
    // m_rxAudioMuted carries that exception: applyKeying() never sets it while
    // the monitor is on, and setTxAudioMonitor() clears it mid-over.
    //
    // ONE BLOCK OF SKEW REMAINS AND IS NOT HIDDEN: the flag clears on this
    // thread while the unmute rides a queued connection to the DSP thread, so
    // at most one already-zero-filled block can pass this gate at the end of
    // the hold. That is one block, not the 70 ms the old gate would have let
    // through, and it is the same direction of skew the SliceSamplingGate
    // comment describes.
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
            q.clear();
            return;
        }
        // THE EDGE INTO THIS PATH. The queue is not empty, which means the
        // min()-aligned drain below ran while a second receiver was contributing
        // and left residue in this one — the deeper of two queues always keeps
        // some — and then that receiver was muted, which clears only ITS queue.
        // Emitting `pcm` here and clearing would discard the residue: a short gap
        // in the audio at the instant the operator mutes a slice, which reads as
        // the mute glitching the wrong receiver. So flatten the whole queue.
        //
        // Whole stereo frames either way: every insert adds an interleaved block
        // and every drain takes an even count, so the residue cannot be odd and
        // cannot swap the channels of what follows it.
        m_mixAccum.assign(q.cbegin(), q.cend());
        q.clear();
        publishLegacyAudio(floatBytes(m_mixAccum));
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
}

void Hl2Backend::releaseReceiverDsps()
{
    // WITHDRAW EVERY CHAIN FIRST, then destroy them — same order, and same reason,
    // as removePanadapter(). Collect and null the pointers, publish the now-empty
    // list (blocking, so the I/O thread has stopped feeding them when it returns),
    // and only then post the destruction.
    std::vector<Hl2RxDsp*> doomed;
    doomed.reserve(m_rx.size());
    for (Receiver& r : m_rx) {
        if (!r.dsp)
            continue;
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
    // THE BUILD THREAD FIRST, before the I/O thread it posts back to. A build
    // in flight hands its finished channels to the I/O thread's loop, so
    // joining it while that loop is still running is what lets those channels
    // be installed (or, if the generation moved on, destroyed) by the thread
    // that owns them rather than leaking with the undelivered event. Joining in
    // the other order would race the loop's end against the post.
    //
    // This BLOCKS for an in-flight OpenChannel, exactly as the I/O-thread join
    // below does and for the same reason: WDSP's open cannot be cancelled. A
    // family switch or an app quit during a rate change waits it out. That is
    // the same GUI stall the connect split leaves standing (see below).
    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }
    if (m_ioThread) {
        // Stop the wire ON its own thread and WAIT for it. A queued stop() would
        // never run -- quit() below ends the event loop that would deliver it --
        // and tearing the socket down from this thread is the affinity bug this
        // whole change exists to avoid.
        //
        // THIS BLOCKS FOR AN IN-FLIGHT DSP BUILD. beginDspSetup()'s job is a
        // single event on that thread's loop and OpenChannel cannot be cancelled,
        // so a family switch or an app quit during a cold connect waits out the
        // rest of the planning -- on the GUI thread. It is the one GUI stall the
        // three-phase split does NOT remove, and splitting the connect is what
        // made it reachable: the operator can now use the UI while the chains
        // open, and reaching for a different radio is the obvious thing to do
        // while waiting. docs/HERMES.md §22.4. It is also what keeps the QPointer in
        // beginDspSetup() sound, so a fix here has to deal with that too.
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
    // NO REPEATER DUPLEX AND NO TONE ENCODE, because this radio cannot key FM
    // at all -- receiveOnlyModes below says so, and these two controls are the
    // surface that still implied otherwise.
    //
    // hasFmRepeaterOffset was never DECLARED here; it was INHERITED. The struct
    // defaults it true, so a radio that omits it claims a duplex offset, and
    // the HL2 omitted it. There is nothing behind that claim:
    // IRadioBackend::setSliceRepeaterOffsetDir and setSliceFmRepeaterOffset are
    // virtuals with empty bodies and this backend overrides neither, so the
    // offset spin and the +/-/simplex buttons -- enabled from this flag in BOTH
    // VfoWidget::configureFmToneControls and RxApplet::configureFmToneControls --
    // moved a number that reached nothing. A dead control that looks live is
    // the failure this field exists to prevent, and the two backends that do
    // decline it (RtlSdrBackend, and IcomCivBackend when the model profile has
    // no duplex) decline it for exactly this reason. The HL2 has no such verb;
    // it has no command plane at all.
    //
    // fmTonePresentation WAS declared, as Legacy, and Legacy is the value that
    // fills the tone-mode combo from legacyFmToneModes() -- i.e. it OFFERS
    // CTCSS ENCODE. There is no CTCSS encoder on this path: grep the whole of
    // src/core/backends/hl2/ for "ctcss" and nothing answers, and this backend
    // overrides none of setSliceFmToneMode, setSliceFmToneValue,
    // setSliceFmToneRxValue or setSliceFmDtcs, all of which are no-op virtuals
    // in IRadioBackend.
    //
    // AND THE MODE CANNOT BE KEYED AT ALL, which is the fact that does not move
    // with the build. FM and NFM are on receiveOnlyModes below, so
    // RadioModel::refuseKeyInReceiveOnlyMode() refuses every TxActivity in
    // them. Resting the argument there rather than on the modulator is
    // deliberate: the modulator is chosen at build time by AETHER_HL2_TX_TXA
    // (default ON = a WDSP TXA channel; OFF = the in-tree phasing modulator),
    // and a comment that described only one of the two would be half wrong in
    // every build. TXA's own chain does carry fmmod, which is exactly why the
    // honest gate is the declaration and not the DSP.
    //
    // Hidden is the honest value and is the one both widgets read to WITHDRAW
    // the tone controls rather than show a control that does nothing -- by
    // different mechanisms, which is worth stating because this comment is the
    // artifact a later reader will trust over the code.
    // VfoWidget::configureFmToneControls hides a CONTAINER
    // (m_fmToneContainer->setVisible(modeEligible && presentation != Hidden));
    // RxApplet::configureFmToneControls hides the individual CHILDREN
    // (m_toneModeCmb, m_toneValueCmb and the CTCSS/DTCS combos) and never
    // touches m_fmContainer by presentation at all -- that one follows the
    // MODE, not this capability. The operator-visible outcome is the same in
    // both: under Hidden no tone control is shown.
    //
    // WHAT THIS DOES NOT TOUCH: receive. FM and NFM demodulate exactly as
    // before -- WDSP's FM demodulator is unaffected by either field. What is
    // withdrawn is a set of TRANSMIT-side controls for a mode this backend
    // already refuses to key in.
    c.hasFmRepeaterOffset = false;
    c.fmTonePresentation = FmTonePresentation::Hidden;
    c.fmDtcsCodes = {};
    // The CEILING, not the running count. A capability answers "what can this
    // radio do", and receivers are now added on demand — so reporting the
    // running count would tell the UI the limit was already reached and
    // "Add Panadapter" could never be offered.
    //
    // The ceiling is the board's own reported count (discovery byte 0x13,
    // never hardcoded — oracle §1) capped by the link budget at the span
    // currently running, so it FALLS when the operator zooms out. That is
    // correct rather than awkward: at 384 kHz a fourth receiver genuinely
    // cannot be delivered, and the honest limit is 3.
    //
    // Backlog item 6 ("receiver count from discovery 0x13; stop hardcoding
    // maxSlices") is what this closes.
    const int ceiling = m_connected ? receiverCeiling()
                                    : std::max(1, m_ids.size());
    c.canCreateSlices = false;
    c.maxSlices = ceiling;
    c.maxPanadapters = ceiling;
    for (const int rate : kIqSampleRatesHz)
        c.sampleRatesHz.append(rate);
    PanSpanModel span;
    // THE SPAN IS THE SAMPLE RATE, so the four rates above are not just stream
    // rates — they are every span this radio can produce, and 48 kHz is a
    // FLOOR, not a preference.
    //
    // This radio ships raw IQ and the spectrum is computed here from what
    // arrives (Hl2Spectrum, fed by Hl2RxDsp). There is no stage between the
    // DDC and the FFT that could narrow the window: a 5 kHz pan — the span a
    // Flex CW or FT8 operator runs routinely — would need samples the radio
    // never sent, because the DDC's decimation is what sets the rate in the
    // first place. So the client must snap a zoom request to one of these
    // (nearestIqSampleRateHz) and clamp the control at the narrowest, rather
    // than pass a literal span down and get a silent refusal.
    //
    // Upstream #5223 is the RFC for showing a sub-window of a delivered span at
    // full bin resolution. That would change what the DISPLAY shows; it would
    // not change what this radio can deliver, which is what this declares.
    span.followsSampleRate = true;
    // ONE SPAN FOR THE WHOLE RADIO. The HPSDR config command carries a single
    // two-bit sample-rate field for the board — MetisProtocol's ccConfig packs
    // SampleRate into C1[1:0], alongside the receiver COUNT in C4, and there is
    // no per-receiver rate anywhere in the frame. So changing one panadapter's
    // span moves every receiver onto the new rate and rebuilds all of their DSP
    // chains (applyPanBandwidth).
    //
    // This is also why receivePanBandwidthControl above is nullopt: the control
    // is real, but it is radio-wide, and publishing it as a per-pan authority
    // would let an operator narrow one window and silently retune the other
    // three. The same shared budget is why receiverCeiling() FALLS as the span
    // widens: span and receiver count draw on the same 100BASE-T link.
    span.radioWide = true;
    c.panSpanModel = span;

    PanAmplitudeModel amplitude;
    // THE Y AXIS IS dBFS WEARING A dBm LABEL, and this says so rather than
    // letting the label stand for a calibration that does not exist.
    //
    // Not "the HL2 cannot be calibrated" — it is that nothing on this radio
    // reports what 0 dBFS is worth at the antenna, and no HL2 oracle states a
    // figure for it. It is a per-unit property of the board, the ADC reference
    // and the front end, so it can only come from a measurement against a
    // reference source that has never been made on this station.
    //
    // AND A DERIVED FIGURE IS NOT THAT MEASUREMENT. Hl2DbReference::
    // fullScaleDbm is no longer 0.0 -- it carries +3 dBm derived from the
    // AD9866 datasheet and the HL2's own input network -- which makes the
    // zero point far better than arbitrary and still not measured.
    // isCalibrated() reports whether setFullScaleDbm has been called, and
    // nothing in src/ calls it, so this stays false and this paragraph stays
    // true.
    //
    // Read from that object rather than hardcoded false: the day a per-unit
    // fullScaleDbm is populated, the declaration follows it instead of having
    // to be remembered. The numbers remain internally consistent — a 3 dB
    // stronger signal still reads 3 dB higher — so what this denies is
    // COMPARISON: a level from this radio must not be published as a spot, held
    // against another station's report, or used as an absolute threshold.
    amplitude.calibratedDbm = m_dbRef.isCalibrated();
    // The bins this backend emits are ABSOLUTE dBFS computed on THIS host, so
    // they do not move when the display reference level does — which is what
    // lets the noise-floor auto-adjust converge even though this radio owns no
    // dBm scale and echoes no range command back.
    //
    // Quoting the path rather than asserting it. Hl2RxDsp::spectrumReady hands
    // over dBFS bins; the lambda wiring it shifts them by m_dbRef.offsetDb()
    // and publishes a raw float32 array (floatBytes). That shift is the shared
    // LNA reference (Hl2DbReference), NOT the display reference level — it
    // moves when the operator changes RF gain and never when m_refLevel moves,
    // which is exactly the property this field claims. From there
    // RadioModel::onBackendSpectrumFrame memcpy's the array into the QVector it
    // emits on panFeedSpectrumReady — "a straight pass-through", its own
    // comment — touching no value, and SpectrumWidget::estimateNoiseFloorDbm
    // reads those bins directly (a trimmed mean over the array; m_refLevel
    // appears nowhere in it). See PanAmplitudeModel::binsAbsolute.
    amplitude.binsAbsolute = true;
    c.panAmplitude = amplitude;

    // radioOwnsDbmScale IS DELIBERATELY NOT DECLARED HERE, and the reason is a
    // measurement rather than caution.
    //
    // The flag is wrong for this radio -- there is no command plane to send a
    // display range to and nothing to echo one back, so declaring it true was
    // always a claim about hardware this backend does not have. But it is ONE
    // flag answering TWO questions, and on the HL2 the answers differ:
    //
    //   1. can the radio be commanded a dBm range?          no
    //   2. does the auto-floor loop's MEASUREMENT depend
    //      on such a command having been accepted?          no, also
    //
    // Setting the flag false answers 1 correctly and answers 2 wrongly, because
    // SpectrumWidget::applyNoiseFloorAutoAdjust's early return keys on it and
    // turns the local auto-floor off. Bench run d101, on this radio: the loop
    // SETTLES. Quiescent it moved 0.307 dB in 74 s and 0.0000 dB/s over the
    // second half; stepped 12 dB of LNA it moved 5.99 dB, re-settled within
    // ~30 s and went flat again. With the flag declared false the reference
    // level sat pinned at -40.000 dBm for 222 s while the measured floor moved
    // 2 dB. So the declaration would remove a loop that demonstrably works.
    //
    // Splitting the flag is the fix, and this change is that split: question 2
    // now has its own field, PanAmplitudeModel::binsAbsolute, declared true
    // above. Answering question 1 correctly is therefore SAFE from here on --
    // noiseFloorAutoAdjustAllowed() is an OR and the second term holds the gate
    // open -- but it is a claim about the command plane, not about the floor,
    // so it gets its own change with its own reasoning rather than riding in on
    // this one.
    // The AD9866 samples at 76.8 MHz, so the first Nyquist zone — everything
    // this receiver can hear without relying on an alias — is DC to 38.4 MHz
    // (oracle §7, which is also why the wideband bandscope spans exactly that).
    // The low end is 100 kHz rather than 0: below that the input transformer
    // rolls off and there is nothing to hear.
    c.tuningMinHz = 100'000.0;
    c.tuningMaxHz = 38'400'000.0;
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine,
                               100'000, 38'400'000};
    // THE MODES THE HEADLESS RECEIVE PATH MAY BE ASKED FOR, and it is an ACCEPT
    // list rather than a menu -- ModelReceiveControlTarget reads it twice.
    //
    //   * ModelReceiveControlTarget::setMode refuses a REQUESTED mode that is
    //     not in it, and
    //   * ModelReceiveControlTarget::checkSlice, on ReceiveOperation::Mode,
    //     requires the slice's currently OBSERVED mode to be in it.
    //
    // The second reading is why the omissions bit harder than a missing menu
    // entry would, and it is SELF-LATCHING: checkSlice is the first statement
    // of setMode, so a slice whose observed mode is absent from this list has
    // slice.setMode refused with "capability.unavailable" WHATEVER mode is
    // requested -- including a mode that is on the list. It cannot be steered
    // back out over the control plane at all.
    //
    // (What it does NOT do is retract the verb from the advertised method set:
    // ModelReceiveControlTarget::available is an any-slice OR over checkSlice,
    // so with one healthy slice present slice.setMode still advertises as
    // available while being refused for the stranded one. That is worse than a
    // clean retraction, not better.)
    //
    // DSB and CWL are modes this backend genuinely demodulates --
    // modeFromString() maps each onto its own WdspChannel::Mode (Dsb, Cwl) and
    // defaultPassbandForMode() carries an entry written for each ({-3000,3000}
    // and {-250,250}) -- and both are on publishedModeStrings(), so the mode
    // MENU offers them. An operator could pick DSB out of the combo and strand
    // the slice.
    //
    // NO ALIAS SPELLING IS ON THIS LIST, AND THAT IS LOAD-BEARING RATHER THAN
    // TIDINESS. An earlier revision of this change added "CWU" here, reasoning
    // that setSliceMode() could put that spelling on a slice and the observed
    // read would then strand it. The right fix was the other one: setSliceMode()
    // below now runs canonicalOfferedMode(), so "CWU" collapses onto "CW"
    // BEFORE a slice holds it, and applyRestoredState() has always done the
    // same. With both closed, every writer of Receiver::mode produces a
    // canonical spelling and no slice can be OBSERVED in an alias at all.
    //
    // AND AN ALIAS LEFT ON THE LIST WOULD THEN LATCH THE SLICE, which is the
    // fault this whole declaration exists to remove, arriving by the other
    // door. ModelReceiveControlTarget::setMode records the REQUESTED string
    // (`m_pendingModes.insert(slice, mode)`) and releases it only on an
    // observation that compares EQUAL to it. Ask for "CWU", get "CW"
    // published, and that entry never clears -- after which checkSlice()
    // refuses every further Mode AND Filter intent on the slice with
    // "request.conflict" until the radio disconnects or the backend is
    // rebuilt. So an alias on this list is not a harmless extra entry once the
    // backend canonicalises; it is a permanent wedge.
    //
    // THE INVARIANT THAT KEEPS IT SHUT, asserted in control_receive_test
    // rather than left here: every mode on this list is its own canonical
    // spelling -- canonicalOfferedMode(m) == m for all of them. That is
    // exactly the condition under which the requested string and the published
    // one cannot disagree.
    //
    // It also happens to make this list equal to publishedModeStrings(), which
    // is the right shape for a different reason given below, and the two are
    // still separate questions: this one asks "may the receive control plane
    // be asked for it", the menu asks "should an operator be able to pick it".
    //
    // DSB BEING ON receiveOnlyModes IS NOT A CONTRADICTION: that list answers
    // "may this radio KEY in this mode", this one answers "may the receive
    // control plane be asked for it". A receive-only mode is precisely a mode a
    // receiver may sit in.
    //
    // THE TEST APPLIED HERE is "does this backend SERVE the mode", not "does
    // modeFromString() have a line for it". DSB and CWL pass it on the same
    // terms as the seven already listed: a WdspChannel::Mode of their own, a
    // defaultPassbandForMode() entry written for them, and nothing about the
    // chain left at a value nobody chose. CWU passes on a narrower ground and
    // it is worth being exact about it -- it is CW's second spelling, sharing
    // both the WDSP mode and the passband entry, and it earns a place here only
    // because the run-time paths above can put that spelling on a slice.
    //
    // FM IS DECLARED, AND ON A DIFFERENT GROUND FROM THE OTHERS.
    // It does NOT pass the "does this backend serve the mode" test above --
    // the demodulator reservations below are all still true -- and it is
    // here anyway, because this list's SECOND reading makes exclusion the
    // more dangerous answer. publishedModeStrings() carries "FM", so
    // SliceDelta::modeList publishes it and the mode MENU offers it. The set
    // difference between the menu and this list was exactly {FM}: pick FM out
    // of the combo and the slice is stranded, in the same self-latching way
    // DSB and CWL were, with no way back out over the control plane.
    //
    // A mode the radio's own menu puts on a slice must not latch the control
    // plane shut. That is the ground: not "the HL2 serves FM well", but "the
    // HL2 already lets an operator sit in FM, so the receive control plane
    // must be able to steer them out of it".
    //
    // "NFM" IS NOT LISTED, and deliberately so. It is FM's alias, it is not on
    // publishedModeStrings(), and setSliceMode() below collapses it onto "FM"
    // before a slice holds it -- so there is no observation to rescue, and
    // listing it would create the permanent "request.conflict" wedge described
    // in the alias paragraph above. receiveOnlyModes is the list that still
    // needs both spellings, and for its own reason: that one is a membership
    // test run on whatever string the slice happens to hold, and it must stay
    // correct even if a future change reopens a route this one closes.
    //
    // NOTE WHAT THIS ALSO WIDENS, because the list is read twice and this is
    // the other read: slice.setMode mode="FM" is now accepted where it was
    // refused "request.out_of_range". KEYING IS UNAFFECTED, and
    // that is checked rather than assumed -- receiveOnlyModes below carries
    // FM and NFM, RadioCapabilities::modeIsReceiveOnly() is a
    // case-insensitive membership test on exactly that list, and
    // RadioModel::refuseKeyInReceiveOnlyMode() runs it inside
    // beginTxActivity() (plus forwardNonFlexCwKeying() for the CW element
    // path), which is the common entry for MOX, TUNE, ATU and CWX alike.
    // receiveModeControl reaches ModelReceiveControlTarget and
    // RadioResourceAdapter and nothing on the transmit side at all.
    //
    // THE DEMODULATOR RESERVATIONS STAND, and declaring the mode does not
    // answer them -- it only stops the answer being "the slice is stuck":
    //
    //   * FM/NFM reach WDSP's fmd, but nothing in this tree ever calls
    //     SetRXAFMDeviation, so fmd runs on create_rxa's 5 kHz default whatever
    //     the signal is; SetRXAMode(FM) also clears the AGC, and there is no
    //     squelch on this backend at all (setSliceSquelch is not overridden
    //     here). "Demodulates" is true and "is served well" is not.
    //
    // WBFM, WFM AND DRM STAY OFF, and each fails differently:
    //
    //   * WBFM/WFM additionally clear nbp0 and panel in SetRXAMode, which is
    //     the stage carrying the sideband selection and the manual notches --
    //     and broadcast FM is outside the first Nyquist zone this radio can
    //     hear (tuningMaxHz above is 38.4 MHz).
    //   * DRM has no decoder here at all.
    //
    // Neither of those three is on publishedModeStrings(), so neither can be
    // reached from the menu and neither has the stranding exposure FM had.
    // Declaring them is a real question and it is not this one; it wants the
    // deviation, squelch and AGC work behind it rather than a list entry that
    // makes the gap harder to see.
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
    // THE RADIO'S POWER CLASS, which is what every forward-power gauge scales
    // its arc from. Declared as a band table because that is the seam the
    // clients already read: RadioModel::refreshTxPowerLimit turns it into
    // TransmitModel::maxPowerLevel, and TxApplet additionally treats a
    // non-empty table as permission to draw a face other than the 100 W one
    // (m_forwardPowerScaleFollowsBandRating).
    //
    // WITHOUT IT, TransmitModel kept its compiled-in 100 W default and every
    // gauge scaled for a 100 W radio: a full-power HL2 transmission sat in the
    // bottom few percent of the arc, which is indistinguishable from a meter
    // that does not work — and is what it has been reported as.
    //
    // ONE BAND, spanning the whole tuning range, because that is the truth
    // about this radio rather than a simplification: "The Hermes-Lite 2.0 is a
    // QRP transceiver and achieves 5W out on ALL HF amateur radio bands" (HL2
    // wiki, FAQ). Unlike the IC-9700, whose three decks each have their own
    // ceiling, there is no per-band variation to describe.
    //
    // The secondary instrumentation output at RF1 is +17 dBm and is
    // deliberately NOT what this describes: it is selected in hardware with no
    // register to read back, so scaling for it would be wrong for every
    // operator using the normal output.
    c.txPowerBands = {TxPowerBand{c.tuningMinHz, c.tuningMaxHz,
                                  static_cast<double>(kHl2RatedOutputWatts)}};
    // Reported from the gate, not hardcoded: the engine's TX guard keys off this,
    // so a build with transmit disabled must look RX-only from above the seam.
    c.canTransmit = m_txAllowed;
    // THE MODES THIS RADIO DEMODULATES AND CANNOT MODULATE.
    //
    // The comment that stood here said the HL2 "transmits in whatever mode WDSP
    // is told to build — there is no mode it receives and cannot send", and left
    // the list empty on that basis. **The transmit chain is not WDSP.**
    // Hl2TxDsp is a hand-written phasing SSB modulator: setMode() stores the
    // mode and the only reader is isLowerSideband(), which returns true for Lsb,
    // Cwl and Digl and false for everything else. So AM, SAM, DSB, FM, NFM, WBFM
    // and DRM all take the upper-sideband branch and go on the air as SSB,
    // announcing nothing.
    //
    // WHAT STAYS OFF THE LIST, deliberately:
    //
    //   * USB / LSB / DIGU / DIGL are the SSB family and modulate correctly.
    //   * CW / CWU / CWL keys a carrier the GATEWARE shapes at the TX NCO
    //     (MetisClient::setCwKeyDown). That path never reaches Hl2TxDsp, so the
    //     sideband switch above does not apply to it and CW transmits correctly.
    //
    // These strings are the neutral vocabulary SliceModel carries, and both
    // spellings of each mode appear because modeFromString() accepts both:
    // refuseKeyInReceiveOnlyMode() compares what the slice holds, not what this
    // backend would have mapped it to, so listing only one spelling would leave
    // the other keying.
    //
    // THIS DECLARATION ALSO WITHDRAWS TUNE IN THESE MODES. Say so here rather
    // than let an operator discover it.
    //
    // refuseKeyInReceiveOnlyMode() is not a MOX-and-CW guard.
    // RadioModel::beginLocalTxActivity() runs it for EVERY TxActivity, ahead of
    // the per-activity capability checks, so TxActivity::Tune is refused too —
    // and that one is a real loss, not a theoretical one. setTune() below raises
    // the carrier from the GATEWARE test-tone generator at zero offset
    // (MetisClient::setTxTestTone), a path that never reaches Hl2TxDsp, exactly
    // like the CW keyer exempted above. This radio could put a clean tune
    // carrier on the air with the TX slice in AM or FM; after this list it will
    // not, and the operator is told "Choose a transmit mode first" and has to
    // move the slice to a mode that transmits. (TxActivity::Atu was already
    // refused here for want of hasTuner, so the plain TUNE button is the only
    // behaviour this changes.)
    //
    // ACCEPTED, deliberately, on two grounds:
    //
    //   * It is what this capability already MEANS. The IC-705 declares WFM
    //     receive-only (#5040) and is refused on this same guard, with a second
    //     wire backstop in IcomCivBackend::refuseKeyingInReceiveOnlyMode() that
    //     its setTune() converges on through setKeying() — "shared by every path
    //     here that can start an emission", in its own words. HL2 is inheriting
    //     a settled contract, not inventing one.
    //   * receiveOnlyModes is ONE list of mode names with no per-activity
    //     granularity, so exempting tune is not expressible from a backend at
    //     all: it would mean changing RadioModel above the family seam, for
    //     every family at once. That is a maintainer's call.
    //
    // The CW/tune asymmetry is in the SHAPE of the list, not in the reasoning
    // behind it: CW stays off because CW is a MODE this radio transmits
    // correctly, and tune is an ACTIVITY, which a list of mode names has no
    // vocabulary for.
    //
    // What the list itself reports is only what the modulator does today. When a
    // mode genuinely transmits — the WDSP TXA chain carries all of these — its
    // entry comes back off this list and the tune refusal lifts with it.
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
    // The wideband converter view (docs/HERMES.md §13 item 18). Declared rather
    // than assumed by a consumer reading the family string: an applet that
    // tests `family == "hl2"` is the shape §"For coding agents" forbids, and
    // this record is the exception that section names.
    //
    // Declared ONLY while connected, and the reason is not caution. The verb
    // behind it raises the run byte's wide_spectrum bit at a radio that is
    // already streaming; with no stream there is nothing to raise it against
    // and MetisClient refuses. A capability offered then would be a control the
    // operator could press to no effect.
    //
    // The numbers are the converter's, not a display choice: 76.8 MHz is
    // hermeslite_core.v's CLK_FREQ, so the view spans DC..38.4 MHz, and 2048 is
    // the depth of the capture FIFO the gateware drains four datagrams at a
    // time.
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
    c.alwaysUseClientSideSpots = false;
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

    // A connect arriving while the previous one's chains are still opening.
    // Reachable now that the build spans event-loop turns: the reconnect timer
    // or an operator picking a different radio both land here.
    //
    // It cannot be served inline. buildReceivers() would destroy the very
    // chains the I/O thread is opening, and its publishIoDsps() is a BLOCKING
    // hop into an I/O thread busy for the rest of that open -- which is exactly
    // the GUI stall this change removes, reintroduced through the back door.
    // So: supersede the build, hold the request, and re-drive it from
    // finishDspSetup() once the I/O thread is free.
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

    // This radio's manual frequency calibration, FIRST — before any frequency is
    // computed below, because the seed at mp.rxFrequencyHz is a commanded value
    // and would otherwise go out uncorrected. The operator would hear the first
    // moments of every session on the uncalibrated frequency and watch it jump
    // when they next touched the dial.
    //
    // Keyed by the radio's MAC: the calibration describes one physical crystal,
    // so a second HL2 must not inherit the first one's number. An empty serial
    // (a hand-built connect request with no identity) yields the family-wide
    // row, which is empty by default — i.e. uncalibrated, not someone else's.
    m_radioSerial = request.serial;
    m_freqCalPpb = Hl2FreqCal::loadPpb(
        RadioSettingsScope(QStringLiteral("hl2"), m_radioSerial));
    m_freqCalScale = Hl2FreqCal::scaleForPpb(m_freqCalPpb);
    if (m_freqCalPpb != 0)
        qCInfo(lcHl2) << "HL2: frequency calibration" << m_freqCalPpb << "ppb"
                      << "— effective clock"
                      << Hl2FreqCal::effectiveClockHz(m_freqCalPpb) << "Hz";

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

    // ---- how many receivers ----
    //
    // THREE independent limits, and the smallest wins. Each exists for its own
    // reason and none of them may be assumed:
    //
    //   1. What the operator asked for (Hl2Settings, or an explicit param).
    //   2. What the BOARD has. Discovery byte 0x13, applied inside MetisClient
    //      because it is the object that saw the reply. The shipping
    //      hl2b5up_main gateware reports 4; the skimmer variants report 9-12 and
    //      have no transmitter at all. Never hardcoded — oracle §1.
    //   3. What the LINK can carry at this sample rate. Four receivers at
    //      384 kHz is ~89 Mbit/s on 100BASE-T, which does not fail cleanly: the
    //      link drops packets, and a dropped EP6 packet is a simultaneous gap in
    //      every panadapter.
    //
    // CONNECT ALWAYS COMES UP WITH ONE. Receivers are added afterwards, by the
    // operator, through "Add Panadapter" — createPanadapter() below. That is
    // where the intent actually is, and it means a radio never starts spending
    // link budget and WDSP channels on receivers nobody asked for.
    //
    // The persisted count is gone as the way in. It required editing settings to
    // get a second receiver, made the connect path the only place the count
    // could change, and meant a saved 4 would be re-imposed on every connect
    // even at a span that could not carry it. `numRx` survives ONLY as an
    // explicit connect param, for automation that wants a known starting state.
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
        // ASK THE RADIO. Connecting by IP skips the broadcast sweep, so nothing
        // has read discovery byte 0x13 and the board's receiver count is
        // unknown — which left the ceiling at whatever the register can encode
        // (7) on a board that has 4. Sending a count the gateware does not have
        // makes it stream slots with no DDC behind them: correctly framed,
        // correctly paced, all-zero IQ on the receivers that do not exist.
        //
        // A UNICAST discovery to the host we are about to connect to answers it
        // in one round trip, using the same parser the broadcast sweep uses.
        // Short timeout: this is on the connect path, and a board that does not
        // answer just leaves us with the conservative default below.
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
    // The filter board has to be right from the FIRST config frame, not from
    // the operator's first band change. m_rxFreqHz here is the persisted
    // frequency this session is coming up on, so a launch straight onto 40 m
    // starts with the 40 m low-pass engaged rather than with whatever the last
    // session left in the relays — the radio never reports its own state, so
    // "unchanged since last time" is indistinguishable from "correct".
    // Every receiver starts on the same frequency, so at connect there is no
    // band spanning yet and this is simply that frequency's filter. It becomes a
    // spanning decision the moment the operator moves one of them; see
    // applyBandFilter().
    mp.ocFilterByte = ocFilterByteForHz(startFreqHz);
    m_ocFilterByte = static_cast<int>(mp.ocFilterByte);
    qCInfo(lcHl2).nospace()
        << "HL2 band filter: " << QString::asprintf("0x%02X", m_ocFilterByte)
        << " (" << ocFilterName(mp.ocFilterByte) << ") for "
        << QString::number(startFreqHz / 1.0e6, 'f', 6) << " MHz, trigger=connect";
    // Seed the reference from the gain we are about to command, so the very
    // first spectrum frame is already on the same footing as every later one.
    m_dbRef.setLnaGainDb(lnaEffectiveDb());

    // ---- build the receivers, BEFORE start() ----
    //
    // ORDER IS LOAD-BEARING, and the reason is the same one that governs every
    // other ordering decision in this backend: EP2 must not stop.
    //
    // Opening a WDSP channel is slow -- ~19 s on the FIRST open this machine
    // ever does, generating FFTW wisdom, and 40-175 ms for every open after
    // that, at any rate (docs/HERMES.md §10 and §22.3) -- and it runs ON THE I/O
    // THREAD, which is the thread that paces EP2. Configuring after start()
    // therefore stalls the pacer for the whole of that, and the gateware
    // watchdog halts the stream when EP2 stops arriving. It also stalls the EP6
    // reader, so the connect watchdog can time out against a radio that is
    // answering perfectly well.
    //
    // The count comes from the STATIC effectiveNumRx(mp), which answers from the
    // params alone. Same clamp the running client will apply to the same struct,
    // so the DEMUX and the radio cannot disagree about how many receivers exist
    // -- and that matters more than it looks, because the EP6 payload carries no
    // receiver-count field: a host decoding for four while the radio sends three
    // misreads every round with nothing reporting an error.
    const int actualNumRx = MetisClient::effectiveNumRx(mp);
    mp.numRx = actualNumRx;

    buildReceivers(actualNumRx);
    for (Receiver& r : m_rx) {
        r.sliceFreqHz = startFreqHz;
        r.ncoHz = startFreqHz;
    }

    // The remembered AGC pair (#4909), onto the receivers that now exist. THIS
    // IS THE ONLY PLACE THE RECEIVERS ARE SEEDED — see applyRestoredState(),
    // which resets the capture side only, and the definition of
    // seedReceiverAgc() for why the split. The channels are OPEN but not yet
    // CONFIGURED — configure() runs in beginDspSetup()'s lambda below, after
    // this — so this settles the STATE and beginDspSetup()/pushInitialState()
    // carry it into the DSP.
    //
    // TWO CONDITIONS, and each covers a case the other does not.
    //
    // A DIFFERENT RADIO must be seeded, or radio A's AGC keeps running under
    // radio B's identity: buildReceivers() deliberately carries receiver state
    // across a rebuild, so a same-family swap inherits it (the Ozy311 leak,
    // PR #4619 review). Keyed on the connect request's SERIAL, which is the
    // identity the restored document was loaded under.
    //
    // RECEIVERS WITH NO CARRIED STATE must be seeded whatever the serial says.
    // tearDownReceivers() clears m_rx on a superseded connect and on a failed
    // socket bind, and the m_queuedConnect re-drive below calls connectRadio()
    // straight back with no applyRestoredState() in front of it — so without
    // this the rebuild would come up on Receiver{}'s med/65 and the restored
    // AGC would be silently lost for the session.
    //
    // What is deliberately NOT seeded is an auto-reconnect to the SAME radio
    // whose receivers survived: buildReceivers() preserved their live
    // per-receiver AGC, and pushInitialState()'s restore block touches only
    // rx(m_txDdc), so re-seeding there overwrote RX2's live setting with the
    // flat remembered pair while its mode and passband survived — a
    // within-session loss on the very path the sibling mode/passband restore
    // engineers around. Flat MEMORY across a restart is the design; flattening
    // live receivers mid-session is not.
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

    // Announce the passband the modulator is being configured with. The Config
    // is how the modulator learns it, so this is the echo half only — but it is
    // the half the operator sees. A restored eSSB 100..4000 reached the
    // modulator and nothing else: the Phone applet went on showing
    // TransmitModel's own construction default until the operator happened to
    // press a cut button, which is a passband readout that disagrees with the
    // transmitter.
    //
    // Emitted HERE rather than after the open, and that is deliberate now that
    // the open is asynchronous. The value is already decided — waiting for the
    // channel to finish opening would tell the operator nothing more, and would
    // make an echo that connectRadio() used to deliver synchronously arrive tens
    // of seconds later on a cold cache. What still holds is the ordering that
    // matters: this precedes linkUp either way.
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
    // WHETHER THE TRANSMIT CHAIN IS A STEP DEPENDS ON THE BUILD, because what
    // Hl2TxDsp::configure() does depends on the build.
    //
    // In the PHASING build it designs two FIR kernels and returns — it opens no
    // WDSP channel and measures no FFTW plan. Counting it inflated the
    // denominator with a step that completes in microseconds and put a
    // "Preparing the transmit chain…" label on screen for work that was
    // already over.
    //
    // In the TXA build it opens a WDSP TRANSMIT CHANNEL, which is FFTW
    // planning and is the most expensive single thing in the connect on a cold
    // cache. Leaving it uncounted there is the opposite error: the receive
    // label would sit at "2 of 2" for a second or more with the dialog
    // apparently finished and the radio not yet ready.
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
    // QPointer, not `this`: the build outlives the call, and a backend
    // destroyed mid-build (a family switch, app teardown) must not be resumed
    // through a dangling pointer.
    //
    // What makes the cross-thread reads of it SOUND is the destructor, not the
    // QPointer: ~Hl2Backend() blocks on the I/O thread (a BlockingQueuedConnection
    // stop, then quit()/wait()), so this lambda has always finished before the
    // object can go away, and every `if (self)` below is a check on a pointer
    // nothing is racing to clear. QPointer is reentrant, NOT thread-safe — the day
    // teardown stops blocking, a check-then-use here becomes a real race and this
    // needs a different mechanism. That blocking teardown has its own cost; see
    // the note in ~Hl2Backend() and docs/HERMES.md §22.4.
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

    // INVALIDATE, DO NOT TEAR DOWN. The I/O thread may be inside configure() on
    // these very chains — WDSP's OpenChannel does not return early, so the build
    // cannot be cancelled — and freeing them from this thread would be a
    // use-after-free. So do exactly what disconnectRadio() does: bump the
    // generation and LEAVE m_pendingConnect SET. The build runs to completion,
    // finishDspSetup() takes its stale branch, and that branch is what releases
    // the chains and re-drives anything queued behind them.
    //
    // Resetting pending here instead would disable the very mechanism this
    // comment relies on: finishDspSetup() would return at its
    // `if (!m_pendingConnect)` guard, tearDownReceivers() would never run, and
    // every timed-out connect would leak its WDSP channels out of the 32-slot
    // pool. Worse, connectRadio() queues behind an in-flight build only while
    // m_pendingConnect is set, so a retry after this error would call
    // buildReceivers() on the chains the I/O thread is still opening — a
    // use-after-free reached by the ordinary "connect failed, try again"
    // gesture. (#5413 triage; #5415 review.)
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
                emit connectionError(
                    QStringLiteral("HL2 DSP: %1").arg(QString::fromStdString(err)));
                emit dspSetupFinished();
                return;
            }
            qCWarning(lcHl2) << "HL2: receiver" << i << "DSP failed —"
                             << QString::fromStdString(err)
                             << "; running" << i << "receiver(s)";
            // WITHDRAW, THEN DESTROY — the same order as removePanadapter() and
            // releaseReceiverDsps(), and for the same reason. buildReceivers()
            // has already published all `actualNumRx` chains to the sample path,
            // so a raw delete here left m_ioDsps holding freed pointers; the wire
            // then started with the untrimmed numRx and the first EP6 packet
            // called processIqBlock() on them, on the I/O thread.
            //
            // deleteLater(), not delete: an Hl2RxDsp owns a WDSP channel and an
            // FFTW plan and lives on the I/O thread, which is the only thread
            // allowed to close them. Every other teardown in this file does this;
            // the one raw delete that remains (in the destructor) is justified
            // there by the thread already being joined.
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
    if (std::abs(hz - r->ncoHz) > usableHz) {
        r->ncoHz = hz;
        if (m_metis)
            // THIS receiver's NCO register, not RX1's. The two-argument overload
            // is the whole reason the receivers can sit on different bands.
            QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
                Q_ARG(int, ddc),
                Q_ARG(std::uint32_t, ncoCommandHz(hz)));
        // Notch centres are measured from the NCO, so moving it without saying
        // so leaves every notch parked at its old RF frequency — the operator
        // tunes across the band and the notches follow them, which is precisely
        // the behaviour a TRACKING notch exists to avoid.
        pushNotchTune(*r);
    }

    // Shift by the slice's offset from the NCO, with the SAME sign.
    //
    // Derivable, now that the handedness is settled: the wire puts a signal at
    // frequency F at -(F - NCO), so mapping the slice's own frequency to
    // baseband needs -(slice - NCO) + shift == 0, i.e. shift = slice - NCO.
    // hl2_shift_test measures exactly that. (This sign is unchanged — it was
    // right all along; what was wrong was the conjugation in Hl2RxDsp, which is
    // why the stage looked correct only in LSB.)
    //
    // rxShiftHz(), not dspShiftHz(): in CW the detector's zero is a PITCH away
    // from the slice, not on it. See the CW BFO note in the header.
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, rxShiftHz(*r)));

    // The TX NCO is a SEPARATE register (addr 0x01) from the RX DDC and does not
    // follow the receiver. Without this the transmit oscillator keeps whatever
    // it last held — zero on a fresh boot — so keying would radiate at the wrong
    // frequency, or at DC, with nothing to indicate anything was wrong.
    //
    // The HL2 has ONE transmitter however many receivers it runs, so the TX NCO
    // follows the receiver that owns transmit — not whichever slice was tuned
    // last. Tuning a second receiver while keyed-up on the first must not drag
    // the transmit frequency with it, which is exactly what an unconditional
    // setTxFrequency(hz) here would do.
    //
    // Sent whether or not transmit is enabled: this is oscillator setup, it keys
    // nothing, and having it already correct is part of what makes the key safe.
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

    // THE ALIAS COLLAPSES HERE, not only at the restore boundary.
    //
    // applyRestoredState() has always run canonicalOfferedMode() before the
    // mode reaches a slice, and Hl2ModeVocabulary.h spells out why: both
    // consumers rebuild the mode combo with clear()/addItems()/findText() and
    // move the selection ONLY on a findText() hit, so a slice holding a
    // spelling publishedModeStrings() does not carry leaves the combo on index
    // 0 -- "LSB" -- with signals blocked while the receiver really is in
    // another mode. "An operator reading LSB while hearing FM is the exact
    // fault #5580 exists to remove."
    //
    // That header names this function as one of the three run-time routes that
    // could still do it -- "CAT, TCI and Hl2Backend::setSliceMode still put
    // either spelling on the slice at run time" -- and it was a description of
    // what happened, not a requirement. Declaring CWU (and FM/NFM) on
    // receiveModeControl above turns that description into a wider hole: the
    // list is read for the REQUESTED mode too, so slice.setMode mode="CWU" now
    // passes ModelReceiveControlTarget and arrives here, where the old code
    // stored it verbatim. Canonicalising is what keeps the request surface
    // from widening to a spelling the menu cannot display.
    //
    // IT CANNOT WEAKEN THE KEY REFUSAL, which is the one property
    // Hl2ModeVocabulary.h asks of any collapse. modeIsReceiveOnly() is a
    // case-insensitive membership test and capabilities()'s receiveOnlyModes
    // lists every alias pair BOTH ways (FM and NFM, WBFM and WFM) or on
    // NEITHER (CW and CWU, both of which key correctly through the gateware
    // keyer), so each pair's two spellings are equivalent across that boundary
    // by construction -- hl2_mode_vocabulary_test pins that equivalence for
    // every accepted spelling. Collapsing one onto the other moves nothing.
    //
    // isKnownModeString() FIRST, exactly as applyRestoredState() does it: a
    // string the vocabulary does not know is passed through untouched rather
    // than merely upper-cased, so this changes nothing for anything outside
    // the three alias pairs.
    const QString mode = isKnownModeString(requested) ? canonicalOfferedMode(requested) : requested;

    const QString previous = r->mode;
    r->mode = mode;
    const WdspChannel::Mode wdsp = modeFromString(mode);

    // The passband belongs to the mode. A radio that owns its own DSP echoes a
    // mode-appropriate filter back on every mode change and heals this for
    // free; we own the DSP, so nothing heals it and the previous mode's
    // passband simply stays. Selecting DIGU out of CW left a ~200 Hz filter on
    // the mode WSJT-X uses, which decodes nothing -- and the operator sees a
    // mode that changed, so the filter is the last thing they suspect.
    //
    // Adopted on CHANGE only, so an operator's own filter edit survives until
    // they change mode again (oracle addendum 2 §B3: "All clients tie default
    // filter width to mode, with user overrides").
    if (!previous.isEmpty() && previous.compare(mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        r->filterLowHz  = lo;
        r->filterHighHz = hi;
    }

    // ORDER IS LOAD-BEARING: mode FIRST, then passband -- and the passband is
    // re-pushed on EVERY mode set, not only when its value changed.
    //
    // In WDSP the mode does not select the sideband; the NBP filter edges do
    // (see WdspChannel::setFilter). SetRXAMode/SetTXAMode rebuild that stage
    // from their own per-mode notion of the passband, so any filter applied
    // BEFORE the mode call is discarded by it.
    //
    // What this cost: USB<->LSB happens to flip the filter's sign, so
    // SliceModel::normalizeFilterPolarity re-pushed the passband after the mode
    // and those two were always correct. USB->DIGU does not flip the sign,
    // nothing re-pushed, and DIGU was left running on whatever sideband
    // SetRXAMode had rebuilt -- FT8 sat on the wrong side of the passband and
    // decoded nothing, while DIGL (reached via a sign flip) worked perfectly.
    // A sideband bug that reverses itself depending on which mode you came
    // from is exactly what an ordering bug looks like from the operator's seat.
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

    // The slice's AGC threshold is a 0..100 operator value (SliceModel bounds it
    // there); the WDSP ceiling is dB of MAXIMUM GAIN. The original 1:1 map was
    // measured wrong on live hardware: on WWV at 10 MHz USB, demodulated audio
    // is clean through 40 dB (peak 0.68) and clips hard by 50 dB (peak 2.01,
    // 21% of samples), so the default threshold of 65 was sitting 25 dB past
    // the clipping point and 60% of samples were saturating.
    //
    // 0..100 -> 0..60 dB puts the default of 65 at 39 dB, measured clean with a
    // healthy level, while leaving the top of the slider available for a quiet
    // band. The ceiling is a maximum, not a limiter, so a strong band can still
    // clip at a high setting — that is correct AGC-T behaviour and the reason
    // the control exists. What was wrong was the DEFAULT landing in that region.
    // VALIDATE ON THE WAY IN, so the capture side can only ever store something
    // the restore side accepts. `m` is just a trimmed lowercase copy of whatever
    // the caller passed, and a bridge or automation call with "medium" used to
    // land in r->agcMode, get echoed by emitSliceState(), and get persisted —
    // while wdspAgcMode() silently ran med and the NEXT launch dropped it via
    // isKnownAgcModeString(). The applet, the DSP, the document and the restore
    // all disagreed. An unknown string now leaves the mode where it was.
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

ReceiveDispatch Hl2Backend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    if (!request.valid() || request.feature != SliceDspRequest::Feature::Nb
        || !rx(ddcForSlice(sliceId))) {
        return ReceiveDispatch::Unsupported;
    }
    setSliceNoiseBlanker(sliceId, request.enabled, request.level);
    return ReceiveDispatch::Dispatched;
}

ReceiveDispatch Hl2Backend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (!request.valid() || request.origin != SliceAudioRequest::Origin::Operator
        || !rx(ddcForSlice(sliceId))) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case SliceAudioRequest::Field::Gain: setSliceAudioGain(sliceId, request.value); break;
    case SliceAudioRequest::Field::Mute: setSliceAudioMute(sliceId, request.value != 0); break;
    case SliceAudioRequest::Field::Pan: setSliceAudioPan(sliceId, request.value); break;
    }
    return ReceiveDispatch::Dispatched;
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

    // Republish BOTH slices: the one that lost transmit and the one that gained
    // it. Publishing only the new one would leave the old indicator lit, and two
    // slices claiming transmit is worse than none — the interlock would find
    // whichever was selected.
    emitSliceState(previous);
    emitSliceState(ddc);
}

// ── Manual notch filters ────────────────────────────────────────────────────
//
// The HL2 has no DSP of its own, so these are WDSP notches running on this
// host — but from above the seam they behave exactly like a Flex TNF: placed at
// an absolute RF frequency, and they stay on the interferer while the operator
// tunes. That equivalence is the whole point of doing it here rather than
// inventing a second, HL2-shaped notch concept.
//
// Every mutation is applied to EVERY receiver. A notch is a fact about the
// band, not about one slice, and a second receiver looking at the same carrier
// should not still hear it.

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
    // The TRUE NCO frequency, not ncoCommandHz(). Deliberate, and worth the
    // arithmetic because the obvious "fix" is worse.
    //
    // WDSP takes a notch's baseband position as fcenter - (tunefreq + shift),
    // and the shift is in the COMMANDED domain (dspShiftHz is
    // sliceTrue * scale - ncoCommand). Notch centres, meanwhile, arrive from the
    // panadapter in TRUE RF Hz. Mixing the two leaves a residual of
    // e * (fcenter - ncoTrue), where e is the calibration error: at the ±50 ppm
    // clamp and the far edge of a 384 kHz span that is under 10 Hz, and on a
    // real crystal it is a fraction of a Hz — against a notch floor of 50 Hz.
    //
    // Handing WDSP ncoCommandHz() instead would make the offset sliceTrue*scale
    // and leave a residual of e * fcenter — the full dial frequency rather than
    // the offset from it, so ~7 Hz at 7 MHz and 50 ppm instead of a fraction of
    // one. Getting it exactly right means scaling the notch centres too, which
    // buys a correction smaller than the notch is wide.
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

void Hl2Backend::seedNotches(const Receiver& r)
{
    if (!r.dsp)
        return;
    // Tune frequency before the notches: the centres are absolute, so a notch
    // placed against a tune frequency of zero lands ~7 MHz away from where it
    // was asked for.
    pushNotchTune(r);
    // REPLACE, never append. pushInitialState() calls this on every linkUp, and
    // MetisClient re-emits linkUp after an EP6 silence timeout without any new
    // connectRadio() — the receivers and their Hl2RxDsp objects survive that,
    // so a seed that only added would give every notch a second copy in WDSP
    // while m_notches still held one. The positional index the whole stable-id
    // mapping rests on would then address the wrong notch, and the duplicate
    // would keep notching a frequency with no UI entry to remove it. Clearing
    // first makes seeding idempotent wherever it is called from, which is the
    // property this path needs — a once-per-connect guard would fix the linkUp
    // case and leave the non-idempotency one refactor away from returning.
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

    // ── Three threads, and each one is there for a reason ─────────────────
    //
    // GUI THREAD (here): snapshot every per-receiver Config. m_rx is
    // GUI-thread-only, so the decisions that need it are all made now and
    // copied; nothing downstream reads the container.
    //
    // BUILD THREAD (m_dspBuildThread): construct a COMPLETE set of N new
    // chains while the old set keeps running and keeps producing audio.
    // Nothing live is touched. N, not one, because the DDC rate register is
    // RADIO-WIDE — that is the one way this differs from AnanBackend, which
    // has a single DDC and therefore a single chain to rebuild.
    //
    // I/O THREAD (m_ioThread, reached through m_metis): if all N built, swap
    // them in and write the rate register, in one turn. The swap is pointer
    // writes.
    //
    // WHY NOT THE I/O THREAD FOR THE BUILD, which is where the previous commit
    // left it. That thread is not one receiver's — it is the wire's AND every
    // receiver's. MetisClient paces EP2 from a 2 ms timer on it and drains EP6
    // on it, and iqBlocksReady is a Qt::DirectConnection that runs
    // Hl2RxDsp::processIqBlock() inline on it. So a rebuild task holding that
    // thread starves every receiver's audio and stops EP2, and
    // docs/HERMES.md §20.8 is explicit that the gateware watchdog halts the
    // stream when EP2 stops arriving. Moving the wait off the GUI thread kept
    // the WINDOW alive during a zoom; only moving the WORK off the I/O thread
    // keeps the RADIO alive.
    //
    // WHY THERE IS NO ROLL-BACK, which the two previous notes here each said
    // was the hard part. There is nothing to roll back. Until every one of the
    // N builds has succeeded, the rate register is unwritten and no published
    // chain has been touched — so the failure path is "destroy the new set and
    // return", and the radio and every receiver are exactly as they were. The
    // roll-back was only ever needed because the old code changed live state
    // before it knew whether it could.
    //
    // WHAT THIS DOES NOT CLAIM. Nothing here is measured. The 0.6-1.1 s figure
    // is docs/HERMES.md §22.4's, from the blocking-on-the-GUI-thread era, and
    // whether the audio actually stays clean across a crossing needs a radio,
    // four panadapters and a zoom drag.
    struct RebuildStep {
        // QPointer, not a raw Hl2RxDsp*: a receiver can be closed while a
        // build is in flight, and tearDownReceivers() retires these through
        // deleteLater() posted to the I/O thread. Both the clear and every
        // check below therefore happen ON the I/O thread, which is what makes
        // a QPointer — reentrant, not thread-safe — sound here. The BUILD
        // thread never CHECKS these: buildChannel() is static and takes only a
        // Config, which is the entire reason it is static. It does hold them —
        // `steps` is captured by value, so a copy of every QPointer is
        // constructed and destroyed on the build thread — and that is safe for
        // the narrower reason that QWeakPointer's refcount is atomic. It is the
        // isNull() check behind `if (st.dsp)` that is not thread-safe, and every
        // one of those stays on the I/O thread.
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
        // ---- I/O THREAD, turn 1: mark, snapshot, hand off ----
        //
        // beginRebuild() FIRST, on each chain's own thread, so a setMode/
        // setFilter/setAgc/setShift/notch/NB call arriving while the build runs
        // updates that chain's mirrors instead of blocking this thread on
        // WDSP's process-wide setup mutex, which the build is about to hold.
        // Hl2RxDsp::installRebuiltChannel() re-applies every one of them at the
        // swap. Nothing the operator asks for during a zoom is lost.
        //
        // The noise-blanker pair is snapshotted here because the channel is
        // OPENED with it (Hl2RxDsp::buildChannel()'s own note) and it is
        // deliberately not part of Config.
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

// The GUI-thread half of a rate change. Everything here used to run inline in
// applyPanBandwidth(); it is separated only so the build and the swap between
// them can happen without the GUI thread waiting — and, since this commit,
// without the I/O thread being held either.
//
// WHAT IS NOT DONE HERE, and is worth naming because AnanBackend's equivalent
// does it: there is no MUTE across the window between the register write and
// the radio actually latching the new rate. For that one C&C round the new
// chains are fed IQ still arriving at the old rate — audible as a moment of
// wrong-pitch audio and a mis-scaled spectrum, not as an error. ANAN mutes its
// single chain across a settle timer; doing the same for N chains here would
// have to share setAudioMuted() with the transmit path, whose mute must not be
// lifted by a zoom that lands mid-transmission. Left undone deliberately rather
// than done unsafely. NOT MEASURED: how long that window actually is.
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

    // ── THE RECEIVER THIS CROSSING DID NOT KNOW ABOUT ────────────────────
    //
    // The snapshot in applyPanBandwidth() is taken on the GUI thread and then
    // the build runs for 0.6-1.1 s. A receiver opened inside that window is not
    // in `covered`, and it was deliberately built for the rate the radio was
    // still producing (see openReceiver()). The register has now moved, so that
    // chain is the one thing left decimating for a rate that no longer arrives:
    // exactly the split set the removed FOLLOW-UP comment called "the set is
    // split across two rates with nothing reporting it".
    //
    // It is rebuilt here, synchronously. That is the honest cost and it is
    // small: one chain, on the GUI thread, only when an operator managed to open
    // a receiver during a zoom — not the N-chain wait this PR exists to remove.
    // Doing it on the build thread would need a second crossing's worth of
    // machinery for a case that cannot involve more than the receivers opened in
    // one rebuild window.
    for (Receiver& r : m_rx) {
        if (!r.dsp || r.configuredRateHz == targetRate)
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
    // THE QUIET-MICROPHONE ADVICE, RESTORED, AND NOW AIMABLE.
    //
    // Making the ALC reduction-only (#5646) means the pre-ALC mic peak IS the
    // on-air level, so an operator whose gain nobody set goes out quiet with
    // nothing to say so. An unkey-time "raise mic gain" line is the right
    // instrument for that.
    //
    // IT COULD NOT BE AIMED FROM #5646, AND IT CAN BE AIMED FROM HERE.
    //
    // #5646 removed the advice rather than misaim it, and said why in as many
    // words: WSPR, AX.25 and RADE all reached submitTxAudio() with
    // `clientLeveled` false, exactly like the microphone, so a beacon at its
    // -20 dBFS default would have drawn "raise mic gain" on every unattended
    // unkey (K5PTB, #5646 review). The seam had two states and engine audio was
    // not one of them. That branch's own comment delegated the fix here:
    // "#5647 reintroduces it gated on `!m_txAudioEngineGenerated`".
    //
    // This is that reintroduction. The gate exists now because the seam carries
    // TxAudioSource, and it is the ONLY reason the advice is safe to restore.
    // The measurement never went away — m_txMicPeakMaxDbfs kept tracking and
    // kept reaching the health snapshot throughout.
    //
    // Note the gate is narrower than #5646 anticipated: AX.25 is tagged
    // Microphone, not EngineGenerated, so the advice DOES fire for a quiet
    // packet frame. That is correct. The mic slider is the only control in the
    // product that can move an AX.25 frame (kTxAfskAmplitude is a constant and
    // the packet dialog has no level control), so "raise mic gain" is exactly
    // the right instrument there. Only the WSPR beacon has no slider in its
    // path, and only the WSPR beacon is gated off.
    //
    // At unkey, once per transmission, on the main thread: the DSP worker must
    // not log per block, and a per-block test would fire on every normal
    // transmission because the pauses between words sit far below the target.
    if (m_keyed && !key) {
        // HOW FAR BELOW THE TARGET COUNTS AS QUIET. Upper bound MEASURED;
        // the value inside it CHOSEN. Both halves are stated because they have
        // different standing.
        //
        // The old -45 dBFS was a property of the hold — a real threshold in the
        // DSP that an over either cleared or did not. Nothing in the DSP
        // replaces it, so this is a judgement about the air, informed by a
        // measurement rather than derived from one.
        //
        // MEASURED (d81b-speech-pauses-alc, four legs, twelve speech bursts,
        // two mic gains 20 dB apart, on this build — against hpsdrsim on a
        // loopback approval, NOT on the air, and nothing radiated):
        //
        //   speech crest factor            18.87 dB, sd 0.80
        //   burst-to-burst level spread    <= 0.91 dB
        //   mic slider 50 (unity)          peak 19.56-19.67 dB BELOW
        //                                  alcTargetPeak
        //
        // The unity figure is read off d81b's own result.json rather than off a
        // summary of it: speech_output_dbfs is -21.08 dBFS on the fault leg and
        // -20.97 dBFS on the control leg, both at mic_level 50, against
        // 20*log10(0.85) = -1.4116 dBFS.
        //
        // THE UPPER BOUND IS ~19.56 dB AND IT IS HARD. Unity is where an operator
        // who has never moved the slider sits, and on this build that is about
        // 19.6 dB short of the target — precisely the operator this diagnostic
        // exists to reach. A margin at or above 19.56 would stay SILENT on them.
        // The earlier placeholder here was 20.0 dB, so it was outside its own
        // bound and would have failed in the one case it was written for. That
        // is why a guess with a name is still a guess.
        //
        // CHOSEN, 12.0 dB: it fires on unity with 7.6 dB to spare, it does not
        // fire until a station is two S-units down — weak on the air, not merely
        // conservatively set — and it clears the measured noise (0.91 dB of
        // burst variation, 0.80 dB of crest scatter) by an order of magnitude.
        //
        // WHAT WOULD MAKE THIS MEASURED RATHER THAN BOUNDED. d81b bounds the
        // correct setting from ONE side only. There is no bracket: d81b's
        // slider-100 legs run a DIFFERENT stimulus (sp-53.wav) from the unity
        // legs (sp-38.wav, sp-50.wav), and the record's own op_leg_caveat says
        // they are not comparable leg-to-leg, so their 8.68 dB of applied ALC
        // reduction is not the other half of a bracket around unity. One
        // further leg at slider ~74 — where 0.8 dB per step puts the peak on
        // the target — would measure the healthy case directly and give this
        // constant data on both sides.
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
            // WHETHER "RAISE MIC GAIN" IS EVEN THE RIGHT ADVICE depends on
            // whether the slider can still close the gap. Speech near -32 dBFS
            // against a target near -1.4 dBFS is a ~30 dB shortfall, and the
            // slider spans +40 dB from 50, so at the top of its travel there
            // may be nothing left to raise — that state is reachable for the
            // first time under a unity ceiling, and telling such an operator to
            // raise a control that is already at maximum is worse than saying
            // nothing. Derived from the mapping rather than from a threshold:
            // remaining travel versus the shortfall, so it stays true if either
            // moves.
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
    // MUTE RECEIVE AUDIO WHILE TRANSMITTING.
    //
    // The HL2 keeps receiving while it transmits, and what it receives is our
    // own signal at enormous strength. Unmuted, the operator hears the tune
    // carrier as fuzz the instant TUNE is pressed, and their own voice played
    // back on MOX — which, with an open microphone, closes an acoustic feedback
    // loop and wrecks the audio actually being transmitted.
    //
    // Muted at the DEMODULATOR, not just at the output: the spectrum keeps
    // running on real IQ so the panadapter still updates, while the audio
    // channel is clocked with silence so nothing accumulates to drain out on
    // unkey.
    // EVERY receiver, not just the transmitting one. All four are behind the same
    // antenna and hear the transmission equally, so muting only the TX receiver
    // would leave three others playing our own carrier back.
    //
    // ...unless the TX audio monitor is on, which is the one case that wants the
    // opposite. radiocert's sideband stage demodulates our OWN transmission —
    // that is the only self-contained way to check the sideband convention,
    // because the panadapter reads raw wire order and therefore agrees with the
    // transmitter by construction. The monitor is off by default and only a
    // measurement turns it on. Applied to every receiver for the same reason the
    // mute is: whichever one the capture is taken from must not be silenced.
    const bool muteWhileKeyed = key && !m_txMonitor;
    // THE TWO EDGES ARE NOT SYMMETRIC, AND #5497 IS WHAT THEY COST WHEN THEY
    // ARE TREATED AS IF THEY WERE.
    //
    // KEY DOWN: mute HERE, ahead of everything else this function does. Muting
    // early is free — the operator cannot want to hear a transmitter that has
    // not started — and late is expensive, because what the receiver hears
    // while the PA is up is our own carrier at a level that rails the ADC.
    //
    // KEY UP: the release is NOT here. It is at the bottom of this function,
    // after the MOX-off has been queued, and it is deferred past the radio's
    // T/R turnaround on top of that. See releaseRxAudioMuteAfterHold() and the
    // constant's own comment in the header.
    //
    // WHY THE OLD CODE WAS WRONG AND NOT MERELY RACY. Every Receiver::dsp is
    // moved to m_ioThread in openReceiverDsp(), and MetisClient is moved to the
    // same thread in this class's constructor. Both edges ride
    // Qt::QueuedConnection onto that one event queue, at equal priority, so
    // delivery is FIFO in POSTING order. An unmute posted ahead of the
    // setMox(false) further down this function was therefore not racing it and
    // did not sometimes lose: it was GUARANTEED to be delivered first, every
    // time. The demodulator listened at full gain while the PA was still up,
    // for the whole T/R turnaround, on every unkey.
    //
    // WHY A TIMER AND NOT AN EVENT. There is no T/R-complete indication to gate
    // on, and that was checked rather than assumed: Ep6Response::ptt is decoded
    // but MetisClient's own note records ptt_resp as `cw_on | ext_ptt` — the
    // radio's EXTERNAL keying inputs, which never go high for a host MOX key.
    // A delay is forced. #5497 measures its length; the header names it.
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

    // A VOICE key must never inherit a TUNE carrier.
    //
    // The packet builder prefers the tone over queued audio, so a tune carrier
    // left running turns every subsequent PTT into an unmodulated carrier — the
    // operator keys, the radio transmits, and not one word goes out. That is
    // exactly what a latched TUNE produced.
    //
    // Only a tone that TUNE itself raised is cleared here. A tone an operator
    // asked for explicitly is theirs, and keying is how they transmit it —
    // clearing that indiscriminately broke exactly that case.
    if (key && !m_tuning && m_toneFromTune)
        setTxTestTone(0.0, 0.0, operation);
    if (!key) {
        if (m_cwHangTimer) {
            m_cwHangTimer->stop();
        }
        m_cwHangCompletion = {};
        m_cwAutoKeyed = false;
        // An unkey ends tune too, however it was started — so the drive register
        // has to come back HERE, not in setTune()'s release branch.
        //
        // setTune(false, …) is only reached when the operator releases the TUNE
        // toggle. Every other way a tune ends — the automation TX watchdog and
        // the key verb via RadioModel::setTransmit() (RadioModel.cpp:2696), the
        // MOX/PTT coordinator (RadioModel.cpp:674), and the disconnect reset
        // below — calls setKeying(false) directly and never goes through
        // setTune() at all. Restoring there left those paths unkeyed with the
        // drive still at TUNE power, and because m_tuning is cleared on this
        // same line, setTxPower() no longer held off: the radio stayed at tune
        // power until the operator happened to move the slider. A subsequent
        // voice transmission would have gone out at 10%.
        const bool wasTuning = m_tuning;
        m_tuning = false;
        if (wasTuning) {
            setTxPower(m_rfPowerPercent);
            // AND re-decide the filter, because the call above ran while
            // m_tuning was still set.
            //
            // applyBandFilter() branches on (m_keyed || m_tuning): with receivers
            // spanning bands it forces the TX receiver's filter while either is
            // true, and bypasses otherwise. On a tune-initiated unkey m_keyed is
            // already false but m_tuning was not, so the earlier call took the
            // forced branch and left the bank on the TX receiver's filter with
            // the other receivers attenuated. Nothing re-ran it: the early-out on
            // `oc == m_ocFilterByte` means the stuck value looks current, so it
            // survives until someone happens to retune.
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
    // AND ONLY NOW THE RELEASE — after the MOX-off above is on the queue, never
    // before it. The ordering is what #5497 is about, so it is stated as an
    // ordering here rather than left to the timer to enforce: even at a hold of
    // zero, the unmute is posted behind the MOX-off rather than ahead of it.
    // The hold then covers what the ordering alone cannot — the control-packet
    // wait, the network hop, the HL2's T/R relay and the PA's decay.
    //
    // ARMED BY THE KEY-UP EDGE, NOT BY THE UNKEYED STATE — and that distinction
    // is worth the three branches below.
    //
    // A hold covers ONE T/R turnaround: the one the MOX-off just above started.
    // A second setKeying(false) arriving with m_keyed ALREADY false started no
    // turnaround and so has none of its own to cover; it inherits the one
    // already being covered. This used to be a bare `if (!muteWhileKeyed)`,
    // which re-entered releaseRxAudioMuteAfterHold() with the mute still set
    // and RESTARTED the single-shot timer from zero — measured at 311 ms of
    // mute past the real MOX-off for a 200 ms hold, against the 200 ms the
    // operator is owed. Every one of those milliseconds is charged to #5498's
    // post-unkey dropout by this change's own accounting, so the defect
    // overstated that cost as well as causing it (ten9876, #5850 review).
    //
    // THE REDUNDANT UNKEY IS NOT HYPOTHETICAL, and the paths were traced rather
    // than assumed. RadioModel::requestTransmitStop() calls
    // TransmitModel::stopTune() — which reaches setTune(false) and its
    // setKeying(false) — and then, in the same function, an unconditional
    // TransmitModel::setMox(false). setMox's off edge is not edge-guarded: the
    // `m_transmitting != on` block is skipped on an already-unkeyed model but
    // the trailing moxCommandIssued(on) still fires, so a second
    // setKeying(false) lands. pushInitialState() below is the same shape by
    // construction — it assigns m_keyed = false and then calls setKeying(false).
    //
    // Nothing upstream filters it either: on the off edge
    // TxCoordinator::Command::permitsDispatch degrades to
    // Operation::permitsCleanup(), which tests liveness and generation and has
    // no notion of key state, and every local unkey uses a cleanupFence() built
    // to satisfy exactly that.
    //
    // keyChanged is already computed above, for m_sinceUnkey and for
    // transmitChanged. The hold simply has to consult it too.
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
            // CW FULL BREAK-IN AT A SHORT DELAY SKIPS THE HOLD. RULED by the
            // maintainer (KK7GWY, 2026-09-24, on #5850): the hold is skipped
            // only when the CW hang is shorter than the hold, which is the case
            // where it would swallow the inter-element space and take QSK away.
            // At a longer delay the hang fires once, at the end of the over, and
            // that unkey takes the ordinary hold below like any other.
            //
            // THIS IS NARROWER THAN WHAT THIS PR ORIGINALLY IMPLEMENTED, and the
            // difference is the whole point. The earlier arm skipped the hold at
            // EVERY break-in delay, on the author's own reading of the trade.
            // AGENTS.md § Autonomous Agent Boundaries makes the maintainer the
            // sole authority on UX direction, and break-in behaviour is UX — so
            // the wider rule was never ours to make. At the default cwDelay of
            // 500 ms the hang is longer than this hold, so it fires once at the
            // end of the over; that key-up now gets the normal hold, so the
            // +57 dB burst #5497 measured is removed for break-in operators too.
            //
            // THE PREDICATE READS THE HANG TIMER'S OWN INTERVAL, and nothing new
            // is latched for it. setCwKeying() starts m_cwHangTimer with
            // max(kCwEnvelopeReleaseMs, clamp(breakInDelayMs, 0, 2000)), and
            // QTimer::interval() still returns that value after the single-shot
            // has fired — which is where we are, since this call arrives FROM
            // that timeout. So the comparison is the operator's configured hang
            // against the hold, exactly as the ruling states it.
            //
            // WHAT THE HOLD WOULD HAVE COST IN THE SKIPPED CASE. The unkey hold
            // is armed per
            // ELEMENT in full break-in, not per transmission: setCwKeying()
            // starts m_cwHangTimer at max(kCwEnvelopeReleaseMs, cwDelay) on
            // every element release, so at the deliberate-QSK setting of
            // cwDelay = 0 the hang is 6 ms and every element's release reaches
            // this function. The next element's key-down then stops the timer
            // and re-mutes, so a hold longer than the inter-element space
            // means the receiver never opens at all.
            //
            // THE CROSSOVER IS ARITHMETIC FROM THE TWO CONSTANTS ABOVE, NOT A
            // MEASUREMENT, and that is stated because it would otherwise read
            // as one. There is NO CW measurement anywhere behind this change:
            // #5497's eleven windows are all 4-second SSB keys. At PARIS
            // timing the inter-element space is 1200/WPM ms, so it falls below
            // kUnkeyUnmuteHoldMs (70) at 1200/70 = 17.1 WPM, and below the 76
            // ms that actually elapses from element key-up to unmute — the
            // 6 ms hang plus the hold it arms — at 1200/76 = 15.8 WPM. Both
            // numbers are divisions, done here, on constants in this file.
            //
            // SEMI-BREAK-IN IS UNAFFECTED AND MUST BE. With break-in off,
            // setCwKeying() never keys: CW rides an MOX/PTT the operator
            // asserted, and the unkey that ends the over is that operator's
            // release arriving through setKeying() with cwBreakIn FALSE. It
            // takes the branch below and gets the full hold, which is right —
            // that unkey ends a transmission and has a real T/R turnaround
            // behind it, and the operator asked for no gap to hear in.
            //
            // THE LEAK IS REAL AND IS THE PRICE, and it is now paid only where
            // the ruling says to pay it. What the receiver hears in these
            // milliseconds is the operator's own PA decaying into their own
            // front end, forty-plus times a second at speed — the same
            // +57.55 dB / +10.60 dBFS artefact this change removes everywhere
            // else. It is not suppressed here because suppressing it is what
            // takes QSK away. At a hang at or above the hold there is no
            // inter-element space to protect, so the burst is suppressed and
            // nothing is given up for it.
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
    QMetaObject::invokeMethod(m_metis, "setTxFrequencyHz", Qt::QueuedConnection,
        Q_ARG(std::uint32_t, ncoCommandHz(hz)));
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
    // MINUS the BFO, not plus. The shift names the RF frequency the detector
    // treats as zero (it is dspShiftHz's whole contract: the value that puts
    // sliceFreqHz at baseband), so pushing that zero DOWN by a pitch is what
    // lifts the marker UP onto the pitch. Adding it instead would put the
    // marker a pitch below zero — audible, on the wrong sideband, and exactly
    // the sort of sign error that hides behind a filter that was slid the same
    // wrong way.
    //
    // Unscaled by the frequency calibration on purpose: this term is an audio
    // offset, not an RF frequency. Scaling it would be applying a crystal
    // correction to the operator's sidetone pitch.
    return dspShiftHz(r.sliceFreqHz, r.ncoHz) - cwBfoHz(r.mode);
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
    // Remember whether THIS transmission carried client-leveled audio. The
    // unkey diagnostic that used to read it went with the ALC's makeup half,
    // so the flag is carried rather than consumed here until #5647 gives it a
    // job as part of TxAudioSource — see Hl2Backend.h.
    //
    // The sticky OR — and the per-block source it forwards — lean on two
    // sources never interleaving inside one transmission. If that mutual
    // exclusion is ever relaxed, Hl2TxDsp would flip its ALC per block and
    // process m_inBuffer residue under the newest block's source — no crash,
    // just a level that depends on block alignment. That cost nothing while
    // the buckets differed only in a ceiling they now share; with
    // EngineGenerated bypassing m_micGain it is worth the slider's full range.
    //
    // WHAT PROVIDES THE EXCLUSION, naming both halves, because an earlier
    // version of this comment named only the half that faces the microphone.
    //
    // EngineGenerated has exactly one producer, AudioEngine::startWsprPump(),
    // and it is fenced on both sides:
    //   • against the MIC path — startWsprPump() calls setDaxTxMode(true), and
    //     onTxAudioReady() returns early on m_daxTxMode;
    //   • against the CLIENT path — feedDaxTxAudio() returns early while
    //     m_wsprBeacon->isActive(), so TCI/DAX samples are dropped rather than
    //     interleaved for the length of the frame.
    //
    // tciAudioFresh() is NOT either of those: feedDaxTxAudioInternal arms
    // m_tciAudioTimer under markExternalSource alone, and the WSPR feed passes
    // it false.
    //
    // Both of those are still fences in ANOTHER class, so they are no longer the
    // only thing holding: Hl2TxDsp::processAudioBlock drops carried m_inBuffer
    // residue when the source changes mid-transmission, which makes the property
    // structural rather than argued. A second engine-generated feed that forgets
    // the fences gets a warning and a dropped block instead of a silent 40 dB
    // level flap. Whoever touches the mic-capture gate — or adds such a feed —
    // still owns re-checking.
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
    // Apply immediately if we are already keyed, so a diagnostic can enable the
    // monitor mid-transmission rather than having to unkey and start again.
    //
    // EVERY receiver, matching setKeying(): the capture is taken from the mixed
    // output, so whichever receiver contributes to it must not be silenced. The
    // mixer's own keyed-drop honours m_txMonitor as well — see mixReceiverAudio(),
    // which is the site that actually gates audioFrameReady().
    // Enabling the monitor mid-transmission is the second way sampling resumes,
    // and the one where the held peak is most likely still fresh by age — the
    // key-down that froze it may be only milliseconds old. applyRxAudioMute()
    // stamps SliceSamplingGate for both.
    //
    // NO HOLD ON THIS PATH, and that is not an oversight. #5497's hold exists
    // to cover the radio's T/R turnaround — the interval in which the PA is
    // still up after the host has asked it to stop. Turning the monitor ON
    // asks to HEAR the transmitter, so there is nothing to wait for; turning it
    // OFF while keyed must silence it now, for the same reason the key-down
    // edge mutes now. Either way this is the immediate path. A hold placed here
    // would make a diagnostic that enables the monitor mid-over wait 70 ms for
    // audio it deliberately asked for.
    if (m_keyed && !on) {
        if (m_unkeyUnmuteTimer) {
            m_unkeyUnmuteTimer->stop();
        }
        applyRxAudioMute(true);
    } else if (!on && !m_keyed && m_unkeyUnmuteTimer && m_unkeyUnmuteTimer->isActive()) {
        // AN ARMED HOLD IS NOT AN OVERTAKEN ONE, and the else branch below used
        // to treat it as one. (UNKEYED, monitor OFF) is exactly the state an
        // unkey has just left behind, with the hold running and the PA still
        // up; cancelling it there unmutes inside the T/R turnaround -- the
        // defect this whole change exists to remove, let back in through
        // another door.
        //
        // Not hypothetical: RadioCertification's run() epilogue calls
        // keyViaOperatorPath(false) and then setTxAudioMonitor(false) in the
        // same synchronous unwind, which is the one path in the tree that
        // deliberately listens to its own transmitter.
        //
        // AND IT SPLITS ON `on`, WHICH IS THE CORRECTION. This guard was first
        // written without the `!on`, so it was taken for BOTH values and
        // swallowed setTxAudioMonitor(TRUE) inside the window as well: a
        // diagnostic that asked to HEAR the receiver got silence until the
        // timer expired, and a following key-down -- muteWhileKeyed false
        // because the monitor is on -- then re-armed a fresh hold off the mute
        // that was still set. ASKING TO HEAR IS ASKING FOR SOMETHING; only
        // asking for OFF is asking for nothing, and only that one may be
        // answered by doing nothing (ten9876, #5850 review).
        //
        // So: monitor OFF inside an armed hold leaves the timer running and
        // stays muted, and the hold expires on its own a few tens of
        // milliseconds later. Monitor ON falls through to the immediate path.
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
    // A tune carrier is an unmodulated steady signal at the transmit frequency.
    // The HL2 has no tune generator of its own, so it is the built-in test tone
    // at ZERO offset — a carrier exactly on the TX NCO — with keying held for as
    // long as tune is engaged.
    //
    // Ordering matters in both directions: bring the carrier up BEFORE keying so
    // the first frames on the air already carry it rather than silence, and drop
    // the key BEFORE the carrier on the way out so nothing is left radiating if
    // the tone clear is delayed behind other queued control verbs.
    //
    // Drive is set from TUNE power, not RF power. The carrier amplitude is a
    // fixed full-scale constant, so without this the drive register still held
    // whatever setTxPower() last pushed — the RF Power slider — and an operator
    // running RF 100 / Tune 10 got a FULL-POWER carrier from a control whose
    // whole purpose is to reduce it. Flex is unaffected: it receives tune power
    // as a text command and applies it radio-side.
    // The RF power restore is NOT here: it lives in setKeying(false), which is
    // the one point every unkey path converges on. See the comment there.
    //
    // m_tuning is therefore set only on the way UP, and left for setKeying() to
    // clear on the way down. Assigning it unconditionally here would clear it
    // before the setKeying(false) below could see it, and the restore that reads
    // it would never fire on the one path that always goes through this
    // function — the operator releasing the TUNE toggle.
    if (on) {
        m_tuning = true;
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
    // SSB VOICE ONLY, even though the operator's setting is remembered globally.
    //
    // The control this comes from is the PHONE applet's TX low-cut/high-cut, and
    // it means "shape my voice". Letting it reach CW would set the keying
    // envelope's bandwidth from a voice slider — an operator who widened to
    // 100..4000 for eSSB would transmit CW four times wider than the 300..900
    // the mode wants, and nothing in the phone applet would suggest why. AM/FM
    // are excluded for the same reason, and the digital modes because their
    // 150..3000 is chosen to match what the far-end decoder expects rather than
    // what sounds good.
    const QString u = mode.toUpper();
    const bool ssbVoice = u == QLatin1String("USB") || u == QLatin1String("LSB");
    if (m_txFilterFromOperator && ssbVoice)
        return {m_txFilterLowHz, m_txFilterHighHz};
    return defaultTxPassbandForMode(mode);
}

// Push the effective passband at the modulator AND echo it upward.
//
// The echo is what stops the Phone applet's cut readout being a claim about a
// passband the modulator is not running. TransmitModel adopts the operator's
// request optimistically — it has to, because a host-modulating backend never
// echoes status — so after a mode change into CW the applet would still show the
// eSSB 100..4000 the operator dialled in for SSB while the transmitter ran the
// 300..900 CW wants. Nothing on screen would say which of the two was real.
//
// Same shape, and the same reason, as the per-band drive echo in
// applyPerBandStateFor(): the backend is authoritative about what it actually
// applied, and says so as a normalized delta. TransmitModel::applyChanges()
// deliberately does NOT emit txFilterCommandIssued, so this cannot loop back
// through RadioModel as a fresh setTxFilter() (pinned by transmit_model_test).
//
// This is also what makes a RESTORED passband visible: applyRestoredState()
// seeds the members, connectRadio() hands them to the modulator through the
// Config, and without an echo the applet showed its own construction default
// until the operator happened to touch a button.
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

    // Push through effectiveTxPassband() rather than the raw values, so a change
    // made while the transmitter is in CW is REMEMBERED but not applied — it
    // takes effect when the operator returns to SSB. Pushing lowHz/highHz
    // directly here would bypass the mode rule that every other call site
    // honours, and the setting would apply immediately in CW and then correct
    // itself on the next mode change.
    //
    // The one push that does NOT go through pushTxPassband(), because it must not
    // echo. In SSB the echo would be value-identical and pointless; outside it,
    // snapping the applet back to the mode default would make the operator's NEXT
    // nudge compute from that default and quietly overwrite the eSSB pair this
    // call just remembered. The mode-change echo below is what makes the readout
    // honest, without a path that can eat the setting.
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

// The Phone applet's MIC slider, 0..100, onto the modulator's linear pre-ALC gain.
//
// 50 IS UNITY, and that is load-bearing rather than cosmetic. This backend now
// remembers its own radio's slider position across sessions — captured into the
// txSetpoints extension in currentOperatingState(), applied by
// pushInitialState() — so the level can arrive here as the operator's last
// position rather than a fresh 50. But 50 is still what a radio with NOTHING
// stored comes up on, and that session must leave the modulator exactly where
// its own default (m_micGain = 1.0) puts it. A mapping with unity anywhere else
// would silently change the transmit level of every existing HL2 install, both
// the first time this code shipped and the first launch after the level began
// persisting.
//
// The travel around that point is ASYMMETRIC: -20 dB below 50 at 0.4 dB per
// step, +40 dB above it at 0.8 dB per step. The upward half was widened when the
// ALC's 40 dB of makeup was removed — speech near -32 dBFS against an ALC target
// near -1.4 dBFS is a ~30 dB shortfall, and the old +20 dB left the chain 10.6 dB
// short at maximum slider. Widening it symmetrically would have moved unity off
// 50 and changed the transmit level of every existing install, which is the one
// thing the paragraph above forbids. So the two legs meet at 50 with different
// slopes, and hl2_tx_level_policy_test pins the join.
//
// WHAT THIS BUYS IS THE SAME ON THE TWO PATHS IT REACHES, and that is the
// change. The ALC behind this only reduces — it has no makeup half left to give
// the gain back with — so this slider is a straight proportional control on the
// air all the way up to alcTargetPeak, for the microphone (including the AX.25
// modem, whose AFSK amplitude is a fixed constant this slider is the only way to
// move) and for a TCI/DAX client alike. TX gain 5 is a real -18 dB. Past the
// target the ALC limits rather than letting the modulator's hard clamp flat-top
// the signal, so the last stretch of travel buys reduced headroom rather than
// more power.
//
// IT DOES NOT REACH TxAudioSource::EngineGenerated AUDIO AT ALL.
// Hl2TxDsp::processAudioBlock substitutes 1.0 for this multiplier on the WSPR
// pump, so a beacon goes out at the level its generator chose and this slider
// does not move it — at 0 or anywhere else. Hl2TxLevelPolicy.h carries the full
// argument; TxAudioSource.h carries which source is which.
//
// The other path-dependent thing is setKeying()'s "raise mic gain" diagnostic,
// gated off for client-leveled AND engine-generated transmissions — for the
// client because the remedy for a quiet TCI/DAX client is that client's own
// level control, and for a beacon because there is no mic slider in its path to
// raise.
//
// LEVEL 0 MUTES THE MICROPHONE AND THE TCI/DAX PATH. IT DOES NOT SILENCE THE
// TRANSMITTER. A slider at the bottom of its travel means off for the audio
// this multiplier reaches, and -20 dB is simply -20 dB on the air, so the
// special case is there because the bottom of a travel should mean off rather
// than very quiet. But engine-generated audio never reaches this multiplier:
// parking this control at 0 between voice sessions does not stop a WSPR beacon.
// Stopping an unattended transmission is the generator's own control, not this
// one.
void Hl2Backend::setMicGain(int level)
{
    level = std::clamp(level, 0, 100);
    const bool moved = level != m_micLevel;
    m_micLevel = level;

    const double linear = micSliderToLinear(level);

    if (m_txDsp)
        QMetaObject::invokeMethod(m_txDsp, "setMicGain", Qt::QueuedConnection,
            Q_ARG(double, linear));

    // The capture half of this radio's TxSetpoints memory. RadioModel debounces
    // this into one RadioStateMemory::store — the backend never touches the
    // settings store itself (IRadioBackend's contract).
    //
    // ONLY ON CHANGE, and the gate is load-bearing rather than an optimisation.
    // RadioModel::setupBackend() re-asserts the model's level into every freshly
    // built host-modulating backend, so an unconditional notify here would
    // schedule a store on every backend rebuild — writing the value back under
    // whichever radio connected next, which is the cross-family bleed this
    // shape exists to prevent. A value-identical echo is not the operator
    // moving the slider, exactly as in setTxPower() above.
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

// A restored mic level, read against the curve the document that carries it was
// written on.
//
// The document says which curve with `micLevelCurve`. ABSENT MEANS CURVE 1 —
// the key did not exist while curve 1 was the only curve, so its absence is a
// positive statement about the writer rather than a gap, and that is the whole
// reason the migration can be one-shot: writing the level back stamps the curve
// beside it, so the next read takes the identity branch and the level stops
// moving.
//
// Anything else present and readable is taken at face value, including a curve
// number from the future. This function's job is to not misread a document, and
// a level written by a build that knows a curve this one does not is a level
// this build cannot re-derive; re-mapping it on the guess that a higher number
// means a wider leg would be inventing a setpoint, which is what the DROPPED
// rule above the caller exists to refuse. A future curve therefore restores
// as-written, and the operator's own next slider move re-stamps it.
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
            applyFreqCalPpb(arg.toInt(), /*persist=*/false);
            if (requestId != 0)
                emit extensionResult(requestId, QVariant(m_freqCalPpb));
            return;
        }
        if (verb == QLatin1String("freqcal.get")) {
            if (requestId != 0) {
                emit extensionResult(requestId, QVariantMap{
                    {QStringLiteral("ppb"), m_freqCalPpb},
                    {QStringLiteral("effectiveClockHz"),
                     Hl2FreqCal::effectiveClockHz(m_freqCalPpb)},
                    {QStringLiteral("scale"), m_freqCalScale},
                });
            }
            return;
        }
        // The wideband bandscope (endpoint 0x04). NO UI AND NO SETTING, on
        // purpose: it is a diagnostic, nothing in the app makes a decision from
        // it, and it is off again at the next connect.
        //
        // ONE CALLER, and it is the automation bridge: AutomationServer's
        // `bandscope` verb (doBandscope). That is the whole operator-facing
        // surface this feature has, and deliberately so — the panadapter-style
        // display and the policy that would act on what it sees are #5535 and a
        // display RFC, neither of which this PR pre-empts. Earlier rounds of
        // review held this open as "no caller anywhere in src/", which was true
        // and is no longer: the route landed in review round 3 rather than
        // shipping an endpoint nothing could reach.
        //
        // What this starts is MetisClient's DUTY-CYCLE GATE, not the stream:
        // one 2048-sample block per sampling period, TWELVE datagrams a second,
        // 0.11 Mbit/s — 3 discarded while arming, 4 flushed, 4 kept, 1 trailing
        // (PR #5650 review; the derivation is at setBandscopeEnabled's header in
        // MetisClient.h). Ungated the same stream is ~3.3 Mbit/s, about as much
        // again as the IQ at 1 RX / 48 kHz.
        //
        // Completes locally, like freqcal.set above and for the same reason:
        // the run byte is fire-and-forget, nothing in Protocol 1 reads it back,
        // and fabricating a device round trip to await would be inventing a
        // confirmation the wire cannot give.
        if (verb == QLatin1String("bandscope.enable")) {
            // Refused while disconnected, and REPORTED as refused: MetisClient
            // ignores a run byte with no stream behind it, so echoing the
            // request back would be this side inventing a state the radio was
            // never told about.
            const bool on = arg.toBool() && m_connected;
            // NOT mirrored here. The health row follows LinkCounters, which
            // reports what MetisClient actually has rather than what this side
            // asked for — the two can disagree across the thread hop, and when
            // they do the gate is right and the request is stale.
            //
            // AN EXPLICIT OPERATOR ACTION TAKES THE GATE BACK, in both
            // directions: turning it on makes it theirs to turn off, and
            // turning it off must not leave the loop believing it still owns a
            // stream that is no longer running. Either way the automatic
            // control stops being the owner; see applyBandscopeForAutoGain.
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
        // ONE wideband frame, on demand. THE REQUEST/REPLY VERB, unlike every
        // other verb in this namespace: the reply carries 2048 converter
        // samples and it cannot be answered locally, because the radio has to
        // be asked for a block and the block takes an arming cycle to arrive.
        // That is exactly the asynchronous case IRadioBackend's requestId
        // contract was written for.
        //
        // ON DEMAND, NOT CONTINUOUS, and that is the design and not an
        // omission. A continuous consumer would be a second permanent load on
        // the hl2-io thread — which already carries EP2 pacing, EP6 ingest,
        // WDSP and the panadapter FFT — and that cost has NOT BEEN MEASURED.
        // A frame per request costs one arming cycle: wide_spectrum up for
        // ~13 ms, four datagrams kept, down again.
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
                    // `on`/`level` are what the DSP ACTUALLY HAS, read across
                    // the thread boundary through Hl2RxDsp's atomics — not what
                    // this backend was asked for. Reporting the request would
                    // make this verb certify its own input: the request is
                    // stored in r.nbOn synchronously, before the queued call to
                    // the chain has run, so it stays true even if the chain
                    // never got it or refused it. requestedOn/requestedLevel
                    // are reported alongside precisely so the two can be
                    // COMPARED — a mismatch is the "the control moves and
                    // nothing happens" failure this verb exists to catch.
                    //
                    // With no chain (a receiver between rebuilds) there is
                    // nothing applied yet, so `on` is false rather than a
                    // flattering echo of the request.
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


    // ONLY the in-band readings here. The stream-free ones come from
    // Hl2TelemetryService, and AutomationServer::doHealth() merges the two with
    // THESE winning on key collision -- in-band is fresher (10 Hz against
    // 1-2 Hz) and its cadence is ours.
    //
    // Merging here instead, as this did at first, makes the stream-free rows
    // unreachable whenever this backend does not exist -- which is precisely
    // the state they are for. That was the defect: an instrument for the
    // no-connection case owned by the connection.
    //
    // AND ONLY WHILE THE STREAM IS ACTUALLY DELIVERING THEM. m_telemetry is
    // never cleared -- it accumulates from EP6 and the last value of every
    // field simply stays -- so "in-band wins on key collision" meant the last
    // readings from BEFORE a stall kept winning over the fresh port-1025 ones
    // for as long as the process lived. A frozen temperature presented as live
    // in-band telemetry is not a side effect of this feature, it is the exact
    // failure the feature was built to expose, so it must not be the feature's
    // own output. When the link is not Streaming these rows report nothing and
    // the stream-free rows survive the merge.
    //
    // "Reports nothing" means an INVALID variant, which put() leaves out of
    // `values` entirely; hl2MergeHealth treats an absent value as "not
    // reported" and leaves the base's alone. Writing zeros here would erase
    // the poller's readings instead of yielding to them.
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
    // ── §13 item 16: the two ADC readings, side by side ──────────────────
    //
    // THEY DISAGREE BY DESIGN, and the disagreement is the diagnostic. One is
    // measured before the DDC and sees everything the converter sees; the other
    // is measured after it and sees one slice. A slice can look quiet while the
    // converter saturates on a broadcast station 20 MHz away — this lab has
    // measured exactly that, and it is why the clip flag alone was the wrong
    // driver for a gain decision. Showing both, labelled distinctly, is what
    // turns an inference into a readout.
    //
    // NEITHER IS CALIBRATED, and they do not even share a scale: the slice
    // figure is dB relative to WIRE full scale, the DDC between the two
    // measurement points carries an unquantified processing gain, and
    // Hl2DbReference::isCalibrated() is false -- its fullScaleDbm is DERIVED
    // rather than measured, and it refers the DISPLAY path in any case, not
    // these two readings -- so nothing here is antenna-referred. The labels say "uncalibrated" because that is
    // the whole of what can be claimed. What survives the missing calibration
    // is the PAIRING itself — the pairing row below states a relationship, and
    // a relationship needs no absolute reference.
    //
    // DISPLAY ONLY, per IRadioBackend.h: "Purely for display — nothing in the
    // app makes a decision from it." Nothing reads any of these rows back.
    // Hl2AdcPairing.h holds the reasoning and the verdict.
    // `t`, not m_telemetry: #5414 made the in-band rows report NOTHING while
    // the link is not Streaming, so a frozen pre-DDC flag cannot outlive the
    // stall and beat the stream-free poller's fresher row in the merge. The
    // pairing below reads the same gated value, which is the converter-side
    // twin of the slice-side freshness gate.
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
        // THE PAIRING. One sentence naming both sides and the gap between
        // them, so the operator reads the relationship instead of deriving it
        // from a dB figure and a boolean two rows apart.
        // The liveness gates are the whole reason this is not just two rows read
        // together. `peak` is HELD through every transmission while
        // m_telemetry.adcOverload keeps moving — EP6 responses ride the same
        // datagrams as the IQ — so without them the sentence would pair a
        // frozen side against a live one and, on a radio whose transmitter
        // shares the receiver's port, assert that the operator's own carrier is
        // "elsewhere in 0-38.4 MHz".
        //
        // TWO OF THEM, because the age alone cannot see the START of a
        // transmission. At key-down `ago` is the age of the last RECEIVE block,
        // under one block period, and it has to climb to kSliceStaleMs before
        // the age gate shuts — 129-150 ms of inverted verdict on every
        // key-down, and this dialog refreshes every 500 ms. But THIS function's
        // own object queued that mute (setKeying, `muteWhileKeyed`), so it
        // knows synchronously that Hl2RxDsp is about to stop sampling.
        //
        // KNOWING IT STOPPED IS NOT KNOWING IT RESTARTED, which is why this
        // reads the gate rather than mirroring `muteWhileKeyed` here. The flags
        // are cleared synchronously and the unmute is queued, so a mirror turns
        // true a block early; on a short key-down the held peak is still inside
        // kSliceStaleMs and the verdict would be asserted from a value nothing
        // is sampling. SliceSamplingGate answers from the stamp on the reading:
        // Hl2RxDsp writes that stamp only while unmuted, so a peak newer than
        // the resume request is proof the chain is sampling again. The TX audio
        // monitor is still honoured — it is what makes the request true — the
        // chain keeps sampling through the transmission, and the pairing keeps
        // pairing.
        //
        // The age gate STAYS. It is the general one — a stalled IQ stream, a
        // starved DSP thread, a chain between rebuilds — and none of those
        // announce themselves to this function. Hl2AdcPairing.h carries both.
        //
        // NaN rather than 0.0 for the don't-care: 0.0 dBFS is a REAL reading
        // (full scale), so a don't-care spelled 0.0 is only safe while
        // `realPeak` short-circuits ahead of it. NaN is inert either way.
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
    // THE CLIP RATE, AND ITS DENOMINATOR BESIDE IT ON PURPOSE.
    //
    // "ADC overload" above is one bit, sampled at 10 Hz, from a flag that
    // cycles up to ~190 times a second: it answers "was it railing at the
    // instant we last looked", which on a dithering band is nearly a coin
    // toss. These rows answer "how often is it railing, out of how many
    // chances" -- the question an operator turning the RF Gain slider on a
    // live antenna is actually asking.
    //
    // The percentage row is ABSENT rather than zero whenever the window
    // carried too few observations to have a rate. That is the same rule the
    // `values`-omission above encodes and it is load-bearing here: three of
    // three responses railing is not 100 %, and none of two is not 0 %.
    //
    // AND THE WHOLE SECTION GOES QUIET WHEN THE RADIO STOPS STREAMING. The
    // gateware clears the counter behind this bit only inside the EP6
    // response cycle, so there is no idle poll for it -- unlike temperature,
    // power and PTT, which do keep converting at idle. "Observed" says how
    // long ago the last window with any observations in it arrived, so a
    // frozen zero cannot be read as a quiet band.
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

    // DRIVE: WHAT WAS ASKED FOR, AND WHAT WAS WRITTEN (#4912).
    //
    // Nothing anywhere reported the APPLIED drive. `get transmit` has rfPower,
    // which reads TransmitModel — the operator's request — which is exactly
    // the readback-shares-the-failure problem this section exists to solve.
    // (This used to name `get radio`.txPower alongside it as a second reader of
    // TransmitModel. It was neither: it read a RadioModel member nothing in the
    // tree assigned, so it was worse than the readback this paragraph warns
    // about. It now carries the measured forward power, qualified — see
    // AutomationServer's radioSnapshot, #5499 item 1.)
    //
    // Worse, applyDrive()'s transmit gate forces the
    // register to 0 while the requested percent reads back untouched, so
    // "commanded but never applied" was invisible to automation in the one area
    // where it is safety-adjacent.
    //
    // The raw register is reported alongside the percent rather than instead of
    // it because the gateware decodes only the drive byte's top nibble: the
    // 0..255 scale moves in steps of 16, so 100 distinct percents land on 16
    // distinct drives and a percent alone cannot tell you which one the radio
    // got.
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

    // THE VOICE CHAIN, END TO END, AS THE MODULATOR ACTUALLY RAN IT.
    //
    // Every row here is read from this backend rather than from TransmitModel.
    // That distinction is the entire reason the section exists: the bridge's
    // transmit snapshot reports the operator's REQUEST, so a control whose verb
    // was dropped on the floor read back as though it had worked. Mic gain was
    // exactly that — the slider moved, the snapshot agreed, and the modulator
    // never heard about it. A readback that shares the failure it is meant to
    // catch is worse than none, because it manufactures confidence.
    //
    // So: what the operator asked for AND what the modulator is running, side by
    // side, and a diagnosis is one comparison rather than a source dive.
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
    // WHAT THIS TRANSMISSION CARRIED, because a mis-tag is otherwise silent.
    //
    // The whole point of TxAudioSource is that engine-generated audio bypasses
    // the mic slider, and the failure mode is a beacon quietly going out 18.58 dB
    // down with every unit test still green. This section's thesis is "report
    // what the modulator is running", and this is the one row that says which
    // levelling rule the last over actually took. Sticky per transmission, both
    // flags cleared on each key edge in setKeying(), so a mixed over reports
    // "mic+engine" rather than picking a winner — which is itself the signal
    // that two producers fed one transmission.
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
    // The target the modulator was actually CONFIGURED with, not the one a
    // default-constructed Config would have. The two agree today because the
    // Config built in connectRadio() never touches this field — but this row sits
    // in a section whose thesis is "report what the modulator is running",
    // and a constant that silently stops matching the modulator is the row
    // nobody would think to suspect.
    //
    // This row reported alcHoldBelowDbfs until the ALC's makeup half was
    // removed. It is the target now, and it applies to EVERY path rather than
    // to the mic path only: with the ceiling at unity there is no longer a
    // mic/client asymmetry for a reader to be misled by.
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
    // The value the FWDPWR meter is actually driven from, next to the raw
    // instantaneous sample it is derived from. Both, because the difference
    // between them IS the diagnosis on SSB: a wide gap means the envelope is
    // being sampled off its peaks, which is the whole reason the hold exists.
    // On a constant-envelope carrier the two should very nearly agree, and a
    // held value ABOVE the instantaneous on TUNE would mean the release is
    // inflating the reading rather than holding it.
    //
    // Gated on the same optional as the instantaneous row above, so the pair
    // says "never told" or "told" TOGETHER. Reported unconditionally this read
    // a hard 0.0 before any telemetry, next to a neighbour saying "not
    // reported" — and "0 W" from a wattmeter reads as a measurement, which is
    // the one thing nothing in this section is allowed to fake.
    // DISPLAY HOLD — NOT AN ASSERTION TARGET (#4912). This is a meter's
    // peak-hold: one key-edge ADC sample decays over seconds, so a script that
    // asserts on it reads a transient from the start of the over as though it
    // were the power now. That is correct for a needle and wrong for a test.
    // Assert on forwardPowerW, the instantaneous row above.
    put("forwardPowerPeakW",
        QStringLiteral("Forward (W, approx — peak HOLD, display only)"),
        t.forwardPowerRaw ? QVariant(m_fwdPeakWatts) : QVariant());
    put("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),
        opt(t.reversePowerRaw));
    put("reversePowerW", QStringLiteral("Reverse (W, approx)"),
        t.reversePowerRaw
            ? QVariant(directionalWatts(*t.reversePowerRaw)) : QVariant());
    // Meaningful without calibration — it is a ratio, so the unknown SCALE
    // cancels. The detector's CURVE does not cancel, which is why swrFromRaw()
    // linearizes both counts first (#4578). Absent below the noise floor, where
    // a ratio of two noise samples is not a mismatch reading.
    //
    // That last sentence described the intent but not the code: this site had no
    // floor, so with no carrier it recomputed a noise ratio at the dialog's
    // 500 ms refresh and the row visibly bounced — while the TX:SWR meter, which
    // did apply the floor, correctly showed nothing. Same guard here now, from
    // the shared constant, so the two surfaces cannot disagree.
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

    // ---- attribution: which path produced the readings above ----
    //
    // The backend publishes this and WINS the merge, because these rows are
    // in-band: 10 Hz against the poller's 1-2 Hz, on a cadence we control.
    // Hl2TelemetryService publishes the same key for the stream-free case and
    // loses to this one whenever we have in-band values.
    //
    // It went missing when the service took the four telemetry rows over, and
    // the result was a row that could never say "in-band" — observed on
    // hardware with the app connected and EP6 healthy, reading "port-1025".
    // Decided by the shared policy rather than restated here: a re-typed copy
    // of a rule proves only that two copies agree.
    //
    // BOTH INPUTS COME FROM HERE, and that is the correction. `haveStreamFree`
    // was hardcoded false on the grounds that "the backend knows nothing about
    // the stream-free path" — but this row WINS the merge, so hardcoding it
    // made `port-1025` unreachable for as long as a backend existed: the
    // service's answer was overwritten in every case, including the stalled one
    // the design note calls the case that matters most. Attribution cannot be
    // decided by either side alone, and the side that wins the merge is the
    // side that has to ask both. The service is injected here precisely so it
    // can be asked.
    //
    // hl2_telemetry_source_test already pinned hl2TelemetrySource(true, false,
    // true) == port-1025 and passed; what was missing was any production path
    // that passed those three arguments together. This is it.
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
    // ---- the silence watchdog's recovery record ----
    //
    // Reported unconditionally, next to the loss row, because the question they
    // answer is the same one: "why did the audio have a hole in it". When the
    // radio stops streaming an established link, MetisClient re-sends the run
    // command before declaring the link down, and a recovery that WORKS is
    // invisible everywhere else -- no linkDown, no linkUp, no reconnect, no
    // pane rebuilt. These two rows are the only place it shows.
    //
    // READ THEM TOGETHER. Attempts climbing while completions do not is the
    // shape that says the re-send is not the right answer for whatever is
    // actually failing, and it is the reading that would be lost if only one of
    // them were published.
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
    // The gate's own health. Blocks ACCEPTED is not ep4Packets/4: most of what
    // arrives is armed, flushed or trailing, and none of that becomes a
    // reading. Timeouts should read zero in steady state, but a non-zero value
    // does NOT on its own mean the radio stopped answering the run byte — and
    // this row used to say it did. bandscopeGuardMs names two benign ways it
    // fires, and they are the honest reading: if the arming delay is clocked by
    // EP6 samples rather than packets, the first cycle after a receiver-count
    // change is abandoned; and the EP4 rate at two, and at four or more,
    // receivers is UNMEASURED, with the block term sized from the slower of the
    // two counts that were measured. Read it as "look at bandscopeGuardMs's
    // assumptions first", not as a hardware fault, or an operator who has just
    // added a fourth panadapter goes hunting for one (PR #5650 review).
    put("bandscopeBlocks", QStringLiteral("Bandscope blocks accepted"),
        static_cast<qulonglong>(m_ep4Blocks));
    put("bandscopeTimeouts", QStringLiteral("Bandscope block timeouts"),
        static_cast<qulonglong>(m_ep4Timeouts));

    // ---- the headroom rows ----
    //
    // NOT REPORTED until a block has arrived, and that is the INVALID VARIANT
    // and not a missing key. put()'s own contract above is the mechanism --
    // "An INVALID variant is left out of `values` on purpose: that is what
    // renders as 'not reported'" -- so the row is always in `order`, always
    // labelled, and reads as a dash until there is something to say. There is
    // no number that honestly stands for "the converter's level has never been
    // looked at", and 0.00 dBFS in particular would read as a hard clip.
    //
    // Guarding the put() calls themselves, as this first did, drops the keys
    // out of `order` entirely: four rows then appeared the moment the first
    // block landed and vanished again on every link edge through
    // resetBandscopeMirrors(), so the dialog's row list changed shape under a
    // reader mid-refresh. (PR #5650 review round 3.)
    //
    // LABELLED UNCALIBRATED, PRE-DDC, and the label is the point. These come
    // off the AD9866 before the DDC, the decimation and the NCO, on the
    // converter's own scale. They are commensurable with the gateware's clip
    // and good-level flags — the same rx_data register feeds both — and with
    // NOTHING ELSE: not the S-meter, not the WDSP ADC peak, not any
    // antenna-referred level. The comparison that would change that is the
    // study's Procedure C; it needs a live antenna and it has not been run.
    //
    // And per IRadioBackend.h: "Purely for display — nothing in the app makes a
    // decision from it." Nothing reads these rows back.
    // ---- what WDSP did with the IQ, per receiver ----
    //
    // THE DSP-SIDE TWIN OF "Dropped EP6 packets" ABOVE. That row counts
    // samples the WIRE lost; these count blocks the DSP refused to turn into
    // audio. Both end as "the audio sounds wrong", and until now only one of
    // them was answerable: every non-`Ok` WdspChannel::ProcessResult was a
    // bare `continue` in Hl2RxDsp::processIqBlock, so a chain that had
    // produced no audio for a minute because WDSP was returning EngineError
    // on every block was indistinguishable from one whose pipeline was
    // filling normally.
    //
    // UNDERRUNS GET THEIR OWN ROW and are not added to the fault row, because
    // they are NORMAL: fexchange2 returns -2 whenever the asynchronous output
    // side has nothing ready, which is every block of a fresh connect and a
    // routine occurrence thereafter. A reader who sees a four-figure
    // "underruns" folded into "faults" on a perfectly healthy radio learns to
    // ignore the row, which is this instrument failing at its own purpose.
    //
    // ABSENT UNTIL A BLOCK HAS BEEN PROCESSED, per put()'s contract: a
    // receiver between rebuilds, or one that has never seen IQ, reports "not
    // reported" rather than a row of confident zeros. Zero faults out of zero
    // blocks is not a clean bill of health.
    //
    // DISPLAY ONLY, like every other row here -- nothing in the app makes a
    // decision from these. The machine-readable form is
    // Hl2RxDsp::processTally(), which returns the six counts as integers.
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
        // THE SAME FACT AS A NUMBER, because the row above is the only form a
        // SCRIPT can see and it is prose. healthSnapshot() feeds the automation
        // bridge's `health` verb, which exists -- in its own documentation's
        // words -- because these rows "until now reached nothing else and so
        // were unavailable to a script or a regression test". Leaving the count
        // only inside "3 - WDSP engine error 3" makes a soak test's best
        // available assertion `!= "none"`, and makes that QString format a
        // contract by accident. Hl2RxDsp::processTally() is the machine-readable
        // form, but nothing on the bridge can reach it.
        //
        // ONE row, not four. The per-kind split stays prose for the reason
        // given above -- three of the four kinds are zero on every radio that
        // has ever worked. What a script actually needs is a threshold on the
        // total, and that is what this is. Caught by aethersdr-agent on #5738.
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
    // TWO QUESTIONS, NOT ONE, AND THE ROWS BELOW DIVIDE ON WHICH THEY ANSWER.
    //
    // `haveObservation` is "has a block ever arrived". `haveBlock` is "is the
    // newest one still describing now". Those were the same test here until
    // this changed, and the gap between them is a defect with a measurement
    // behind it: stop the EP4 stream inside a session -- closing the wideband
    // bandscope does exactly that, with the link up and everything else on the
    // dialog live -- and the level rows went on publishing the last block the
    // gate happened to deliver, indefinitely, with no marker. Measured on
    // hardware: 31 dB of commanded LNA gain moved these three rows 0.00 dB
    // while adcSlicePeakDbfs0, sampled by a different subsystem, moved 19.04
    // dB over the same steps. An earlier bench leg caught a pair frozen 21.17
    // dB away from the radio's actual operating point.
    //
    // resetBandscopeMirrors() already applies exactly this cure at exactly one
    // edge -- "back to 'never seen', which is what makes the level rows go
    // ABSENT again rather than keep showing the previous session's last
    // reading" -- and a gate stopped mid-session is the same sentence with the
    // link still up. The auto-gain path is handed the age and refuses on it
    // (see the bandscopeHeadroom() call in stepAutoGain); these rows were
    // handed nothing. Two readers of one block, one refusing and one
    // publishing.
    //
    // THE EXPIRY IS bandscopeHeadroom()'s OWN, deliberately and not by
    // coincidence: kHeadroomMaxAgeMs, three MetisClient::kBandscopeSampleMs
    // gate periods, which Hl2BandscopeHeadroom.h derives as two periods of
    // block-to-block spacing plus one of slack for a late I/O thread, and
    // which that header already calls "a DISPLAY AND CONTROL boundary". The
    // rows and the loop read one block from one gate, so a separately chosen
    // display threshold would buy nothing and would leave a window in which
    // the loop refuses a block these rows still publish -- a smaller version
    // of the bug being fixed.
    //
    // WHAT KEEPS A LIVE GATE FROM BLINKING is the producer's period against
    // this expiry and nothing else: MetisClient::kBandscopeSampleMs is 1000 ms
    // into kHeadroomMaxAgeMs's 3000 ms, so a running gate may miss two blocks
    // before a row goes absent. RadioHealthDialog::kRefreshIntervalMs is NOT
    // the reason and cannot be — a poll rate changes how soon a blink is
    // OBSERVED, never whether there is one. (This comment asserted otherwise
    // until PR #5880 review round 2.)
    //
    // AND THESE ROWS DO GO ABSENT DURING TRANSMIT. That is a consequence of
    // this expiry and it is meant. MetisClient::bandscopeInterlocked() holds
    // the gate off for m_mox, for the radio's OWN ptt, and for
    // kBandscopeUnkeyHoldoffMs after unkey, and bandscopeArm() refuses
    // silently on it — so no block arrives while keyed, and three seconds into
    // an over these four rows read as dashes (JSON null on the bridge) until
    // unkey plus the hold-off plus one gate period. The honest answer: the
    // converter is being shown our own PA rather than the band, the sensor is
    // not sampling it, and the last pre-key number presented as current is
    // precisely the fabrication this function is fixing. adcObservedAgoMs is
    // not expired with them and is what says which silence it is.
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
    // Peak-to-RMS, which is the one figure here that IS scale-free: it
    // survives the missing calibration intact, because both terms carry the
    // same unknown offset and it cancels.
    //
    // It is what separates a broadband floor from a discrete carrier — 11-12
    // dB over 2048 Gaussian samples against ~3 dB for a sinusoid — and that
    // separation is exactly what an about-zero RMS destroyed: a DC pedestal
    // inflated the denominator and dragged the reading toward the carrier end
    // whatever the antenna was doing. With the RMS now AC-referred and the
    // peak still absolute (ruled on PR #5832), a large crest means EITHER a
    // peaky signal OR a large DC offset under a quiet band; surfacing the
    // offset itself (an adcDcDbfs row) is #5856, and until it exists this row
    // cannot tell those two apart.
    //
    // NOT REPORTED, rather than fabricated, when either term is at or below
    // the floor: that constant is a sentinel meaning "below the smallest code
    // this converter has", and subtracting it invents the level it exists to
    // refuse. See Ep4Stats::crestDb(), which owns the predicate so it can be
    // tested without Qt.
    //
    // An invalid variant here is the SAME "nothing to say" this row and its
    // two neighbours already use before the first block arrives: put() keeps
    // the key in `order` and `labels` and only withholds the value, so the row
    // stays in place and reads as a dash rather than the list changing shape
    // under a reader — which was tried and reverted once already (PR #5650
    // review round 3). On the bridge it lands as a JSON null, exactly as the
    // pre-block case does, and not as a fabricated number.
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
    // the two names exist. This row is what EXPLAINS the four above going
    // absent: an operator who sees four dashes and an age of 46 810 ms knows
    // the gate stopped, where four dashes and a fifth dash says only that
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
    // THE VALIDATION BOUNDARY (Principle VII): everything in the document is
    // operator data from disk. Each field is range-checked here, once, and a
    // field that fails validation is dropped — never "fixed up" into a value
    // the operator didn't choose. Restoring NEVER keys transmit: everything
    // below is a setpoint; the TX gate (m_txAllowed / keying paths) is
    // untouched.
    // FULL RESET FIRST — and applyRestoredState({}) is a legitimate call
    // meaning "this radio has no memory": RadioModel invokes this
    // unconditionally on every engaged connect, so a same-family radio swap
    // can never leak radio A's maps OR its live members into radio B
    // (PR #4619 review, Ozy311 finding 1). Live members reset to the same
    // virgin defaults a fresh backend construction would have.
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
    m_autoGainConfig = AetherSDR::hl2::AutoGainConfig{};
    m_autoGainMode = QStringLiteral("ramp");
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
    // DELIBERATELY NOT RESET: m_micLevel and m_appliedMicGainLinear.
    //
    // Those two are not observations and not radio state — they are the
    // operator's setting on a modulator that lives on THIS HOST, and a
    // same-family swap does not rebuild it, so Hl2TxDsp genuinely still holds
    // that gain. Blanking the mirror here would make the snapshot report "not
    // reported" for a gain the modulator demonstrably has, which is the same
    // class of lie in the opposite direction — and this section exists to make
    // the applied gain checkable, not plausible. A swap that DOES rebuild the
    // backend gets a fresh pair, re-asserted by RadioModel::setupBackend().

    RestoredRadioState valid;
    if (state.rfFrequencyHz >= 100'000.0 && state.rfFrequencyHz <= 38'400'000.0)
        valid.rfFrequencyHz = state.rfFrequencyHz;
    // ACCEPTED, THEN RECONCILED — two steps, and the second one is #5755's
    // review finding (jensenpat). isKnownModeString() answers "may the document
    // say this"; canonicalOfferedMode() answers "which spelling does the menu
    // carry for it". Doing only the first left a session saved in NFM restoring
    // with the slice holding "NFM" while publishedModeStrings() no longer
    // offers it, and both mode combos rebuild with findText(currentText) and
    // move the selection only on a hit — so the operator was shown LSB with the
    // receiver in FM. That is the fault #5580 exists to remove, arriving from
    // the other direction.
    //
    // This drops nothing: NFM and FM are one WdspChannel mode (modeFromString
    // branches on them together), so the operator keeps the mode they saved and
    // only its spelling settles. It weakens no TX refusal either — every alias
    // pair is on capabilities().receiveOnlyModes both ways or neither way, and
    // hl2_mode_vocabulary_test pins that for every accepted spelling. It also
    // does the uppercasing the old comment here was about: a "cw" from a
    // hand-edited document must not round-trip into the UI.
    //
    // BEFORE the passband work below, which reads valid.mode through
    // defaultPassbandForMode() and cwBfoOffsetHz().
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
    // PRE-#4914 CW DOCUMENTS, dropped rather than replayed.
    //
    // #4914 changed what filterLowHz/HighHz MEAN for CW: they are now measured
    // from the carrier ({-250, 250}) instead of from the audio the carrier
    // becomes ({350, 850} at a 600 Hz pitch). Passband is a declared
    // clientSettingsDomain for this backend, so an operator who last quit in CW
    // has the old-domain pair on disk right now, and nothing above rejects it —
    // the pair is ordered and inside ±12 kHz.
    //
    // Replayed, dspFilterHz() adds the BFO to a value that already had it:
    //
    //     stored {350, 850} + BFO 600  ->  DSP {950, 1450}
    //     the marker's tone lands at   ->  +600 Hz
    //     600 is outside {950, 1450}   ->  silence on the marker
    //
    // and notifyOperatingStateChanged() then writes the bad pair straight back,
    // so it never heals. setSliceMode()'s default adoption does not rescue it
    // either: that fires only when the mode CHANGES, and the restore arrives
    // already in CW.
    //
    // The test is exact rather than heuristic. In the new domain a CW passband
    // must CONTAIN the carrier, because every producer builds it that way —
    // defaultPassbandForMode() returns {-250, 250} and
    // VfoWidget::applyFilterPreset builds {-w/2, +w/2}. So a CW pair sitting
    // entirely to one side of zero is a pre-#4914 document, with no false
    // positives. Drop it and let pushInitialState() derive the mode default;
    // the next capture writes the new-domain value and the document heals.
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
    // rfGain.defaultDb IS DELIBERATELY NOT READ (#5829). The fallback gain an
    // UNVISITED band comes up on is hl2::kLnaDefaultGainDb and nothing else.
    //
    // The key used to be read here into a member that was then written straight
    // back out by currentOperatingState() -- and nowhere in the tree did
    // anything else ever assign it. No setter, no verb, no GUI control. So its
    // value was a closed loop: whatever a profile happened to hold, it held
    // forever, steering every first visit to a band, with no operator action
    // able to move it. One station's profile sat at -6 dB permanently.
    //
    // Ignoring it on read and dropping it from the capture below is what makes
    // a stale value harmless: the key decays out of the document on the next
    // snapshot and the shipped constant is the single source of truth. A
    // document that still carries the key is not rejected -- it is simply not
    // consulted, which is what "authoritative" has to mean here.
    //
    // This also retires a clamp the codebase's own rule rejects. The read this
    // comment replaces qBound()ed the document value and then persisted the
    // clamped result, which is exactly what the AGC threshold restore a few
    // lines up refuses to do in its own words: clamping invents a setpoint the
    // operator never chose and then writes it back.
    //
    // IGNORED OUT LOUD, because this boundary logs every other value it
    // declines -- the pre-#4914 CW passband just above, the invalid mic level
    // just below -- and a key dropped in silence is the one an operator cannot
    // connect to what they hear. A document still carrying this key is exactly
    // a profile whose next UNVISITED band now comes up on +20 dB instead of
    // whatever was frozen in it, so the line names both numbers: the value
    // being ignored, and the value that replaces it. That is what lets a
    // support log be traced back to #5829 rather than read as a radio fault.
    if (rfGain.contains(QStringLiteral("defaultDb"))) {
        qCInfo(lcHl2) << "HL2: ignoring stale restored LNA default (#5829)"
                      << rfGain.value(QStringLiteral("defaultDb")).toVariant()
                      << "— unvisited bands come up on"
                      << hl2::kLnaDefaultGainDb << "dB";
    }

    // ARMED LATER, NOT HERE. Restore runs before the link is up, and the
    // control refuses to arm from a baseline it does not trust -- a decision it
    // cannot make until the restored baseline has actually been applied. So
    // this records the WISH and the connect edge acts on it.
    //
    // ABSENT MEANS ON, and this reverses what an earlier revision of this
    // comment argued. That argument was: "a session that predates this key
    // never armed anything, and reading a missing key as on would switch a
    // control on for an operator who never asked." It was right about the
    // mechanism and wrong about the alternative, because there was no neutral
    // ABSENT MEANS OFF. A document with no `autoEnabled` key is an operator who
    // has never expressed a preference, and they get the control switched off.
    //
    // NOT A JUDGEMENT ABOUT WHETHER THE LOOP IS GOOD -- RFC #5535 approved it
    // and asked for it armed by default. It is that arming from the shipped
    // default cannot work: this radio's constructed LNA default is +20 dB
    // (kLnaDefaultGainDb, which #5752 examined and deliberately preserved) and
    // kAutoRfGainMaxBaselineDb is +19, so the connect edge would call
    // setAutoRfGain(true), the baseline guard would refuse, and every new
    // operator would get a warning in the log about a control they never asked
    // for. Defaulting to true here would ship exactly that.
    //
    // The default-on half of #5535 waits on a trustworthy gain axis at the
    // shipped default -- the AD9866 fold reconciled against the gateware RTL or
    // replicated on a second board, or a default gain inside the trusted
    // region. An operator who wants the loop today switches it on and that
    // choice is persisted here.
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

    // The mic level, DROPPED rather than clamped when it is out of range —
    // the same rule as the AGC threshold above, and for a sharper reason. This
    // control's floor is the MUTE: clamping a hand-edited -10 would put the
    // operator silently off the air on a slider reading 0, which is the
    // readback-agrees-with-the-failure shape this backend keeps refusing. A
    // document this client cannot read must not be allowed to set a level at
    // all; leaving m_restoredMicLevel at -1 lets setupBackend()'s re-assert
    // stand, which is the operator's live slider position.
    //
    // contains() first, because toInt() answers 0 — the mute — for a missing
    // key and for "banana" alike.
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

    // The TX passband. Validated as a PAIR and adopted only if the pair is
    // sane — a half-restored passband would be a value the operator never
    // chose, which is exactly what this boundary exists to refuse. Both keys
    // must be present for the same reason: one edge restored against the other
    // edge's mode default is not the setting that was saved.
    //
    // The bounds are the modulator's, matching setTxFilter(): 24 kHz TX audio
    // gives a 12 kHz ceiling, and the edges must stay 50 Hz apart. A document
    // that fails this is dropped whole, leaving m_txFilterFromOperator false so
    // the mode derivation stays in charge.
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
    // THE CAPTURE SIDE ONLY. The remembered pair belongs to the radio whose
    // document this is, so it is reset here — otherwise a same-family swap
    // leaves radio A's AGC being written back under radio B's identity, the
    // leak applyRestoredState({}) exists to close (PR #4619 review, Ozy311
    // finding 1).
    //
    // The RECEIVERS are deliberately not touched here, and that is the whole
    // shape of #4909's second half. This function runs before EVERY connect,
    // reconnect included (RadioModel::handRestoredStateToBackend), and on a
    // reconnect m_rx still holds the live receivers — so seeding them here
    // flattened an operator's per-receiver AGC on every dropped link, no
    // matter what guard connectRadio() carried. Receiver seeding lives at the
    // one place that can tell a new radio from a returning one: connectRadio(),
    // which has the serial.
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

// Every receiver's AGC pair set to what this session should come up with.
//
// EVERY receiver, which is a deliberate difference from the mode and passband
// pushInitialState() restores onto the transmit receiver alone. Those are
// per-slice — the operator tunes each receiver to its own signal, so pushing
// one receiver's pair onto all of them would overwrite choices they made. The
// AGC is captured FLAT (currentOperatingState) precisely because it is NOT
// per-slice: it is one remembered setting. Seeding only the TX receiver would
// leave the rest on a value the operator never chose, and this is the same rule
// buildReceivers() already applies when a NEW receiver inherits the first one's
// settings rather than construction ones.
//
// The DEFAULT branch is load-bearing rather than tidiness. buildReceivers()
// deliberately carries receiver state across a rebuild, so "no memory for this
// radio" has to be written as the defaults rather than skipped — otherwise a
// same-family swap leaves radio A's AGC running under radio B's identity, the
// leak applyRestoredState({}) exists to close (PR #4619 review, Ozy311
// finding 1).
//
// ONE CALL SITE, in connectRadio(), and its condition is the point: this is a
// RESTORE, not a re-assertion, so it must run when the radio identity changes
// or when the receivers were rebuilt from nothing, and must NOT run on an
// auto-reconnect whose receivers carried their live per-receiver AGC across.
// Calling it from applyRestoredState() as well — which runs before every
// connect, reconnect included — is what made a dropped link flatten RX2.
//
// Each half applies on its own, matching the independent validation in
// applyRestoredState(): a document carrying only a threshold restores that
// threshold against the default mode.
// Seeds the STRUCT only. The channel buildReceivers() opened is still on
// Config's defaults until beginDspSetup()/pushInitialState() push the pair —
// the same open-then-configure window the mode and passband restore already
// lives with, for the same EP2-pacing reason.
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
        // THE SAME PRESERVATION RULE AS THE WRITE-BACK, and it has to be here
        // too. This snapshot is taken on a debounced store that any unrelated
        // action schedules -- a same-band tune, a mode change, a filter change
        // -- so it reaches the band map long BEFORE the first band change.
        // Protecting only rememberCurrentBandState() left the session pin free
        // to be persisted through this path: restore 20 m at -12, connect with
        // lnaGainDb=20, tune within 20 m, and the capture stored 20 for 20 m.
        // (#5402 review, Ozy311.)
        //
        // One policy, two call sites asking it -- not two copies of the rule.
        lnaByBand.insert(
            m_currentBandKey,
            AetherSDR::hl2::bandMemoryWriteback(
                m_lnaGainDb, m_lnaSessionPin,
                m_lnaDbByBand.contains(m_currentBandKey),
                m_lnaDbByBand.value(m_currentBandKey)));
        driveByBand.insert(m_currentBandKey, m_rfPowerPercent);
    }

    // THE OPERATOR'S AUTOMATIC-GAIN SWITCH, and it belongs HERE rather than in
    // an AppSettings key. docs/HERMES.md is explicit: a value the radio cannot
    // store goes in this family's OperatingState, "never in a flat AppSettings
    // key". It rides the rfGain object because that is the axis it acts on.
    //
    // THE SWITCH ONLY. The OFFSET the loop is holding is deliberately NOT
    // persisted and is absent from this object: an automatic transient that
    // outlived the session that produced it would be indistinguishable, next
    // launch, from a gain the operator chose. See m_lnaAutoOffsetDb.
    // NO "defaultDb" KEY (#5829). It was persisted here and read back in
    // applyRestoredState with nothing in the tree able to write it, so a
    // profile's value was frozen for the life of that profile and decided the
    // gain of every first band visit.
    // hl2::kLnaDefaultGainDb is now the only answer to that question; see
    // applyRestoredState(). Dropping the key here is the half that heals an
    // existing document, because the next capture writes the object without it.
    QJsonObject rfGain{{QStringLiteral("lnaDbByBand"), lnaByBand},
                       // THE WISH, NOT THE RUNNING FLAG. m_autoRfGainEnabled is
                       // whether the loop is running right now; m_autoRfGainWanted
                       // is whether the operator wants it to. Those differ exactly
                       // when the backend DECLINED to arm -- refusing from a
                       // baseline where the gain axis is not trustworthy -- and
                       // Hl2Backend.h states the invariant that makes the
                       // difference matter: "the wish stays true, the control
                       // stays off, and the next connect from a baseline it
                       // trusts honours the operator without them having to ask
                       // twice."
                       //
                       // Persisting the running flag broke that in the one
                       // direction that cannot recover: a declined session wrote
                       // an explicit false, and an explicit false is honoured
                       // forever -- so the operator lowering their RF Gain into
                       // the trusted region would never see the loop arm again,
                       // and would have no way to know why.
                       {QStringLiteral("autoEnabled"), m_autoRfGainWanted}};
    QJsonObject txSetpoints{{QStringLiteral("driveByBand"), driveByBand}};
    if (m_driveDefaultPercent >= 0)
        txSetpoints.insert(QStringLiteral("defaultPercent"), m_driveDefaultPercent);

    // The operator's TX passband — the Phone applet's low-cut / high-cut.
    //
    // FLAT, not per-band or per-mode, unlike the drive and LNA maps beside it.
    // That is a deliberate difference and worth stating, because the neighbours
    // set the opposite expectation.
    //
    // The control is ONE pair of sliders. Persisting it per band or per mode
    // would make those sliders move on their own: change band without touching
    // them and the displayed cut points would jump to that band's remembered
    // pair. That is the same class of surprise as a mode change silently
    // replacing the passband, which is the bug m_txFilterFromOperator exists to
    // prevent — reintroducing it on a different axis would be a poor trade.
    //
    // Per-mode specifically buys nothing here: effectiveTxPassband() only honours
    // the override for USB and LSB, and an operator's voice is the same voice on
    // both. What genuinely varies between a ragchew and a DX pileup is the whole
    // audio chain, not one filter pair, and mic profiles are the surface for that.
    //
    // ONLY WRITTEN ONCE THE OPERATOR HAS CHOSEN. Persisting the mode-derived
    // default would make the next connect look like an operator override and
    // permanently suppress the per-mode derivation.
    if (m_txFilterFromOperator) {
        txSetpoints.insert(QStringLiteral("filterLowHz"), m_txFilterLowHz);
        txSetpoints.insert(QStringLiteral("filterHighHz"), m_txFilterHighHz);
    }

    // The Phone/CW MIC slider. FLAT, like the TX passband above and unlike the
    // drive map beside it: the right mic level is a property of the operator's
    // voice and their microphone, not of the band they are on.
    //
    // WHY THIS RADIO REMEMBERS IT AND OTHERS MUST NOT. On a host-modulating
    // backend this gain exists nowhere but this host — the HL2 has no mic-gain
    // register and keeps nothing across a power cycle, so the client is the
    // only memory there is. It is also the operator's only control over where
    // their audio lands relative to the ALC's hold threshold, which setKeying()'s
    // unkey diagnostic calls a silent cliff, so an operating procedure that
    // begins "set mic gain once" is defeated by a control that forgets
    // overnight.
    //
    // A Flex and an Icom persist mic gain IN THE RADIO (Constitution II/III) —
    // on an Icom setMicGain is a live CI-V 14 0B / LAN MOD write into a front-
    // panel setting. Neither declares ClientSettingsDomain::TxSetpoints, so
    // neither reaches this document at all, in either direction. That is the
    // whole reason the level rides this radio's own extension sub-object rather
    // than a shared field or a flat AppSettings key: the gate is structural,
    // not a family string somebody has to remember to check.
    //
    // UNCONDITIONAL, unlike the passband pair above. There is no "the operator
    // has not chosen" state to protect here: 50 is the position a radio with
    // nothing stored comes up on, and writing it back is a no-op that restores
    // to the same place. The mute at 0 is a real choice and must round-trip, so
    // it cannot be filtered out as "absent" — see the -1 sentinel on
    // m_restoredMicLevel.
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

// Register write, dB-reference lockstep, and the every-pan echo — all three on
// the EFFECTIVE value, never on the baseline.
//
// The dB reference moves IN LOCKSTEP with the gain: spectrum and S-meter are
// both rendered through m_dbRef, so without this every gain change would
// slide the whole trace — an operator backing off 10 dB would watch the
// noise floor drop and read it as the band going quiet. That argument applies
// to an automatic step exactly as it does to a manual one, which is why the
// offset goes through here rather than round it.
// Arm or disarm the automatic control. RADIO-WIDE: there is one AD9866.
void Hl2Backend::setAutoRfGain(bool on)
{
    // A DECLINED ARM IS A THIRD STATE, and a guard on the running flag alone
    // can only see two. The refusal path leaves the loop off with the wish
    // recorded (m_autoRfGainWanted true, m_autoRfGainEnabled false) on purpose,
    // so that the asking survives a decline -- and an explicit "off" from there
    // matched `on == m_autoRfGainEnabled` and returned before the disarm branch,
    // the only writer of m_autoRfGainWanted = false. currentOperatingState()
    // persists the WISH, so the withdrawal never reached the profile and the
    // next connect from a baseline the loop trusts armed a control the operator
    // had switched off (#5828).
    //
    // ON BOTH FLAGS RATHER THAN A SECOND DISARM PATH BEFORE THE GUARD. The
    // withdrawal is not a different event from a disarm; it is a disarm of a
    // loop that happens not to be running, and it owes the caller exactly what
    // a disarm owes: the wish cleared, the refusal reason dropped, a settled
    // verdict emitted and the document republished. Hand-copying that subset
    // into a second exit is what produced a "successful off" that went on
    // reporting "declined" through the bridge reply and the checkbox's
    // accessible description. One exit, one list.
    //
    // AND EVERY SIDE EFFECT OF THE DISARM BRANCH IS INERT FROM THE REFUSED
    // STATE, which is what makes routing it there safe rather than merely
    // tidier. m_autoRfGainEnabled is already false. applyBandscopeForAutoGain()
    // computes `wanted` false and returns at `if (!m_bandscopeOwnedByAutoGain)`,
    // because that flag is set only where m_autoRfGainEnabled was true.
    // m_autoGainReason is already Disarmed -- its member default, and the value
    // every route out of a running loop leaves behind. m_autoGainState is
    // default-constructed for the same reason. setLnaAutoOffsetDb(0) returns at
    // its own `requested == m_lnaAutoOffsetDb`, the offset being 0 unless the
    // loop ran. The one thing that was NOT inert is the log sentence, and it is
    // conditioned below.
    //
    // THE COMBINATION THIS WIDENING NEWLY ADMITS TO THE ARM BRANCH -- on true,
    // enabled true, wanted false -- IS UNREACHABLE: m_autoRfGainEnabled = true
    // is written in exactly one place and the next statement sets
    // m_autoRfGainWanted = true, while every writer of wanted = false (the
    // disarm below, and applyRestoredState) has already set enabled false.
    if (on == m_autoRfGainEnabled && on == m_autoRfGainWanted) {
        return;
    }
    if (on) {
        // REFUSED, NOT CLAMPED. See kAutoRfGainMaxBaselineDb. Moving the
        // operator's own number so the feature could be switched on would be a
        // UI reporting one value while the wire carried another.
        if (m_lnaGainDb > kAutoRfGainMaxBaselineDb) {
            // THE ASKING SURVIVES THE REFUSAL. This is what makes the invariant
            // on m_autoRfGainWanted true rather than merely stated: the
            // operator asked, the radio declined on its own evidence, and the
            // request is what is persisted -- so the next connect from a
            // baseline the loop trusts arms without them having to ask twice.
            //
            // Recorded BEFORE the return, which is where it was missing. The
            // disarm branch below already says "a REFUSAL does not reach here,
            // so a radio that declined to arm keeps the operator's on
            // recorded"; without this line nothing had recorded it.
            m_autoRfGainWanted = true;
            // COMPOSED ONCE AND KEPT, because the operator needs it more than
            // the log does. Reading isArmed() back tells the GUI THAT this
            // declined; only this sentence says why, and it already names the
            // baseline, the ceiling and the remedy. Storing it is what lets the
            // checkbox explain itself instead of springing back in silence
            // (#5817).
            //
            // tr(), AND WITHOUT THE ISSUE NUMBER, because this sentence stopped
            // being a log line the moment it was kept: it is shown on the
            // panadapter and read out by a screen reader. "#5354" is provenance
            // for us and noise to an operator, so it stays on the qWarning --
            // which is where the next person debugging this actually looks --
            // and the operator gets the baseline, the ceiling and the remedy.
            m_autoRfGainRefusal = tr(
                       "Auto RF gain declined — the RF Gain baseline is "
                       "%1 dB and this radio's gain axis is not trusted above "
                       "%2 dB. Lower RF Gain to %2 dB or below and try again. "
                       "Your setting has not been changed.")
                       .arg(m_lnaGainDb)
                       .arg(kAutoRfGainMaxBaselineDb);
            qWarning().noquote()
                << QStringLiteral("Hl2Backend: ") + m_autoRfGainRefusal
                     + QStringLiteral(" (#5354: +48 dB measures like +18 dB)");
            // SETTLED AS NOT ARMED, and said so. A refusal that only the
            // caller's own readback could discover was invisible on the two
            // routes that have no readback: the restore below and the bridge.
            emit autoRfGainArmSettled(false);
            // THE SURVIVING ASK IS PERSISTED STATE, so it moves the document and
            // has to say so. Without this the wish lives only in this process and
            // the "arms on the next connect that allows it" promise above holds
            // only until the application is closed -- and, worse, it makes the
            // withdrawal above untestable: a profile that never recorded the true
            // reads false afterwards whether or not the withdrawal works.
            //
            // ORDERED AFTER autoRfGainArmSettled, to match the arm and disarm
            // branches: both emit the settled verdict inside the branch and reach
            // the shared notifyOperatingStateChanged() at the tail afterwards.
            // The flag this publishes is already set above, so nothing observable
            // turns on the order -- only the uniformity does, and a handler of
            // the verdict should not see one path's document refreshed and the
            // other two's not.
            //
            // AND THIS SCHEDULES A WRITE ON EVERY DECLINED ASK, so the rate is
            // worth knowing before anything new is wired to this path. Today it
            // is bounded by who asks: the linkUp handler once per connect, the
            // operator once per click, the bridge once per verb --- and
            // RadioModel::scheduleOperatingStateSave() coalesces behind a
            // debounce, so a burst is one write. A PERIODIC RE-ARM ATTEMPT would
            // turn this into a timer-driven save of the whole operating-state
            // document; if one is ever added, give it its own guard rather than
            // letting it inherit this line.
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
        // THE REASON DIES WITH THE REQUEST IT DESCRIBED, and this is the only
        // place that can kill it: it is otherwise cleared on a successful arm
        // and on applyRestoredState, so an off that followed a decline left
        // lastArmRefusalReason() returning "Auto RF gain declined ..." about an
        // attempt the operator had already abandoned. IAutoRfGainControl defines
        // that accessor as empty when the last attempt succeeded, and an off
        // that took is an attempt that succeeded. Three readers repeat it
        // otherwise: the bridge's `pan autorfgain` reply and every later status
        // report, and MainWindow's per-pan catch-up, which writes it as the
        // checkbox's ACCESSIBLE DESCRIPTION -- so a screen-reader user was told
        // "declined" about a switch they had turned off.
        //
        // BEFORE THE EMIT BELOW, NOT AFTER. MainWindow::onAutoRfGainArmSettled
        // reads lastArmRefusalReason() on entry when `armed` is false; emitting
        // first would hand it the stale sentence and re-show the refusal card on
        // a plain off.
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

// Which of Hl2AutoGainPolicy.h's configurations the loop runs. Applied live:
// the state is NOT reset, because the offset the radio is actually holding is
// real whichever law asked for it, and the policy's own ceiling check gives
// back any excess on its next evaluation, in one step.
bool Hl2Backend::setAutoRfGainMode(const QString& mode)
{
    using namespace AetherSDR::hl2;
    const QString m = mode.trimmed().toLower();
    AutoGainConfig cfg;
    if (m == QLatin1String("ramp") || m == QLatin1String("default")) {
        cfg = AutoGainConfig{};
    } else if (m == QLatin1String("probe") || m == QLatin1String("probing")) {
        cfg = probingReleaseConfig();
    } else if (m == QLatin1String("binary")) {
        cfg = binaryHighLowConfig();
    } else if (m == QLatin1String("bandscope")) {
        // THE DEFAULT, and the one law here whose release condition rests on a
        // measurement rather than on a gamble. The bias budget is computed from
        // the gate period MetisClient actually runs, never from a literal, so
        // the two cannot drift apart.
        cfg = bandscopeReleaseConfig(
            gatedPeakBiasDbForPeriod(MetisClient::bandscopeSamplePeriodMs()));
    } else {
        qWarning().noquote()
            << QStringLiteral("Hl2Backend: auto RF gain mode \"%1\" is not one of "
                              "bandscope|ramp|probe|binary. The law has not been "
                              "changed.")
                   .arg(mode);
        return false;
    }
    m_autoGainConfig = cfg;
    m_autoGainMode = (m == QLatin1String("default")) ? QStringLiteral("ramp")
                   : (m == QLatin1String("probing")) ? QStringLiteral("probe") : m;
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

// THE GATE THE MEASURED RELEASE DEPENDS ON, armed and released with the law
// that needs it.
//
// A law with requireHeadroomToRelease set and no bandscope running would hold
// its offset forever and report HeadroomAbsent, which is safe but is not a
// feature. So arming such a law arms the gate.
//
// IT DOES NOT STOMP A MANUAL ENABLE. An operator who turned the bandscope on
// through the `bandscope.enable` extension owns it; this only ever releases a
// gate THIS function started, which is what m_bandscopeOwnedByAutoGain records.
// The alternative -- disarming the loop silently killing a diagnostic stream
// the operator started for their own reasons -- is the kind of surprise the
// extension's own comment exists to avoid.
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

    // THE WIDEBAND HEADROOM READING, from the newest accepted bandscope block.
    //
    // Classified HERE rather than in the policy because the age is a clock
    // reading and Hl2AutoGainPolicy.h owns no clock -- the same division
    // m_adcOverloadClock already observes for the overload warning.
    //
    // `m_bandscopeBlock.samples == 0` means no block has ever arrived, which
    // bandscopeHeadroom() turns into Absent rather than into a level. An
    // invalid clock is passed as a negative age for the same reason: "never
    // observed" and "observed too long ago" are both Absent, and neither is a
    // headroom of zero.
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
    // THE AGC CEILING IS THE OTHER HALF OF THAT LOCKSTEP, and it is the half
    // the operator hears rather than sees. WDSP's maximum gain is a setpoint
    // about the antenna signal applied to a POST-LNA one, so moving the LNA
    // without moving the ceiling changes how far into the noise the AGC
    // chases — the display holds still and the band floor in the headphones
    // does not. Hl2DbReference::agcCeilingDb() refers it; this is where every
    // live receiver is told the referred value.
    //
    // The operator's own 0..100 is NOT touched. It is their judgement about
    // the signal at the antenna and it stays exactly where they left it; only
    // the derived ceiling moves, which is why nothing here emits a slice
    // change or triggers a state capture.
    //
    // Queued, and per receiver, for the same reason setSliceAgc is: the DSP
    // objects live on the I/O thread. Receivers with no DSP yet (pre-connect,
    // or a chain that failed to open) are skipped — they pick the referred
    // ceiling up from agcCeilingDb() when their Config is assembled.
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
    // THE RADIO REPORTS NO VFO, SO THE APP IS AUTHORITATIVE AND MUST PUSH.
    //
    // A Hermes-Lite 2 has no state to read back: it never tells us its
    // frequency, mode or drive. Every register simply retains whatever the last
    // session left in it. So anything not explicitly asserted here is silently
    // inherited from a previous connection, and the UI will confidently display
    // something the hardware is not doing.
    //
    // That is not hypothetical. The TX NCO was set only when the operator
    // retuned, so on reconnect the receiver moved to the app's frequency while
    // the TRANSMITTER stayed on the previous session's — the VFO read 10 MHz
    // and the radio transmitted on 14 MHz. Nothing in the app could have shown
    // that, because nothing in the app was wrong.
    //
    // The rule for anything added later: if the radio cannot be asked for it, it
    // belongs here.
    if (const Receiver* txRx = rx(m_txDdc))
        setTxFrequency(txRx->sliceFreqHz);

    // NOT the drive level. connectRadio() already asserts a safe 0 before the
    // link comes up, and by the time this runs RadioModel has pushed the
    // operator's actual RF power — emit connected() above is synchronous, so
    // resetting here silently undid it and the radio transmitted at drive 0
    // with the PA disabled. Caught by measurement: forward power went to 0.

    // Derive each receiver's passband from its MODE, not from its stored values.
    //
    // The defaults (150..3000) correspond to no mode at all — they happen to
    // equal the unmapped-mode fallback — so a fresh connect in the default USB
    // left the radio with DIGU's passband while the mode indicator read USB. Same
    // category as the mode-change stickiness in docs/HERMES.md 15.7: mode and passband
    // must agree, and CONNECT is a place they can disagree just as easily as a
    // mode change. (#4484)
    //
    // Found by radiocert's mode-map stage: 150..3000 for USB at connect,
    // 100..2900 for the same mode once any other mode had intervened.
    //
    // OUTSIDE the per-receiver dsp guard below: these are the backend's OWN
    // values, published by emitAllSliceState() and read by sliceDetail(), so a
    // receiver whose DSP failed to open would still have the UI told 150..3000
    // for USB — the very bug this fixes.
    //
    // EVERY receiver, not just the first: each carries its own mode, so each can
    // disagree with its own passband independently.
    //
    // ONCE PER CONNECT, not once per linkUp. pushInitialState() runs on every
    // linkUp, and MetisClient re-emits that after a silence timeout without any
    // new connectRadio(): onWatchdogTick() clears m_linkUp on EP6 silence while
    // m_running stays true, then resuming EP6 fires linkUp again. Deriving
    // unconditionally there would reset an operator's own filter edit — say
    // 300..2400 on USB — after a few seconds of packet loss, which contradicts
    // the override-preservation rule setSliceMode() documents ("adopted on
    // CHANGE only, so an operator's own filter edit survives"). A reconnect is a
    // new session and should re-derive; a transient glitch is not.
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
    }
    if (m_txDsp) {
        const Receiver* txRx = rx(m_txDdc);
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode,
                  modeFromString(txRx ? txRx->mode : QStringLiteral("USB"))));
        QMetaObject::invokeMethod(m_txDsp, "reset", Qt::QueuedConnection);
    }

    // THIS RADIO'S REMEMBERED MIC LEVEL — the operator's slider position from
    // the last session on this same HL2, staged by applyRestoredState() for one
    // connect-time application.
    //
    // HERE rather than in applyRestoredState() because m_txDsp is only built by
    // connectRadio(), and here rather than above the seam because this is the
    // one family the value belongs to. It runs AFTER
    // RadioModel::setupBackend()'s re-assert of the live slider, which is the
    // correct order: the re-assert exists to keep a mid-session family swap
    // from parting the slider from the modulator, and a connect that has a
    // document for THIS radio knows better than the model does.
    //
    // NOTHING HAPPENS WITH NOTHING STORED. A fresh radio leaves the sentinel at
    // -1 and the seam's answer stands, so the operator who has never touched
    // the control still transmits at the 50 that maps to the modulator's own
    // 1.0 default — byte-identically to before this code existed.
    //
    // THE SLIDER FOLLOWS, and that is not optional. setMicGain() moves the
    // modulator only; without the echo the control would read the model's
    // stale position while the radio transmitted at the restored one, which is
    // the lying-readback failure inverted. transmitChanged is the observed-state
    // route RadioModel already owns (applyBackendTransmitDelta) — the same one
    // the band-memory drive push uses above — so this needs no new wiring and
    // creates no second source of truth.
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
    // How far this pan may be zoomed, which on this radio is simply the range of
    // DDC rates it can run. Pushed here for the same reason everything else in
    // this function is: nothing above the seam can derive it. The GUI's fallback
    // clamp is a FlexLib model table, and "Hermes-Lite 2" falls through it to
    // 5.4 MHz — fourteen times more than the widest window this receiver has, so
    // the operator could zoom out into spectrum that was never sampled and the
    // uncovered part rendered as black bars.
    //
    // The UPPER limit falls as receivers are added, because the span and the
    // receiver count draw on the same 100BASE-T budget: four receivers cannot
    // run at 384 kHz. Reporting the unreduced maximum would leave the operator
    // able to zoom to a span applyPanBandwidth() then refuses, which reads as a
    // broken control rather than a hardware limit.
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
    if (m_lastTxOperation.permitsCleanup()) {
        setKeying(false, m_lastTxOperation);
    }
    // AND THE RECEIVE-AUDIO HOLD WITH IT — AFTER the cleanup unkey above, never
    // before it.
    //
    // While the mixer gated on m_keyed, clearing m_keyed above was enough;
    // since #5497 it gates on m_rxAudioMuted, which that line does not touch. A
    // session that ended mid-transmission would otherwise hand the new one a
    // closed gate — and the setKeying(false) above is conditional, so on the
    // path where it is skipped nothing would ever reopen it. Hence
    // unconditional, and cleared rather than deferred: the T/R turnaround this
    // hold exists to cover belonged to a transport that no longer exists.
    //
    // THE ORDER IS THE POINT, AND IT USED TO BE THE WRONG WAY ROUND. This block
    // stood ABOVE the setKeying(false) — posting the unmute ahead of a queued
    // MOX-off, which is the exact posting order this change removes from
    // applyKeying(), reproduced on the relink path. It was not a live
    // regression: EP2 stops when the link drops, the gateware's own watchdog
    // halts the transmission, and the PA is long down before a relink runs. But
    // ordering is what #5497 is about, and a counter-example sitting in the
    // same file is what a future reader copies (ten9876, #5850 review). Costs
    // nothing to put right.
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
    // The gain the ALC is applying — the companion to meter 7, not a second
    // form of it. Seven is the post-ALC LEVEL and sits near the target
    // whatever the operator does; this is how hard the stage is working to put
    // it there, and it is the half that moves when a mic is too quiet.
    //
    // The range is the modulator's own, not a display preference. The TOP is
    // 0 dB because that is the stage's ceiling: the ALC may only reduce, so
    // alcGainDb() cannot report a positive number and anything above zero
    // would be face the needle can never reach. It read +40 until the makeup
    // half was removed, where the top was Hl2TxDsp::Config::alcMaxGainDb;
    // deleting that field without moving this would have left #5636 inheriting
    // a meter pinned in the bottom third of its own scale.
    //
    // The bottom is reduction, which has no configured limit — the loop
    // reduces toward alcTargetPeak/blockPeak — so -20 is a PRESENTATION floor
    // rather than a measured one, wide enough for the reductions this chain
    // produces on real audio. The largest figure recorded anywhere in the tree
    // is the -21.41 dB in processAudioBlock's own comment, which is a
    // full-scale client-leveled block and not speech; that lands just off the
    // bottom of the face and reads "hard down", which is the right answer.
    //
    // sourceIndex stays at its default 0 for the same reason COMPPEAK's does:
    // one transmitter, so it lands in MeterModel's by-slice map under the
    // implicit slice rather than the explicit TX-waveform map.
    def(9, QStringLiteral("TX"),  QStringLiteral("ALCGAIN"), QStringLiteral("dB"),
        -20.0, 0.0,    QStringLiteral("Gain the ALC is applying"));
}

void Hl2Backend::publishTelemetry(const Hl2Telemetry& t)
{
    // Forward/reverse power are UNCALIBRATED ADC counts. MeterModel's FWDPWR
    // path expects dBm and converts to watts, so publishing a raw count there
    // would render as a confident, wrong wattage. Until there is a per-unit
    // calibration curve (oracle §6 is explicit that Quisk and SparkSDR both
    // build one, and that raw counts must not be presented as watts), only the
    // quantities that are actually meaningful get published.
    //
    // SWR is meaningful WITHOUT calibration because it is a RATIO — but of two
    // linearized readings, not of two raw counts. The unknown SCALE cancels in
    // a raw ratio; the detector's CURVE does not, and taking the raw ratio read
    // optimistically low at low drive (#4578). swrFromRaw() maps both counts
    // through detectorVolts() first; see its comment for the whole argument.
    // SWR only means something with real forward power behind it.
    //
    // Measured on the live radio: with no carrier the forward and reverse counts
    // are both near zero and dominated by noise, reverse frequently exceeds
    // forward, and the computed ratio saturated the meter at 255.99:1 — a
    // dramatic reading of nothing at all. An operator glancing at that sees a
    // catastrophic mismatch on an antenna that is fine.
    //
    // The threshold is in raw counts because that is what we have; it is a
    // noise floor, not a calibrated power level. It lives in MetisProtocol.h,
    // beside the calibration curve its value is derived from, so the Radio
    // Health snapshot applies the SAME floor — see kMinForwardCountsForSwr.
    if (t.forwardPowerRaw && t.reversePowerRaw
        && *t.forwardPowerRaw >= kMinForwardCountsForSwr) {
        if (const auto swr = swrFromRaw(*t.forwardPowerRaw, *t.reversePowerRaw))
            emit meterUpdate(QStringLiteral("TX:SWR"), *swr);
    }
    // Forward and reverse power, through the reference curve in
    // directionalWatts(). UNCALIBRATED — see that function for exactly what
    // that means and why publishing an approximate value still beats publishing
    // none. The raw counts continue to be logged alongside, because they are
    // what a per-unit calibration will be built from and they are the only way
    // to tell "the radio reports no power" from "we never asked".
    //
    // Arrives at the 10 Hz MetisClient already paces telemetry at
    // (kTelemetryMinIntervalMs), so no further rate gate is needed here;
    // MeterModel applies its own forward-power ballistics on top.
    //
    // Published through the peak hold, not raw. See kFwdPeakReleaseAlpha for
    // why a 10 Hz instantaneous sample of a speech envelope reads ~10 dB low
    // and what the hold does and does not recover.
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
    // TX IQ FIFO — THE RADIO'S, not ours. `fill` is the top 7 bits of the
    // gateware's DSIQ level, 0-127, not a sample count; `pacingFault` is the
    // one flag the gateware sends for both underrun and blocked writes.
    //
    // WHAT THIS PAIR CANNOT TELL YOU, corrected here because the comment that
    // stood in this place said the opposite and a healthy reading was being
    // taken as evidence against a defect it is structurally blind to:
    // it does NOT report a starvation of the CLIENT's queue
    // (MetisClient::m_txIq). That queue running dry does not drop an EP2
    // packet or shorten one — MetisClient::onEp2PacerTick emits a full-size
    // frame off the wall clock either way and ep2WriteTxIq zero-fills the
    // samples that were not supplied — so the radio is handed an unbroken
    // 48 kHz sample stream whose CONTENT is partly silence, and its FIFO fill
    // is identical in both cases. The host-side fault has its own counters:
    // MetisClient::txUnderflowPackets, ::txUnderflowSamples and
    // ::txOverflowSamples, logged under this same category from the I/O
    // thread that owns them.
    //
    // So these two readings answer "did the audio reach the radio late or in
    // bursts", and the client's counters answer "was there any audio to send".
    // A fault in either one is invisible to the other.
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
    // Rate-limited, not merely edge-gated. The edge gate above is necessary and
    // was never sufficient: the comparator genuinely chatters on a strong band,
    // so nearly every telemetry sample is an edge and one message repeats at the
    // full telemetry cadence (see the members' comment in the header for the
    // rate, and for why the historical figure there is not repeated as a
    // current one).
    //
    // Deliberately OUTSIDE the edge test, and this is the whole reason the two
    // are separate: a burst that stops must still report its tally. Flushing
    // only on the next edge would hold the count until the band goes loud
    // again, which could be hours away or never. publishTelemetry runs on every
    // telemetry update, so the window closes on time whether or not the
    // condition is still happening.
    //
    // Reported rather than dropped because the rate IS the severity here — a
    // flag that sets once is a hint, one that sets on every sample for a minute
    // is a front end being slammed.
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
    // THE ONE PLACE either edge of the throttle reaches the wire.
    //
    // It exists because the guard below was originally written into the
    // trailing edge only, and the leading edge — the commoner path — silently
    // lacked it. Two call sites that must agree about a hardware safety
    // condition is one call site too many, so both now go through here and the
    // asymmetry cannot come back.
    //
    // Do not let a disconnected tune enqueue work for a future session.
    // The MetisClient guard and stop-time purge also enforce this at the wire.
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
    // Called on linkDown. The timer's armed/pending state is about a session:
    // left running across a disconnect, a reconnect inside the residual window
    // takes the coalescing branch and stores the connect-time frequency as
    // PENDING instead of pushing it — delaying the board by up to the cooldown
    // at exactly the moment linkUp() intends an immediate push.
    //
    // The band key is cleared too, so the first push of the next session is
    // always treated as a band change and takes the leading edge. Assuming the
    // previous session's band still applies is precisely the assumption that
    // cannot be made across a disconnect.
    if (m_ioBoardThrottle)
        m_ioBoardThrottle->stop();
    m_ioBoardSchedule.reset();
    m_ioBoardBandKey.clear();
}

// How old the mirrored bandscope block is, in the one encoding every consumer
// of it already expects: a NEGATIVE age means "never observed", which is what
// bandscopeBlockIsCurrent() and bandscopeHeadroom() both turn into Absent
// rather than into a headroom of zero.
//
// ONE DEFINITION, because there are three readers — the auto-gain release rows,
// the converter rows and stepAutoGain() — and this expression was written out
// at all three. That is the same drift shape PR #5880 removed from the
// predicate itself; leaving it in the argument would have re-opened it one
// level down.
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

    // ONE filter board, N receivers.
    //
    // The J16 open-collector byte is a radio-wide register: there is a single
    // relay bank in the antenna path ahead of the single ADC. With one receiver
    // "the filter for the slice frequency" was a complete answer. With four it
    // is not, because four receivers can sit on four different bands and the
    // hardware has one opinion.
    //
    // The policy is AGREE-OR-BYPASS. If every active receiver wants the same
    // filter, engage it — the common case, since an operator usually spreads
    // slices within a band. If they disagree, release every relay (kOcNone)
    // rather than pick a winner.
    //
    // Picking a winner is the tempting alternative and it is worse: a low-pass
    // chosen for 40 m ATTENUATES a receiver listening on 15 m, so three of the
    // four panadapters would show a signal level that is an artefact of the
    // fourth receiver's tuning. Bypass is honest — every receiver sees the same
    // unfiltered front end, and a level comparison between panadapters means
    // something.
    //
    // WHAT THIS COSTS, stated plainly: bypass drops the AM-broadcast HPF, and on
    // the HL2 that filter matters more than on radios with better dynamic range
    // (oracle §8). Near a broadcast transmitter, spanning bands can therefore
    // raise the noise floor on EVERY receiver. That is a real trade the operator
    // makes by putting receivers on different bands, and it is logged below so
    // the cause is visible rather than mysterious.
    //
    // TRANSMIT is not affected by the bypass decision, because transmit is what
    // the low-pass is legally there for. See the TX-receiver override below.
    int oc = -1;
    bool spanned = false;
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        const int want = static_cast<int>(ocFilterByteForHz(m_rx[i].sliceFreqHz));
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
        if (m_keyed || m_tuning) {
            const Receiver* txRx = rx(m_txDdc);
            oc = txRx ? static_cast<int>(ocFilterByteForHz(txRx->sliceFreqHz))
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
    // WHAT THIS RADIO ACTUALLY DEMODULATES, published so the UI stops offering
    // what it does not.
    //
    // modeFromString() FALLS BACK TO USB for anything it does not recognise, so
    // selecting RTTY, DFM or DSTR on an HL2 put the receiver in USB while every
    // readback agreed the mode was RTTY -- the slice keeps the string it was
    // given. The operator sees a mode they chose and hears a mode they did not,
    // and nothing in the path disagrees with them.
    //
    // publishedModeStrings(), not knownModeStrings(): a subset of the same
    // source, so the menu can never offer a mode the restore boundary would
    // REJECT, while the boundary stays free to accept spellings the menu has no
    // business showing. See publishedModeStrings() for which three groups those
    // are and why.
    d.modeList = publishedModeStrings();
    d.frequency = r->sliceFreqHz / 1.0e6;   // MHz
    d.mode = r->mode;
    d.filterLow = r->filterLowHz;
    d.filterHigh = r->filterHighHz;
    d.audioGain = qRound(r->audioGain * 100.0f);
    d.audioMute = r->audioMuted;
    // The AGC pair the DSP is actually running.
    //
    // Never published before, which is why a RESTORED AGC would have been
    // invisible: the backend would have come up on the operator's slow/40 and
    // the RX Controls applet would have gone on showing SliceModel's own
    // construction defaults (med/65) — the classic HERMES §17 shape in reverse,
    // where the radio is right and the control lies about it (#4909).
    //
    // Safe to echo unconditionally: SliceModel::applyDelta() assigns these
    // without emitting agcCommandIssued, so a published value cannot come back
    // as a command (Principle II).
    d.agcMode = r->agcMode;
    d.agcThreshold = r->agcThresholdDb;
    // The HL2 has one transmitter however many receivers it runs, so EXACTLY ONE
    // slice is the transmit slice — the one on m_txDdc.
    //
    // Publishing this is load-bearing rather than informational: leaving it
    // unset meant txSlice() was null and every key attempt died in RadioModel's
    // interlock with "No transmit slice is assigned", before the backend was
    // ever asked, which is why the refusal was silent from down here.
    //
    // Marking every slice as the TX slice would be worse than marking none: the
    // interlock would then find a transmit slice whichever one happened to be
    // selected, and the operator could key from a receiver whose frequency the
    // transmit NCO is not following.
    d.txSlice = (ddc == m_txDdc);
    // EXACTLY ONE slice is active, for the same reason exactly one is the TX
    // slice. This was an unconditional `true`, correct while there was only ever
    // one slice to be active and wrong the moment there were two: every slice
    // then claimed it, and anything resolving "the active slice" got whichever
    // one it happened to look at first.
    //
    // What that looked like: tuning across a panadapter moved the right DDC and
    // its VFO flag showed the right frequency, while the RX Controls applet —
    // which follows the ACTIVE slice — stayed pointed at a different receiver.
    // Two slices claiming to be active is indistinguishable from none, and the
    // applet had no way to tell which pane the operator was working on.
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
