#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Bands.h"

#include <QJsonObject>

#include <cmath>
#include <limits>

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2TxDsp.h"
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

#include <cstdint>
#include <utility>

// Hl2Backend.h seeds m_alcHoldBelowDbfs with a literal because it can only
// forward-declare Hl2TxDsp. This is what stops the two drifting: the seed has
// to keep meaning "the modulator's own default" for a pre-connect snapshot to
// be honest, and a silent divergence is exactly the class of readout error this
// backend's health section exists to eliminate.
static_assert(AetherSDR::hl2::Hl2TxDsp::Config{}.alcHoldBelowDbfs == -45.0,
              "Hl2Backend.h seeds m_alcHoldBelowDbfs with this value — keep them equal");

Q_LOGGING_CATEGORY(lcHl2Tx, "aether.hl2.tx")

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

// The IQ rates the HL2's DDC can be told to run, ascending. ONE list, because
// this is simultaneously the capability advertisement, the panadapter's zoom
// limits, and the set a zoom request snaps to — and on this radio those are the
// same fact. The pan span IS the sample rate (Hl2Backend::emitPanState), so a
// second list would be a way for the advertised span and the deliverable span to
// drift apart, which is exactly the failure being fixed here.
constexpr int kIqSampleRatesHz[] = {48000, 96000, 192000, 384000};

// Minimum forward-power reading, in raw converter counts, below which an SWR
// ratio is noise rather than a measurement.
//
// With no carrier, forward and reverse are both near zero and dominated by
// noise; reverse frequently exceeds forward and the ratio saturates. An
// operator glancing at that sees a catastrophic mismatch on an antenna that is
// fine. Raw counts because that is what we have — this is a noise floor, not a
// calibrated power level.
//
// File-scope so EVERY consumer shares one threshold. It was previously local to
// the meter path, so the Radio Health snapshot computed an unguarded ratio and
// bounced at its 500 ms refresh while the meter beside it stayed silent — two
// surfaces disagreeing about the same radio because only one of them had the
// guard.
constexpr int kMinForwardCountsForSwr = 16;

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

// Is `mode` a name modeFromString() genuinely maps (rather than falling back
// to USB)? The restore boundary uses this so a corrupt document's mode string
// is dropped instead of reaching Receiver::mode, the UI, and — via capture —
// re-persisting itself (PR #4619 review, Ozy311).
bool isKnownModeString(const QString& mode) noexcept
{
    static const QStringList kKnown = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("DSB"),
        QStringLiteral("CWL"), QStringLiteral("CWU"), QStringLiteral("CW"),
        QStringLiteral("FM"),  QStringLiteral("NFM"), QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("SAM"),
        QStringLiteral("DRM"), QStringLiteral("WBFM"), QStringLiteral("WFM"),
    };
    return kKnown.contains(mode.toUpper());
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
    // CW: 500 Hz around the conventional 600 Hz pitch. The pitch itself is a
    // client-side setting the backend is not told about, so this is the
    // default-pitch case; an operator running another pitch retunes the filter
    // and that edit survives until the next mode change.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return {350, 850};
    if (u == QLatin1String("CWL"))  return {-850, -350};
    // Carrier-straddling modes: symmetric about the carrier, which the envelope
    // and synchronous detectors both need.
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

// Default TRANSMIT passband per mode, in Hz. POSITIVE for every mode, and that
// is not an oversight — TX and RX use opposite conventions and mixing them up
// transmits on the wrong sideband:
//
//   RX (RXANBPSetFreqs): the SIGN of the passband selects the sideband. The
//                        mode does not.
//   TX (SetTXABandpassFreqs): the MODE selects the sideband; the bandpass is an
//                        audio-domain magnitude. Handing it a negative pair
//                        flips LSB and DIGL onto the upper sideband.
//
// Measured, not assumed: hl2_txdsp_test drives a 1 kHz tone through the real
// modulator and reads the sideband off the emitted IQ. With a negative pair,
// LSB lands on the same wire bin as USB.
//
// Voice stays at the established 300..2700 rather than inheriting the wider RX
// window — that width is deliberate (see Hl2TxDsp::Config), and widening every
// SSB transmission is not part of making WSJT-X work. The digital modes get the
// full window because that is the one the decoder occupies.
std::pair<int, int> defaultTxPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("DIGU") || u == QLatin1String("DIGL")) return {150, 3000};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {300, 900};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")
        || u == QLatin1String("DSB")) return {100, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {100, 3000};
    return {300, 2700};   // USB/LSB and anything else: the voice default
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

    // Link lifecycle: first EP6 -> connected; stop -> disconnected.
    connect(m_metis, &MetisClient::linkUp, this, [this] {
        m_connected = true;
        // Started here rather than in connectRadio(): before the first EP6 there
        // is no link to describe, and ticking through the connect attempt would
        // publish a "reported" snapshot of zeros that reads as a dead link
        // rather than as one that has not come up yet.
        m_link = LinkStats{};
        m_linkRxPacketsAtLastTick = 0;
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
        // At connect there is one receiver, so this is always "not wide" — but
        // it is published rather than assumed, so the indicator starts from a
        // stated value instead of whatever the widget happened to hold.
        publishWideState();
    });
    connect(m_metis, &MetisClient::linkDown, this, [this] {
        if (m_connected) {
            m_connected = false;
            m_linkStatsTimer->stop();
            emit disconnected();
        }
    });
    // F4 (#4448): the radio never sent EP6 within the connect deadline — off,
    // unreachable, or already streaming to another client. Surface it as a
    // connection error and stop the Metis client so it does not sit half-open
    // paying out C&C at a radio that will never answer.
    connect(m_metis, &MetisClient::connectFailed, this, [this](const QString& reason) {
        // This handler runs on the MAIN thread (queued from the io thread), but
        // m_metis lives on the io thread — stop() touches its socket and timers,
        // so it must run THERE, not here. A direct call is the affinity bug the
        // destructor also guards against.
        QMetaObject::invokeMethod(m_metis, "stop", Qt::QueuedConnection);
        m_connected = false;
        m_linkStatsTimer->stop();
        emit connectionError(QStringLiteral("Hermes-Lite 2: %1").arg(reason));
    });

    // Per-receiver DSP outputs are wired in buildReceivers(), because the
    // receivers do not exist yet. Everything below is radio-wide.
    //
    // Modulated IQ -> the wire. Both live on the I/O thread, so this is a direct
    // call and the transmit path never touches the GUI thread.
    connect(m_txDsp, &Hl2TxDsp::iqReady, m_metis,
            [this](const std::vector<std::complex<float>>& iq) {
        m_metis->queueTxIq(iq);
    });
    connect(m_txDsp, &Hl2TxDsp::micPeak, this,
            [this](float dbfs) {
        emit meterUpdate(QStringLiteral("TX:MICPEAK"), dbfs);
        // Loudest thing the operator said this transmission. Evaluated at unkey
        // (setKeying) against the ALC's hold threshold — a PER-BLOCK test would
        // fire on every normal transmission, because the pauses between words
        // are exactly what the hold exists to sit through.
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
    // alcGain drives no meter — TX:ALC is fed from alcPeak above, for the reason
    // given there — but it is the number that answers "is the ALC holding?", so
    // it is mirrored for healthSnapshot() and the bridge. Before this it was
    // emitted into nothing, which is why a chain that was quietly refusing to
    // lift a quiet mic could only be diagnosed by reading the source.
    connect(m_txDsp, &Hl2TxDsp::alcGain, this,
            [this](float db) { m_alcGainDb = db; });
    // The modulator's own copy of the mic gain, for healthSnapshot(). Reported
    // ALONGSIDE m_micLevel rather than instead of it: the operator's request and
    // the modulator's state are different facts, and a diagnosis needs to see
    // them disagree. See Hl2TxDsp::micGainChanged.
    connect(m_txDsp, &Hl2TxDsp::micGainChanged, this,
            [this](double linear) { m_appliedMicGainLinear = linear; });

    connect(m_metis, &MetisClient::telemetryUpdated, this,
            [this](const Hl2Telemetry& t) { publishTelemetry(t); });
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
    });

    m_linkStatsTimer = new QTimer(this);
    m_linkStatsTimer->setInterval(kLinkStatsIntervalMs);
    connect(m_linkStatsTimer, &QTimer::timeout, this, &Hl2Backend::publishLinkStats);
}

void Hl2Backend::publishLinkStats()
{
    LinkStats s = m_link;
    // The link is REPORTED from the moment we are connected, even before the
    // first counter snapshot has crossed from the I/O thread. Otherwise the
    // consumer's first tick sees reported=false, keeps its Flex sources, and
    // renders the blank readout this whole path exists to fix.
    s.reported = true;
    // Fresh packets since the last tick — the transport-level proof of life the
    // heartbeat runs on. Computed here rather than in linkStats() because the
    // comparison CONSUMES the previous value, and linkStats() is a const getter
    // any caller may poll at any rate.
    s.alive = m_connected && m_link.rxPackets != m_linkRxPacketsAtLastTick;
    m_linkRxPacketsAtLastTick = m_link.rxPackets;
    emit linkStatsUpdated(s);
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
        r.haveSMeter = false;
        r.sMeterClock = QElapsedTimer{};

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
        // dBFS -> dBm through the one object that owns the reference. With
        // an uncalibrated fullScaleDbm this is a pure -lnaGain shift, which
        // is the part that is exactly right: it holds the trace still across
        // a gain change instead of letting the whole display jump.
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
            [this, ui](const std::vector<float>& pcm) {
        const auto* ids = m_ids.byUi(ui);
        if (!ids)
            return;
        // THIS SLICE's audio, before the mixer touches it. Per-slice consumers
        // (a TCI receiver channel, a decoder) need one slice's audio and cannot
        // un-mix the speaker sum. Pre-mute and pre-gain on purpose — see the
        // signal's comment: muting a slice must not stop WSJT-X decoding on it.
        //
        // Emitted even while keyed. The mixer drops keyed audio for the speaker
        // (we hear our own transmitter), but a per-slice consumer decides that
        // for itself, and the TX path already mutes the demodulator.
        emit sliceAudioFrameReady(ids->uiNumber, floatBytes(pcm));

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

        // Smooth EVERY sample, publish only on the tick. Both halves matter:
        // smoothing all of them is what makes the published value represent
        // the whole interval rather than one arbitrary instant inside it,
        // and the tick is what stops ~47 cross-thread emits a second
        // repainting a widget nobody can read that fast. Per receiver, so a
        // strong signal on one does not drive another's needle.
        if (!r->haveSMeter) {
            r->sMeterDbm = dbm;
            r->haveSMeter = true;
        } else {
            const double alpha = (dbm > r->sMeterDbm) ? kMeterAttackAlpha
                                                      : kMeterDecayAlpha;
            r->sMeterDbm = alpha * dbm + (1.0 - alpha) * r->sMeterDbm;
        }
        if (r->sMeterClock.isValid()
            && r->sMeterClock.elapsed() < kMeterPublishIntervalMs)
            return;
        r->sMeterClock.restart();
        emit meterUpdate(sliceMeterName(ui), r->sMeterDbm);
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
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 24000;
    dc.mode = modeFromString(r.mode);
    dc.filterLowHz = r.filterLowHz;
    dc.filterHighHz = r.filterHighHz;
    dc.agcMode = wdspAgcMode(r.agcMode);
    dc.maximumAgcGainDb = r.agcThresholdDb * kAgcCeilingDbPerUnit;
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
        Q_ARG(double, dspShiftHz(r.sliceFreqHz, r.ncoHz)));
    // Notches are radio-wide, so a receiver that appears after them has to be
    // brought up to date. Otherwise adding a second panadapter gives you one
    // receiver with the interferer notched out and one without.
    seedNotches(r);

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
        emit panRfGainChanged(ids->panId, m_lnaGainDb);
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

void Hl2Backend::mixReceiverAudio(int ddc, const std::vector<float>& pcm)
{
    // Belt and braces with the demodulator mute. This drops any block that was
    // already in flight when the key went down; Hl2RxDsp::setAudioMuted stops
    // the pipeline FILLING with our own transmission, which is what stopped the
    // tail draining out afterwards.
    //
    // THE MONITOR EXCEPTION HAS TO BE HERE, not only at the demodulator mute.
    // audioFrameReady() is emitted from this function and nowhere else, and it is
    // what feeds the engine's "output" capture — so an early return here silences
    // the capture no matter what the DSP is doing. Porting setTxAudioMonitor as a
    // demodulator-mute change alone would leave it a no-op on this backend, and a
    // diagnostic that reads silence draws a confident wrong conclusion from it
    // (#4487 review, finding 1). Off by default; only a measurement turns it on.
    if (m_keyed && !m_txMonitor)
        return;
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
        // the reason the fast path exists — no copy, no clamp.
        if (q.size() == pcm.size()) {
            emit audioFrameReady(floatBytes(pcm));
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
        emit audioFrameReady(floatBytes(m_mixAccum));
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

    emit audioFrameReady(floatBytes(m_mixAccum));
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
        // while waiting. HERMES.md §22.4. It is also what keeps the QPointer in
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
}

RadioCapabilities Hl2Backend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("hl2");
    c.manufacturer = QStringLiteral("Hermes-Lite");
    c.model = QStringLiteral("Hermes-Lite 2");
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
    c.maxSlices = ceiling;
    c.maxPanadapters = ceiling;
    for (const int rate : kIqSampleRatesHz)
        c.sampleRatesHz.append(rate);
    // The AD9866 samples at 76.8 MHz, so the first Nyquist zone — everything
    // this receiver can hear without relying on an alias — is DC to 38.4 MHz
    // (oracle §7, which is also why the wideband bandscope spans exactly that).
    // The low end is 100 kHz rather than 0: below that the input transformer
    // rolls off and there is nothing to hear.
    c.tuningMinHz = 100'000.0;
    c.tuningMaxHz = 38'400'000.0;
    // Reported from the gate, not hardcoded: the engine's TX guard keys off this,
    // so a build with transmit disabled must look RX-only from above the seam.
    c.canTransmit = m_txAllowed;
    c.hostModulates = true;
    // Same tap, same seam — see RadioCapabilities::takesTxAudioOverSeam.
    c.takesTxAudioOverSeam = true;             // PC runs the modulator; no on-radio mic jacks
    c.txPowerMaxWatts = 0.0;            // uncalibrated; see the oracle on power counts
    c.hasTuner = false;
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
    // Both moot while hasRadioSideDsp is false — the host runs every filter
    // this radio has — but stated rather than defaulted, per the struct's
    // "a backend that omits one silently declares it absent" rule.
    c.hasLmsNoiseFilters = false;
    c.hasManualNotch = false;
    // The 76.8 MHz NCO scale is a localparam in the bitstream and nothing in the
    // HPSDR map can be told the crystal's real error — so the correction is ours
    // or it does not happen. See Hl2FreqCal for the derivation.
    c.hostFrequencyCalibration = true;
    // Declared because invokeExtension() now implements it (freqcal.get / .set /
    // .set_live). This field is the handshake a client pre-checks before issuing
    // an extension call, so leaving it empty while the verbs work would report
    // the opposite of the truth.
    c.extensionNamespaces << QStringLiteral("hl2");
    // No on-radio configuration store. The HL2 holds no state across a
    // connection beyond its registers — everything the operator can change
    // lives in this application, so there is nothing for a profile to name.
    c.hasProfiles = false;
    c.hasSelectableMicInputs = false;

    // EMPTY: the HL2's receive filters are the host DSP's, and continuous.
    c.rxFilterWidthsHz = {};
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
    // The HL2 declares PATEMP but no "+13.8A": PA temperature is a real reading
    // from this radio, the supply rail is not reported at all. Only the volts
    // readout goes away — the temperature above it keeps working.
    c.hasSupplyVoltageTelemetry = false;
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
                            // Memories: the client owns them (persistsMemories
                            // is false above). NOTE the bank itself engages on
                            // persistsMemories and keeps its own SHARED
                            // document — this declaration is descriptive
                            // completeness, and Memories stays out of
                            // RadioStateMemory's ext gate (one domain, one
                            // document — RFC #4603 PR 6).
                            | RadioCapabilities::ClientSettingsDomain::Memories;
    // No extension namespaces (no invokeExtension verbs yet), matching FlexBackend.
    return c;
}

void Hl2Backend::connectRadio(const RadioConnectRequest& request)
{
    const QHostAddress host(request.host);
    if (host.isNull()) {
        emit connectionError(QStringLiteral("HL2: invalid host '%1'").arg(request.host));
        return;
    }

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
    if (m_haveRestoredState
        && !request.params.contains(QStringLiteral("lnaGainDb"))) {
        m_lnaGainDb = qBound(kLnaGainMinDb,
                             m_lnaDbByBand.value(m_currentBandKey, m_lnaDefaultDb),
                             kLnaGainMaxDb);
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
    // COMMANDED, not true-RF: this seeds MetisClient's initial RX and TX
    // command banks, which are register contents. Everything else in this
    // function keeps startFreqHz in the true-RF domain.
    mp.rxFrequencyHz = ncoCommandHz(startFreqHz);
    mp.lnaGainDb = m_lnaGainDb;
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
    m_dbRef.setLnaGainDb(m_lnaGainDb);

    // ---- build the receivers, BEFORE start() ----
    //
    // ORDER IS LOAD-BEARING, and the reason is the same one that governs every
    // other ordering decision in this backend: EP2 must not stop.
    //
    // Opening a WDSP channel is slow -- ~19 s on the FIRST open this machine
    // ever does, generating FFTW wisdom, and 40-175 ms for every open after
    // that, at any rate (HERMES.md §10 and §22.3) -- and it runs ON THE I/O
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
    // Remember the threshold the modulator is being handed, so the health
    // snapshot and the unkey diagnostic both report what it is RUNNING rather
    // than what a default-constructed Config would have said. Assigned from
    // `tc` and not from the default so it stays correct the day this Config
    // sets the field.
    m_alcHoldBelowDbfs = tc.alcHoldBelowDbfs;

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
    // The RECEIVERS, and only them. The transmit chain is not a step:
    // Hl2TxDsp::configure() designs two FIR kernels and returns — it opens no
    // WDSP channel and measures no FFTW plan (the class includes WdspChannel.h
    // for the Mode enum alone). Counting it inflated the denominator with a step
    // that completes in microseconds and put a "Preparing the transmit chain…"
    // label on screen for work that was already over.
    const int total = actualNumRx;

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
        dc.filterLowHz = r.filterLowHz;
        dc.filterHighHz = r.filterHighHz;
        chains.push_back(r.dsp);
        configs.push_back(dc);
    }

    emit dspSetupProgress(tr("Preparing the receive chain…"), 0, total);

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
    // the note in ~Hl2Backend() and HERMES.md §22.4.
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
        // No progress emit for the transmit chain: it designs two FIR kernels
        // and returns. Announcing a step that is over before the label repaints
        // is worse than saying nothing.
        if (txDsp)
            result.txOk = txDsp->configure(tc, &result.txErr);

        // ---- back to the GUI thread ----
        QMetaObject::invokeMethod(self, [self, result] {
            if (self)
                self->finishDspSetup(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Hl2Backend::finishDspSetup(const DspSetupResult& result)
{
    if (!m_pendingConnect)
        return;
    // A disconnect, or a second connect, arrived while the chains were opening.
    // The wire was never started, so there is nothing to stop — but the chains
    // ARE open, and leaving them open would leak WDSP channels out of the
    // 32-slot pool on every abandoned connect.
    if (m_pendingConnect->generation != m_connectGeneration) {
        qCInfo(lcHl2) << "HL2: connect superseded while the DSP was opening —"
                      << "releasing the chains it built";
        m_pendingConnect.reset();
        // Safe to block inside here: the build has finished, so the I/O thread
        // is back at its event loop and publishIoDsps() returns promptly.
        tearDownReceivers();
        emit dspSetupFinished();
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

void Hl2Backend::disconnectRadio()
{
    // Invalidate any DSP build still in flight. Without this, a disconnect
    // during the opens would be followed by finishDspSetup() starting a wire
    // for a session the operator has already left. The build itself cannot be
    // cancelled — WDSP's OpenChannel does not return early — so it runs to
    // completion on the I/O thread and finishDspSetup() releases what it built.
    ++m_connectGeneration;
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
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, dspShiftHz(r->sliceFreqHz, r->ncoHz)));

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

void Hl2Backend::setSliceMode(int sliceId, const QString& mode)
{
    const int ddc = ddcForSlice(sliceId);
    Receiver* r = rx(ddc);
    if (!r)
        return;
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
        QMetaObject::invokeMethod(r->dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        QMetaObject::invokeMethod(r->dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(r->filterLowHz)),
            Q_ARG(double, static_cast<double>(r->filterHighHz)));
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
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, lowHz), Q_ARG(double, highHz));
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
    const int wdspAgc = wdspAgcMode(m);

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
    r->agcMode = m.isEmpty() ? r->agcMode : m;
    r->agcThresholdDb = qBound(0, thresholdDb, 100);
    const double ceilingDb = r->agcThresholdDb * kAgcCeilingDbPerUnit;
    if (r->dsp)
        QMetaObject::invokeMethod(r->dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgc), Q_ARG(double, ceilingDb));
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
}

void Hl2Backend::setSliceAudioGain(int sliceId, int gainPercent)
{
    Receiver* r = rx(ddcForSlice(sliceId));
    if (!r)
        return;
    // 0..100 -> 0.0..1.0 LINEAR, matching what a Flex does with audio_level
    // rather than inventing a dB curve here. Unity at 100 keeps a single
    // unmuted slice at exactly the level it has today.
    r->audioGain = std::clamp(gainPercent, 0, 100) / 100.0f;
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
            Q_ARG(double, dspShiftHz(r->sliceFreqHz, r->ncoHz)));
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
    if (clamped == m_lnaGainDb)
        return;
    applyLnaGainDb(clamped);
    qCInfo(lcHl2) << "HL2 LNA gain:" << m_lnaGainDb << "dB (requested" << gainDb << ")";

    // The operator's gain belongs to the band they set it on (RFC #4603 PR 3).
    if (!m_currentBandKey.isEmpty())
        m_lnaDbByBand.insert(m_currentBandKey, m_lnaGainDb);
    notifyOperatingStateChanged();
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

    const int previousRate = m_sampleRateHz;

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

    // The DDC rate lives in the config register (C0=0x00), latched into the next
    // C&C round. Deliberately NOT followed by a filter-pipeline reset: sending
    // 0x39 on every geometry change is what wedged a board hard enough to need a
    // power cycle (see MetisClient::requestPipelineReset). The decimation filters
    // settle on their own within a few blocks.
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setSampleRate", Qt::QueuedConnection,
            Q_ARG(AetherSDR::hl2::SampleRate, sampleRateEnum(rate)));

    // FOLLOW-UP: this loop still BLOCKS THE GUI THREAD, which is the same defect
    // connectRadio() had before it was split into beginDspSetup()/
    // finishDspSetup(). Each receiver costs an open (40-175 ms) plus a close
    // that flushes under WDSP's 100 ms timeout, and it runs for every receiver
    // because the rate register is radio-wide — roughly 0.6-1.1 s of frozen UI
    // per rate-boundary crossing with four panadapters open. That is the
    // "chunky zoom" operators report. HERMES.md §22.4 has the measurements.
    //
    // Not fixed here on purpose: unlike the connect, this has ordering
    // constraints that survive a partial failure — the DSP must expect the new
    // rate BEFORE EP6 delivers at it, and a receiver that fails to reconfigure
    // has to put the ones already rebuilt back, or the set is split across two
    // rates with nothing reporting it. The phase split is reusable; the
    // roll-back is what needs designing.
    //
    // Rebuild the receive chain at the new input rate. WDSP's channel is opened
    // with a fixed input rate, so a rate change is a reconfigure, not a setter —
    // Hl2RxDsp::Config carries the operator's live mode/filter/AGC/shift so the
    // rebuild comes back up where they left it rather than on construction
    // defaults.
    //
    // Blocking, and in this order: the DSP must already expect the new rate
    // before EP6 starts delivering at it. Reconfiguring afterwards would feed
    // 384 kHz IQ into a chain still decimating for 48 kHz, which is not an error
    // anything reports — it is simply the wrong audio and a mis-scaled spectrum.
    // EVERY receiver, because the rate register is radio-wide: one receiver left
    // decimating for the old rate would produce wrong audio and a mis-scaled
    // spectrum, with nothing reporting an error.
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        Receiver& r = m_rx[i];
        if (!r.dsp)
            continue;
        Hl2RxDsp::Config dc;
        dc.inputSampleRateHz = m_sampleRateHz;
        dc.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
        dc.mode = modeFromString(r.mode);
        dc.filterLowHz = r.filterLowHz;
        dc.filterHighHz = r.filterHighHz;
        // Carried through the rebuild rather than reapplied afterwards. A
        // reconfigured channel opens on Config's defaults, so an operator who had
        // moved their AGC would have had it silently snap back to medium/39 dB
        // every time they zoomed.
        dc.agcMode = wdspAgcMode(r.agcMode);
        dc.maximumAgcGainDb = r.agcThresholdDb * kAgcCeilingDbPerUnit;
        std::string err;
        bool ok = false;
        Hl2RxDsp* dsp = r.dsp;
        QMetaObject::invokeMethod(dsp, [dsp, &dc, &err, &ok] {
            ok = dsp->configure(dc, &err);
        }, Qt::BlockingQueuedConnection);
        if (!ok) {
            // Failing back to the old rate keeps the wire and the DSP agreeing.
            // The alternative — leaving the register commanded to a rate the DSP
            // cannot process — is silent: audio would be wrong with nothing in
            // the UI to say why.
            //
            // The receivers already rebuilt are put BACK, so a partial failure
            // does not leave the set split across two rates. That is the failure
            // mode multi-receiver adds: with one receiver there was nothing to
            // be inconsistent with.
            qWarning() << "Hl2Backend: could not reconfigure RX DSP" << i << "for"
                       << m_sampleRateHz << "Hz —"
                       << QString::fromStdString(err)
                       << "— staying at" << previousRate << "Hz";
            m_sampleRateHz = previousRate;
            for (std::size_t k = 0; k < i; ++k) {
                Hl2RxDsp* back = m_rx[k].dsp;
                if (!back)
                    continue;
                Hl2RxDsp::Config rc = dc;
                rc.inputSampleRateHz = previousRate;
                rc.mode = modeFromString(m_rx[k].mode);
                rc.filterLowHz = m_rx[k].filterLowHz;
                rc.filterHighHz = m_rx[k].filterHighHz;
                rc.agcMode = wdspAgcMode(m_rx[k].agcMode);
                rc.maximumAgcGainDb = m_rx[k].agcThresholdDb * kAgcCeilingDbPerUnit;
                std::string backErr;
                bool backOk = false;
                QMetaObject::invokeMethod(back, [back, &rc, &backErr, &backOk] {
                    backOk = back->configure(rc, &backErr);
                }, Qt::BlockingQueuedConnection);
                if (!backOk) {
                    qCCritical(lcHl2) << "HL2: receiver" << k
                                      << "could not be restored to" << previousRate
                                      << "Hz —" << QString::fromStdString(backErr);
                }
            }
            if (m_metis)
                QMetaObject::invokeMethod(m_metis, "setSampleRate",
                    Qt::QueuedConnection,
                    Q_ARG(AetherSDR::hl2::SampleRate,
                          sampleRateEnum(previousRate)));
            emitAllPanState();
            return;
        }
    }

    // Remember it. The span is the operator's deliberate choice about how much
    // network and CPU this radio may consume, so it survives the session rather
    // than snapping back to the conservative default on the next launch.
    // Written only after the reconfigure SUCCEEDED — persisting a rate the DSP
    // just refused would make the failure permanent across restarts.
    Hl2Settings::setSpanMhz(static_cast<double>(m_sampleRateHz) / 1.0e6);

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

void Hl2Backend::setKeying(bool key)
{
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
    // THE ALC'S HOLD THRESHOLD IS A SILENT CLIFF, so say when an operator has
    // fallen off it.
    //
    // Below alcHoldBelowDbfs the ALC deliberately stops raising gain — that is
    // what keeps it from winding up 40 dB on room noise between words and
    // fighting the speech processor. But a microphone whose PEAKS never reach
    // the threshold now gets no makeup gain at all where it previously got up to
    // 40 dB, and "I was quiet on the air" points at nothing. The answer is mic
    // gain, and the meter that shows it is the one this very peak feeds.
    //
    // At unkey, once per transmission, on the main thread: the DSP worker must
    // not log per block, and a per-block test would fire on every normal
    // transmission because the pauses between words are what the hold is for.
    if (m_keyed && !key) {
        const double kHoldDbfs = m_alcHoldBelowDbfs;
        if (m_txMicPeakMaxDbfs > -139.0f
            && m_txMicPeakMaxDbfs < static_cast<float>(kHoldDbfs)) {
            qCInfo(lcHl2) << "HL2 TX: microphone peaked at" << m_txMicPeakMaxDbfs
                          << "dBFS for the whole transmission, below the ALC hold"
                             " threshold of" << kHoldDbfs
                          << "dBFS — the ALC held rather than lifting it, so that"
                             " audio went out quiet. Raise mic gain.";
        }
    }
    if (key) {
        m_txMicPeakMaxDbfs = -140.0f;
        // Start each transmission's peak hold from nothing, rather than trusting
        // the unkeyed branch in publishTelemetry() to have already walked it
        // down. Telemetry is 10 Hz, so a key inside 100 ms of the previous unkey
        // can arrive before any no-carrier sample does, and the new over would
        // open displaying the old one's peak.
        m_fwdPeakWatts = 0.0;
    }

    m_keyed = key;
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
    for (Receiver& r : m_rx) {
        if (r.dsp)
            QMetaObject::invokeMethod(r.dsp, "setAudioMuted", Qt::QueuedConnection,
                Q_ARG(bool, muteWhileKeyed));
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
        setTxTestTone(0.0, 0.0);
    if (!key) {
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
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setMox", Qt::QueuedConnection,
            Q_ARG(bool, key));
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
        if (m_txDsp)
            QMetaObject::invokeMethod(m_txDsp, "reset", Qt::QueuedConnection);
        if (m_metis)
            QMetaObject::invokeMethod(m_metis, "flushTxIq", Qt::QueuedConnection);
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
                Q_ARG(double, dspShiftHz(r->sliceFreqHz, r->ncoHz)));
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

void Hl2Backend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz)
{
    // Only modulate while actually keyed. Feeding the modulator unkeyed would
    // fill the transmit queue with audio that goes out the instant MOX asserts —
    // the operator would hear the last second of the shack on their first
    // syllable.
    if (!m_txDsp || !m_keyed || int16Stereo.isEmpty())
        return;
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
    QMetaObject::invokeMethod(m_txDsp, [this, mono = std::move(mono)] {
        m_txDsp->processAudioBlock(mono);
    }, Qt::QueuedConnection);
}

void Hl2Backend::setTxTestTone(double offsetHz, double amplitude)
{
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
    QMetaObject::invokeMethod(m_metis, "setTxTestTone", Qt::QueuedConnection,
        Q_ARG(double, offsetHz), Q_ARG(double, amplitude));
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
    for (Receiver& r : m_rx) {
        if (r.dsp)
            QMetaObject::invokeMethod(r.dsp, "setAudioMuted", Qt::QueuedConnection,
                Q_ARG(bool, m_keyed && !on));
    }
}

void Hl2Backend::setTune(bool on, int tunePowerPercent)
{
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
        setTxTestTone(0.0, kTuneCarrierAmplitude);
        m_toneFromTune = true;   // set AFTER: setTxTestTone clears the flag
        setKeying(true);
    } else {
        setKeying(false);   // clears m_tuning and restores the operator's RF power
        setTxTestTone(0.0, 0.0);
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
// 50 IS UNITY, and that is load-bearing rather than cosmetic. TransmitModel
// constructs m_micLevel at 50 and nothing restores it at startup, so a session
// where the operator never touches the slider must leave the modulator exactly
// where its own default (m_micGain = 1.0) puts it. A mapping with unity anywhere
// else would silently change the transmit level of every existing HL2 install
// the first time this code shipped.
//
// Above and below that, +/-20 dB linear in dB — 0.4 dB per slider step, which is
// fine enough to set by ear and wide enough to cover the range between a headset
// boom mic and a built-in laptop microphone.
//
// WHAT THIS DOES AND DOES NOT BUY, because the ALC sits right behind it:
// Hl2TxDsp's ALC normalizes each block's peak to alcTargetPeak, so raising mic
// gain on already-loud speech is largely given back and PEP barely moves. Where
// it MATTERS is the ALC's hold threshold (alcHoldBelowDbfs, -45 dBFS): below
// that the ALC deliberately stops lifting, so a microphone quiet enough to sit
// under it gets no makeup at all and goes out weak. This slider is what carries
// such a mic over the threshold — which is exactly what setKeying()'s "raise mic
// gain" diagnostic tells the operator to do, and until now that advice pointed
// at a control that did nothing on this backend.
//
// Level 0 mutes outright rather than resolving to -20 dB. A slider at the bottom
// of its travel means off, and a mic that is merely 20 dB down would still be
// hauled back up by the ALC's 40 dB of makeup — so without the special case,
// "0" would sound barely different from "50".
void Hl2Backend::setMicGain(int level)
{
    level = std::clamp(level, 0, 100);
    m_micLevel = level;

    const double linear = micSliderToLinear(level);

    if (m_txDsp)
        QMetaObject::invokeMethod(m_txDsp, "setMicGain", Qt::QueuedConnection,
            Q_ARG(double, linear));
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
    }
    // No other HL2 extension verbs; honor the async contract without hanging.
    if (requestId != 0)
        emit extensionError(requestId, QStringLiteral("hl2: no extension verbs implemented"));
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

    section("connected", QStringLiteral("Radio"));
    put("connected", QStringLiteral("Connected"), m_connected);
    put("model", QStringLiteral("Model"), QStringLiteral("Hermes-Lite 2"));
    // From EP6 RADDR 0, C4[7:0]. This is the GATEWARE's firmware revision as
    // the running radio reports it — not the version string the discovery reply
    // carried, which is only ever seen by the radio picker before connecting.
    put("firmwareVersion", QStringLiteral("Firmware version"),
        opt(m_telemetry.firmwareVersion));

    section("adcOverload", QStringLiteral("Converter"));
    put("adcOverload", QStringLiteral("ADC overload"), opt(m_telemetry.adcOverload));
    put("lnaGainDb", QStringLiteral("LNA gain (dB)"), m_lnaGainDb);

    section("txInhibited", QStringLiteral("Transmit"));
    // The register bit is ACTIVE LOW and MetisProtocol already decodes it, so
    // what is shown here is the plain-language sense: true means transmit is
    // being held off. Displaying the raw bit would read exactly backwards.
    put("txInhibited", QStringLiteral("TX inhibited"), opt(m_telemetry.txInhibited));
    put("ptt", QStringLiteral("PTT (radio)"), m_telemetry.ptt);
    put("keyed", QStringLiteral("Keyed (app)"), m_keyed);
    put("tuning", QStringLiteral("Tune carrier"), m_tuning);
    // "The most important number in the protocol" (oracle §6): the depth of the
    // FPGA's transmit sample buffer. Drifting up means we are sending faster
    // than the radio consumes; drifting down is an impending underrun.
    put("txFifoCount", QStringLiteral("TX FIFO depth"), opt(m_telemetry.txFifoCount));
    put("txFifoUnderflow", QStringLiteral("TX FIFO underflow"),
        opt(m_telemetry.txFifoUnderflow));
    put("txFifoOverflow", QStringLiteral("TX FIFO overflow"),
        opt(m_telemetry.txFifoOverflow));

    // DRIVE: WHAT WAS ASKED FOR, AND WHAT WAS WRITTEN (#4912).
    //
    // Nothing anywhere reported the APPLIED drive. `get transmit` has rfPower
    // and `get radio` has txPower, but both read TransmitModel — the operator's
    // request — which is exactly the readback-shares-the-failure problem this
    // section exists to solve. Worse, applyDrive()'s transmit gate forces the
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
    put("micGainAppliedLinear", QStringLiteral("Mic gain at the modulator (linear)"),
        std::isnan(m_appliedMicGainLinear) ? QVariant()
                                           : QVariant(m_appliedMicGainLinear));
    // Peak mic level for the CURRENT transmission, reset at each key. Compared
    // against the hold threshold below, these two rows are the whole "why did I
    // go out quiet" diagnosis: a peak under the threshold means the ALC declined
    // to lift it and the answer is mic gain.
    put("txMicPeakDbfs", QStringLiteral("Mic peak this over (dBFS)"),
        m_txMicPeakMaxDbfs > -139.0f ? QVariant(m_txMicPeakMaxDbfs) : QVariant());
    // The threshold the modulator was actually CONFIGURED with, not the one a
    // default-constructed Config would have. The two agree today because the
    // Config built in connectRadio() never touches this field — but this row sits
    // in a section whose thesis is "report what the modulator is running",
    // and a constant that silently stops matching the modulator is the row
    // nobody would think to suspect.
    put("alcHoldBelowDbfs", QStringLiteral("ALC hold threshold (dBFS)"),
        m_alcHoldBelowDbfs);
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
    put("temperatureC", QStringLiteral("PA temperature (°C)"),
        m_havePaTemp ? QVariant(m_paTempC) : QVariant());
    put("temperatureRaw", QStringLiteral("Temperature (raw counts)"),
        opt(m_telemetry.temperatureRaw));
    put("biasCurrentRaw", QStringLiteral("PA bias current (raw counts)"),
        opt(m_telemetry.biasCurrentRaw));

    section("forwardPowerRaw", QStringLiteral("Directional coupler (uncalibrated)"));
    put("forwardPowerRaw", QStringLiteral("Forward (raw counts)"),
        opt(m_telemetry.forwardPowerRaw));
    put("forwardPowerW", QStringLiteral("Forward (W, approx — instantaneous)"),
        m_telemetry.forwardPowerRaw
            ? QVariant(directionalWatts(*m_telemetry.forwardPowerRaw)) : QVariant());
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
        m_telemetry.forwardPowerRaw ? QVariant(m_fwdPeakWatts) : QVariant());
    put("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),
        opt(m_telemetry.reversePowerRaw));
    put("reversePowerW", QStringLiteral("Reverse (W, approx)"),
        m_telemetry.reversePowerRaw
            ? QVariant(directionalWatts(*m_telemetry.reversePowerRaw)) : QVariant());
    // Meaningful without calibration — it is a ratio of two readings from the
    // same converter, so the unknown scale cancels. Absent below the noise
    // floor, where a ratio of two noise samples is not a mismatch reading.
    //
    // That last sentence described the intent but not the code: this site had no
    // floor, so with no carrier it recomputed a noise ratio at the dialog's
    // 500 ms refresh and the row visibly bounced — while the TX:SWR meter, which
    // did apply the floor, correctly showed nothing. Same guard here now, from
    // the shared constant, so the two surfaces cannot disagree.
    {
        QVariant swr;
        if (m_telemetry.forwardPowerRaw && m_telemetry.reversePowerRaw
            && *m_telemetry.forwardPowerRaw >= kMinForwardCountsForSwr) {
            if (const auto v = swrFromRaw(*m_telemetry.forwardPowerRaw,
                                          *m_telemetry.reversePowerRaw))
                swr = *v;
        }
        put("swr", QStringLiteral("SWR"), swr);
    }

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
    m_lnaDefaultDb = 20;          // Hl2Backend.h: m_lnaGainDb's constructed default
    m_lnaGainDb = 20;
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
    if (isKnownModeString(state.mode))
        valid.mode = state.mode.toUpper();   // canonical casing — a "cw" from a
                                             // hand-edited document must not
                                             // round-trip into the UI
    // A passband is kept only as a sane pair; mode+passband are applied
    // together in pushInitialState() (the #4484 reconciliation).
    if (state.filterLowHz < state.filterHighHz
        && state.filterLowHz >= -12'000.0 && state.filterHighHz <= 12'000.0)
    {
        valid.filterLowHz = state.filterLowHz;
        valid.filterHighHz = state.filterHighHz;
    }
    if (state.sampleRateHz > 0)
        valid.sampleRateHz = nearestIqSampleRateHz(state.sampleRateHz);

    // Per-band maps ride the typed extension's domain sub-objects
    // (RestoredRadioState.h). Values clamp to the hardware's own ranges.
    const QJsonObject rfGain =
        state.extension.value(QStringLiteral("rfGain")).toObject();
    if (rfGain.contains(QStringLiteral("defaultDb")))
        m_lnaDefaultDb = qBound(kLnaGainMinDb,
                                rfGain.value(QStringLiteral("defaultDb")).toInt(),
                                kLnaGainMaxDb);
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
    qCInfo(lcHl2) << "HL2 restore: freq" << valid.rfFrequencyHz << "mode"
                  << valid.mode << "filter" << valid.filterLowHz << ".."
                  << valid.filterHighHz << "rate" << valid.sampleRateHz
                  << "lna bands" << m_lnaDbByBand.size() << "drive bands"
                  << m_driveByBand.size();
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
        lnaByBand.insert(m_currentBandKey, m_lnaGainDb);
        driveByBand.insert(m_currentBandKey, m_rfPowerPercent);
    }

    QJsonObject rfGain{{QStringLiteral("defaultDb"), m_lnaDefaultDb},
                       {QStringLiteral("lnaDbByBand"), lnaByBand}};
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
    state.extension = QJsonObject{{QStringLiteral("rfGain"), rfGain},
                                  {QStringLiteral("txSetpoints"), txSetpoints}};
    state.extensionSchemaVersion = 1;
    return state;
}

// The one true LNA application: register write, dB-reference lockstep, and
// the every-pan echo. Shared by the operator path (setPanRfGain) and the
// band-memory path so the two can't drift (PR #4619 review).
//
// The dB reference moves IN LOCKSTEP with the gain: spectrum and S-meter are
// both rendered through m_dbRef, so without this every gain change would
// slide the whole trace — an operator backing off 10 dB would watch the
// noise floor drop and read it as the band going quiet.
void Hl2Backend::applyLnaGainDb(int gainDb)
{
    m_lnaGainDb = gainDb;
    m_dbRef.setLnaGainDb(m_lnaGainDb);
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setLnaGainDb", Qt::QueuedConnection,
            Q_ARG(int, m_lnaGainDb));
    // Echo what the hardware actually took, to every pan — a slider that
    // asked for something outside the register's range finds out here.
    for (const auto& ids : m_ids.all())
        emit panRfGainChanged(ids.panId, m_lnaGainDb);
}

void Hl2Backend::rememberCurrentBandState()
{
    if (m_currentBandKey.isEmpty())
        return;
    m_lnaDbByBand.insert(m_currentBandKey, m_lnaGainDb);
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
    const QString oldBand = m_currentBandKey;
    m_currentBandKey = newBand;

    const int lna = qBound(kLnaGainMinDb,
                           m_lnaDbByBand.value(newBand, m_lnaDefaultDb),
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
    // category as the mode-change stickiness in HERMES.md 15.7: mode and passband
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
        QMetaObject::invokeMethod(r.dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(r.mode)));
        QMetaObject::invokeMethod(r.dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(r.filterLowHz)),
            Q_ARG(double, static_cast<double>(r.filterHighHz)));
        QMetaObject::invokeMethod(r.dsp, "setAudioMuted", Qt::QueuedConnection,
            Q_ARG(bool, false));
        // The notch axis, which is measured from the NCO and defaults to ZERO.
        // Miss this and a notch is placed ~10 MHz outside the passband, where
        // WDSP finds no notch to apply and simply builds an unnotched filter —
        // no error, no notch, and a `notch list` that reports it as present.
        // createPanadapter() seeded the receivers it creates; the ones built at
        // connect need it too, which is the whole set on a normal session.
        seedNotches(r);
    }
    if (m_txDsp) {
        const Receiver* txRx = rx(m_txDdc);
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode,
                  modeFromString(txRx ? txRx->mode : QStringLiteral("USB"))));
        QMetaObject::invokeMethod(m_txDsp, "reset", Qt::QueuedConnection);
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
        emit panRfGainChanged(ids.panId, m_lnaGainDb);
    }

    // Keying state is ours, not the radio's: a reconnect must never come up
    // keyed because the previous session ended mid-transmission.
    m_keyed = false;
    m_tuning = false;
    setKeying(false);
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
    // SWR is meaningful WITHOUT calibration because it is a ratio of two
    // readings from the same converter, so the unknown scale cancels.
    // SWR only means something with real forward power behind it.
    //
    // Measured on the live radio: with no carrier the forward and reverse counts
    // are both near zero and dominated by noise, reverse frequently exceeds
    // forward, and the computed ratio saturated the meter at 255.99:1 — a
    // dramatic reading of nothing at all. An operator glancing at that sees a
    // catastrophic mismatch on an antenna that is fine.
    //
    // The threshold is in raw counts because that is what we have; it is a
    // noise floor, not a calibrated power level. It lives at file scope so the
    // Radio Health snapshot applies the SAME floor — see kMinForwardCountsForSwr.
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
    // TX IQ FIFO depth — the oracle calls this the most important number in the
    // protocol. A queue-fed transmission can starve the radio's buffer in a way
    // a per-packet generated tone never can, so this is what distinguishes
    // "the audio is wrong" from "the audio never arrived".
    if (m_keyed && t.txFifoCount)
        qCDebug(lcHl2Tx) << "HL2 fifo:" << *t.txFifoCount
                         << "under" << t.txFifoUnderflow.value_or(false)
                         << "over" << t.txFifoOverflow.value_or(false);
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
    if (t.adcOverload && *t.adcOverload != m_adcOverload) {
        m_adcOverload = *t.adcOverload;
        if (m_adcOverload)
            qWarning() << "Hl2Backend: ADC OVERLOAD — reduce LNA gain or attenuate";
    }
}

double Hl2Backend::directionalWatts(int raw)
{
    // Quisk's `power_meter_std_calibrations['HL2FilterE3']` verbatim
    // (quisk_conf_defaults.py): measured [ADC count, watts] pairs for a
    // Hermes-Lite 2 with an N2ADR companion filter board, rev E3. Quisk is the
    // reference client and tier 3 on the source-precedence ladder, and this is
    // the only published curve for this coupler.
    //
    // WHAT THIS IS NOT: a calibration of THIS radio. The oracle (§6) is explicit
    // that these counts need a per-unit calibration against a dummy load to mean
    // watts, because the coupler, the toroid winding and the detector diode all
    // vary between boards. A reading from this curve is the right ORDER OF
    // MAGNITUDE and roughly the right shape; it is not a measurement.
    //
    // It is still much better than the alternative, which was publishing
    // nothing: an operator had no way to tell 100 mW from 5 W, and on a radio
    // where a mis-set drive level is silent that is the difference between
    // "working" and "not transmitting". The meters are labelled uncalibrated
    // (defineMeters) so nobody reads them as a power measurement.
    //
    // A future per-unit calibration replaces this table and nothing else.
    struct Point { double counts; double watts; };
    static constexpr Point kCurve[] = {
        {    0.000000, 0.000000 }, {   25.865385, 0.002550 },
        {  101.024540, 0.012752 }, {  265.290123, 0.050601 },
        {  647.915584, 0.216458 }, { 1196.593548, 0.665480 },
        { 1603.703226, 1.155723 }, { 2012.327160, 1.811892 },
        { 2616.772727, 3.008585 }, { 3173.818182, 4.392743 },
        { 3382.792208, 4.979133 }, { 3721.071429, 6.024751 },
        { 4093.178571, 7.289948 }, { 4502.496429, 8.820838 },
        { 4952.746071, 10.673214 },
    };
    constexpr std::size_t kN = std::size(kCurve);

    const double counts = static_cast<double>(raw);
    if (counts <= kCurve[0].counts)
        return 0.0;
    // Above the top of the table, extrapolate along the last segment rather
    // than clamping. Clamping would pin the meter at 10.7 W and hide the one
    // reading an operator most needs to see — that they are past where the
    // curve was ever measured.
    if (counts >= kCurve[kN - 1].counts) {
        const Point& a = kCurve[kN - 2];
        const Point& b = kCurve[kN - 1];
        const double slope = (b.watts - a.watts) / (b.counts - a.counts);
        return b.watts + (counts - b.counts) * slope;
    }
    for (std::size_t i = 1; i < kN; ++i) {
        if (counts <= kCurve[i].counts) {
            const Point& a = kCurve[i - 1];
            const Point& b = kCurve[i];
            const double span = b.counts - a.counts;
            if (span <= 0.0)
                return b.watts;
            return a.watts + (counts - a.counts) * (b.watts - a.watts) / span;
        }
    }
    return kCurve[kN - 1].watts;
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
    // AD9866 on-die temperature via the HL2's instrumentation ADC. The scaling
    // below is the Hermes-Lite 2 wiki's published formula. It is NOT verified
    // against a reference thermometer here, so treat it as indicative.
    return (3.26 * (static_cast<double>(raw) / 4096.0) - 0.5) / 0.01;
}

void Hl2Backend::applyBandFilter(const char* reason)
{
    if (!m_metis || m_rx.empty())
        return;

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
    d.frequency = r->sliceFreqHz / 1.0e6;   // MHz
    d.mode = r->mode;
    d.filterLow = r->filterLowHz;
    d.filterHigh = r->filterHighHz;
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
