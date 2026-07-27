#include "core/backends/hl2/Hl2Backend.h"

#include <cmath>
#include <limits>

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include "core/AutomationBridgeSettings.h"
#include "core/LogManager.h"
#include "core/backends/hl2/Hl2Settings.h"

#include <QByteArray>
#include <QHostAddress>
#include <QLoggingCategory>

#include <cstdint>
#include <utility>

Q_LOGGING_CATEGORY(lcHl2Tx, "aether.hl2.tx")

namespace AetherSDR::hl2 {

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
    m_dsp = new Hl2RxDsp(nullptr);
    m_txDsp = new Hl2TxDsp(nullptr);

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
    m_dsp->moveToThread(m_ioThread);
    m_txDsp->moveToThread(m_ioThread);
    m_ioThread->start();

    // Wire: raw IQ -> DSP. Both objects live on the I/O thread, so this stays a
    // DIRECT call -- the sample path never touches the GUI thread or a queue.
    connect(m_metis, &MetisClient::iqBlockReady, m_dsp, &Hl2RxDsp::processIqBlock);

    // Link lifecycle: first EP6 -> connected; stop -> disconnected.
    connect(m_metis, &MetisClient::linkUp, this, [this] {
        m_connected = true;
        emit connected();
        // Publish initial slice/pan state AFTER connected(), not in connectRadio():
        // RadioModel::onConnected() stages every existing model as "previous
        // session" leftovers, so anything emitted earlier is wiped before the UI
        // ever sees it (slice panel stuck empty / 0.000000).
        emitSliceState();
        emitPanState();
        defineMeters();
        pushInitialState();
    });
    connect(m_metis, &MetisClient::linkDown, this, [this] {
        if (m_connected) {
            m_connected = false;
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
        emit connectionError(QStringLiteral("Hermes-Lite 2: %1").arg(reason));
    });

    // DSP outputs -> seam data plane + S-meter.
    connect(m_dsp, &Hl2RxDsp::spectrumReady, this,
            [this](const std::vector<float>& bins) {
        // dBFS -> dBm through the one object that owns the reference. With an
        // uncalibrated fullScaleDbm this is a pure -lnaGain shift, which is the
        // part that is exactly right: it holds the trace still across a gain
        // change instead of letting the whole display jump.
        const double off = m_dbRef.offsetDb();
        if (off == 0.0) {
            emit spectrumFrameReady(0, floatBytes(bins));
            return;
        }
        std::vector<float> dbm(bins.size());
        for (std::size_t i = 0; i < bins.size(); ++i)
            dbm[i] = static_cast<float>(bins[i] + off);
        emit spectrumFrameReady(0, floatBytes(dbm));
    });
    connect(m_dsp, &Hl2RxDsp::audioReady, this,
            [this](const std::vector<float>& pcm) {
        // Belt and braces with the demodulator mute below. This drops any block
        // that was already in flight when the key went down; Hl2RxDsp::
        // setAudioMuted stops the pipeline FILLING with our own transmission,
        // which is what stopped the tail draining out afterwards.
        if (m_keyed)
            return;
        emit audioFrameReady(floatBytes(pcm));
    });
    // Modulated IQ -> the wire. Both live on the I/O thread, so this is a direct
    // call and the transmit path never touches the GUI thread.
    connect(m_txDsp, &Hl2TxDsp::iqReady, m_metis,
            [this](const std::vector<std::complex<float>>& iq) {
        m_metis->queueTxIq(iq);
    });
    connect(m_txDsp, &Hl2TxDsp::micPeak, this,
            [this](float dbfs) { emit meterUpdate(QStringLiteral("TX:MICPEAK"), dbfs); });

    connect(m_metis, &MetisClient::telemetryUpdated, this,
            [this](const Hl2Telemetry& t) { publishTelemetry(t); });
    // Mirror the drop counter onto this thread so healthSnapshot() can read it
    // without touching an object that lives on the I/O thread.
    connect(m_metis, &MetisClient::dropsUpdated, this,
            [this](quint64 drops) { m_drops = drops; });
    connect(m_dsp, &Hl2RxDsp::meterUpdate, this,
            [this](float dbfs) {
        // Same reference as the spectrum -- a meter that moved on a gain change
        // while the trace stayed put would be its own kind of lie.
        const double dbm = m_dbRef.toDbm(dbfs);

        // Smooth EVERY sample, publish only on the tick. Both halves matter:
        // smoothing all of them is what makes the published value represent the
        // whole interval rather than one arbitrary instant inside it, and the
        // tick is what stops ~47 cross-thread emits a second repainting a
        // widget nobody can read that fast. See kMeterPublishIntervalMs.
        if (!m_haveSMeter) {
            m_sMeterDbm = dbm;
            m_haveSMeter = true;
        } else {
            const double alpha = (dbm > m_sMeterDbm) ? kMeterAttackAlpha
                                                     : kMeterDecayAlpha;
            m_sMeterDbm = alpha * dbm + (1.0 - alpha) * m_sMeterDbm;
        }
        if (m_sMeterClock.isValid()
            && m_sMeterClock.elapsed() < kMeterPublishIntervalMs)
            return;
        m_sMeterClock.restart();
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), m_sMeterDbm);
    });
}

Hl2Backend::~Hl2Backend()
{
    if (m_ioThread) {
        // Stop the wire ON its own thread and WAIT for it. A queued stop() would
        // never run -- quit() below ends the event loop that would deliver it --
        // and tearing the socket down from this thread is the affinity bug this
        // whole change exists to avoid.
        if (m_metis)
            QMetaObject::invokeMethod(m_metis, "stop", Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    } else if (m_metis) {
        QMetaObject::invokeMethod(m_metis, "stop");
    }
    // Safe now: the thread is joined, so nothing can be running in either object.
    delete m_txDsp;
    delete m_dsp;
    delete m_metis;
}

RadioCapabilities Hl2Backend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("hl2");
    c.model = QStringLiteral("Hermes-Lite 2");
    c.maxSlices = 1;
    c.maxPanadapters = 1;
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
    c.hostModulates = true;             // PC runs the modulator; no on-radio mic jacks
    c.txPowerMaxWatts = 0.0;            // uncalibrated; see the oracle on power counts
    c.hasTuner = false;
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
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

    // The span the operator last chose, snapped to a rate we can actually run
    // and to the current low-bandwidth ceiling. Applied BEFORE the explicit
    // param below so an automation/test caller can still pin a rate outright.
    //
    // Restoring this is what lets the default stay at the cheap 48 kHz: the
    // operator picks a wide span once and keeps it, instead of re-zooming every
    // launch, and nobody who never asked for it pays 25 Mbps.
    if (const double remembered = Hl2Settings::spanMhz(); remembered > 0.0)
        m_sampleRateHz = nearestIqSampleRateHz(remembered * 1.0e6);

    // Optional overrides from the namespaced params.
    if (request.params.contains(QStringLiteral("sampleRateHz")))
        m_sampleRateHz = request.params.value(QStringLiteral("sampleRateHz")).toInt();
    if (request.params.contains(QStringLiteral("lnaGainDb")))
        m_lnaGainDb = request.params.value(QStringLiteral("lnaGainDb")).toInt();
    // m_dbRef is synced to the final m_lnaGainDb unconditionally at the seed
    // below (right before the wire command), so it cannot drift regardless of
    // which override params were supplied.
    if (request.params.contains(QStringLiteral("rxFrequencyHz")))
        m_rxFreqHz = request.params.value(QStringLiteral("rxFrequencyHz")).toDouble();

    Hl2RxDsp::Config dc;
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
    dc.mode = modeFromString(m_mode);
    dc.filterLowHz = m_filterLowHz;
    dc.filterHighHz = m_filterHighHz;
    std::string err;
    // Blocking: the caller needs the result, and the DSP must be configured
    // before the wire starts delivering samples into it.
    bool dspOk = false;
    QMetaObject::invokeMethod(m_dsp, [this, &dc, &err, &dspOk] {
        dspOk = m_dsp->configure(dc, &err);
    }, Qt::BlockingQueuedConnection);
    if (!dspOk) {
        emit connectionError(QStringLiteral("HL2 DSP: %1").arg(QString::fromStdString(err)));
        return;
    }

    MetisClient::Params mp;
    mp.host = host;
    mp.port = request.port ? request.port : kMetisPort;
    mp.sampleRate = sampleRateEnum(m_sampleRateHz);
    mp.rxFrequencyHz = static_cast<std::uint32_t>(m_rxFreqHz < 0 ? 0 : m_rxFreqHz);
    mp.lnaGainDb = m_lnaGainDb;
    // The filter board has to be right from the FIRST config frame, not from
    // the operator's first band change. m_rxFreqHz here is the persisted
    // frequency this session is coming up on, so a launch straight onto 40 m
    // starts with the 40 m low-pass engaged rather than with whatever the last
    // session left in the relays — the radio never reports its own state, so
    // "unchanged since last time" is indistinguishable from "correct".
    mp.ocFilterByte = ocFilterByteForHz(m_rxFreqHz);
    m_ocFilterByte = static_cast<int>(mp.ocFilterByte);
    qCInfo(lcHl2).nospace()
        << "HL2 band filter: " << QString::asprintf("0x%02X", m_ocFilterByte)
        << " (" << ocFilterName(mp.ocFilterByte) << ") for "
        << QString::number(m_rxFreqHz / 1.0e6, 'f', 6) << " MHz, trigger=connect";
    // Seed the reference from the gain we are about to command, so the very
    // first spectrum frame is already on the same footing as every later one.
    m_dbRef.setLnaGainDb(m_lnaGainDb);
    // Blocking: start() constructs the QUdpSocket, which must take the I/O
    // thread's affinity, and we need to know whether the bind succeeded.
    bool started = false;
    QMetaObject::invokeMethod(m_metis, [this, &mp, &started] {
        started = m_metis->start(mp);
    }, Qt::BlockingQueuedConnection);
    if (!started) {
        emit connectionError(QStringLiteral("HL2: could not open the UDP socket"));
        return;
    }
    // Assert a known TX drive rather than inheriting whatever the board held.
    // ZERO is deliberate: this backend has no drive-level control wired to the
    // UI yet, so anything higher would be an un-commanded power level chosen by
    // a default. An operator raising it explicitly is the only way it should go up.
    Hl2TxDsp::Config tc;
    tc.inputSampleRateHz = 24000;    // AudioEngine's rate; submitTxAudio re-checks
    tc.outputSampleRateHz = 48000;   // EP2 is fixed at 48 kHz
    tc.mode = modeFromString(m_mode);
    // Sideband-correct from the first key, not from the first mode change. The
    // struct default is a positive 300..2700, so connecting straight into LSB
    // or DIGL would otherwise transmit on the upper sideband until the operator
    // happened to change mode.
    {
        const auto [txLo, txHi] = defaultTxPassbandForMode(m_mode);
        tc.filterLowHz  = txLo;
        tc.filterHighHz = txHi;
    }
    std::string txErr;
    bool txOk = false;
    QMetaObject::invokeMethod(m_txDsp, [this, &tc, &txErr, &txOk] {
        txOk = m_txDsp->configure(tc, &txErr);
    }, Qt::BlockingQueuedConnection);
    if (!txOk)
        qWarning() << "Hl2Backend: TX DSP unavailable —"
                   << QString::fromStdString(txErr) << "(receive is unaffected)";

    setTxDriveLevel(0);

    // Initial slice/pan state is published from the linkUp handler above, once
    // connected() has fired and RadioModel has finished staging the old session.
}

void Hl2Backend::disconnectRadio()
{
    if (m_metis)
        // Queued: serialises behind whatever the I/O thread is doing.
        QMetaObject::invokeMethod(m_metis, "stop");   // linkDown -> disconnected()
}

bool Hl2Backend::isConnected() const
{
    return m_connected;
}

void Hl2Backend::setSliceFrequency(int /*sliceId*/, double hz)
{
    m_rxFreqHz = hz;

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
    if (std::abs(hz - m_ncoHz) > usableHz) {
        m_ncoHz = hz;
        if (m_metis)
            QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
                Q_ARG(std::uint32_t, static_cast<std::uint32_t>(hz < 0 ? 0 : hz)));
    }

    // Shift by the slice's offset from the NCO, with the SAME sign.
    //
    // Derivable, now that the handedness is settled: the wire puts a signal at
    // frequency F at -(F - NCO), so mapping the slice's own frequency to
    // baseband needs -(slice - NCO) + shift == 0, i.e. shift = slice - NCO.
    // hl2_shift_test measures exactly that. (This sign is unchanged — it was
    // right all along; what was wrong was the conjugation in Hl2RxDsp, which is
    // why the stage looked correct only in LSB.)
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, m_rxFreqHz - m_ncoHz));

    // The TX NCO is a SEPARATE register (addr 0x01) from the RX DDC and does not
    // follow the receiver. Without this the transmit oscillator keeps whatever
    // it last held — zero on a fresh boot — so keying would radiate at the wrong
    // frequency, or at DC, with nothing to indicate anything was wrong.
    //
    // The HL2 has one receiver and one transmitter, and its single slice is the
    // TX slice, so the TX NCO simply tracks the slice. Sent whether or not
    // transmit is enabled: this is oscillator setup, it keys nothing, and having
    // it already correct is part of what makes the eventual key safe.
    setTxFrequency(m_rxFreqHz);

    // The companion filter board follows the SLICE, not the NCO: it is band
    // hardware in the antenna path, and on transmit it is what keeps the
    // harmonics legal. Change-gated inside, so tuning within a band sends
    // nothing and only a band crossing moves the relays.
    applyBandFilter("tune");

    emitSliceState();
    emitPanState();
}

void Hl2Backend::setSliceMode(int /*sliceId*/, const QString& mode)
{
    const QString previous = m_mode;
    m_mode = mode;
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
        m_filterLowHz  = lo;
        m_filterHighHz = hi;
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
    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(m_filterLowHz)),
            Q_ARG(double, static_cast<double>(m_filterHighHz)));
    }

    // The transmit sideband follows the slice. Without this, switching to LSB
    // would receive on the lower sideband and still transmit on the upper.
    //
    // The passband half of that was missing entirely: Hl2TxDsp::setFilter
    // existed and had no caller, so the TX chain ran on its construction-time
    // 300..2700 for every mode of the session. Same ordering rule as RX.
    if (m_txDsp) {
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        const auto [txLo, txHi] = defaultTxPassbandForMode(mode);
        QMetaObject::invokeMethod(m_txDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(txLo)),
            Q_ARG(double, static_cast<double>(txHi)));
    }
    emitSliceState();
}

void Hl2Backend::setSliceFilter(int /*sliceId*/, int lowHz, int highHz)
{
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, lowHz), Q_ARG(double, highHz));
    emitSliceState();
}

void Hl2Backend::setSliceAgc(int /*sliceId*/, const QString& mode, int thresholdDb)
{
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
    m_agcMode = m.isEmpty() ? m_agcMode : m;
    m_agcThresholdDb = qBound(0, thresholdDb, 100);
    const double ceilingDb = m_agcThresholdDb * kAgcCeilingDbPerUnit;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgc), Q_ARG(double, ceilingDb));
    emitSliceState();
}

void Hl2Backend::setPanCenter(const QString& /*panId*/, double hz)
{
    // Moving the window means moving the DDC. The slice does NOT move with it —
    // that is the point of keeping the two separate — so its offset from the new
    // centre is recomputed and re-applied as a shift.
    if (hz <= 0.0)
        return;
    // A drag delivers a centre command every 33 ms and forwards every one. Skip
    // the ones that do not actually move the DDC rather than re-sending an
    // identical NCO bank ~30 times a second.
    if (hz == m_ncoHz)
        return;
    m_ncoHz = hz;
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setRxFrequencyHz", Qt::QueuedConnection,
            Q_ARG(std::uint32_t, static_cast<std::uint32_t>(hz)));
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, m_rxFreqHz - m_ncoHz));
    emitPanState();
}

void Hl2Backend::setPanBandwidth(const QString& /*panId*/, double hz)
{
    if (hz <= 0.0)
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

void Hl2Backend::setPanRfGain(const QString& /*panId*/, int gainDb)
{
    const int clamped = qBound(kLnaGainMinDb, gainDb, kLnaGainMaxDb);
    if (clamped == m_lnaGainDb)
        return;
    m_lnaGainDb = clamped;

    // Move the dB reference IN LOCKSTEP with the gain, and do it here rather
    // than letting the two drift. The spectrum and the S-meter are both
    // rendered through m_dbRef, so without this every gain change would slide
    // the whole trace up or down the display — an operator backing off 10 dB on
    // a strong band would watch the noise floor drop 10 dB and read it as the
    // band going quiet, which is the opposite of what they just did.
    m_dbRef.setLnaGainDb(m_lnaGainDb);

    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setLnaGainDb", Qt::QueuedConnection,
            Q_ARG(int, m_lnaGainDb));

    qCInfo(lcHl2) << "HL2 LNA gain:" << m_lnaGainDb << "dB (requested" << gainDb << ")";

    // Echo what the hardware actually took. The slider is free to have asked
    // for something outside the register's range, and this is where it finds
    // out it did not get it.
    emit panRfGainChanged(QString::fromLatin1(kPanId), m_lnaGainDb);
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
        emitPanState();
        return;
    }

    const int previousRate = m_sampleRateHz;
    m_sampleRateHz = rate;

    // The DDC rate lives in the config register (C0=0x00), latched into the next
    // C&C round. Deliberately NOT followed by a filter-pipeline reset: sending
    // 0x39 on every geometry change is what wedged a board hard enough to need a
    // power cycle (see MetisClient::requestPipelineReset). The decimation filters
    // settle on their own within a few blocks.
    if (m_metis)
        QMetaObject::invokeMethod(m_metis, "setSampleRate", Qt::QueuedConnection,
            Q_ARG(AetherSDR::hl2::SampleRate, sampleRateEnum(rate)));

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
    if (m_dsp) {
        Hl2RxDsp::Config dc;
        dc.inputSampleRateHz = m_sampleRateHz;
        dc.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
        dc.mode = modeFromString(m_mode);
        dc.filterLowHz = m_filterLowHz;
        dc.filterHighHz = m_filterHighHz;
        // Carried through the rebuild rather than reapplied afterwards. A
        // reconfigured channel opens on Config's defaults, so an operator who had
        // moved their AGC would have had it silently snap back to medium/39 dB
        // every time they zoomed.
        dc.agcMode = wdspAgcMode(m_agcMode);
        dc.maximumAgcGainDb = m_agcThresholdDb * kAgcCeilingDbPerUnit;
        std::string err;
        bool ok = false;
        QMetaObject::invokeMethod(m_dsp, [this, &dc, &err, &ok] {
            ok = m_dsp->configure(dc, &err);
        }, Qt::BlockingQueuedConnection);
        if (!ok) {
            // Failing back to the old rate keeps the wire and the DSP agreeing.
            // The alternative — leaving the register commanded to a rate the DSP
            // cannot process — is silent: audio would be wrong with nothing in
            // the UI to say why.
            qWarning() << "Hl2Backend: could not reconfigure RX DSP for"
                       << m_sampleRateHz << "Hz —"
                       << QString::fromStdString(err)
                       << "— staying at" << previousRate << "Hz";
            m_sampleRateHz = previousRate;
            if (m_metis)
                QMetaObject::invokeMethod(m_metis, "setSampleRate",
                    Qt::QueuedConnection,
                    Q_ARG(AetherSDR::hl2::SampleRate,
                          sampleRateEnum(previousRate)));
            emitPanState();
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
    setSliceFrequency(kSliceId, m_rxFreqHz);
}

void Hl2Backend::setPanFrameRate(const QString& /*panId*/, int fps)
{
    // Straight through to the DSP, which skips the FFT itself when a frame is
    // not due. Queued: the cap is read on the DSP thread.
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setSpectrumRateFps", Qt::QueuedConnection,
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
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
            Q_ARG(bool, key));

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
    if (!key)
        m_tuning = false;   // an unkey ends tune too, however it was started
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
    QMetaObject::invokeMethod(m_metis, "setTxFrequencyHz", Qt::QueuedConnection,
        Q_ARG(std::uint32_t, static_cast<std::uint32_t>(hz)));
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

void Hl2Backend::setTune(bool on)
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
    m_tuning = on;
    if (on) {
        setTxTestTone(0.0, kTuneCarrierAmplitude);
        m_toneFromTune = true;   // set AFTER: setTxTestTone clears the flag
        setKeying(true);
    } else {
        setKeying(false);
        setTxTestTone(0.0, 0.0);
    }
}

void Hl2Backend::setTxPower(int percent)
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
    // The operator's 0..100 maps onto the HL2's 0..255 drive field. The gateware
    // only decodes the top nibble, so the effective resolution is coarser than
    // this suggests — the mapping is linear in the register, NOT calibrated to
    // watts, and nothing here should imply otherwise.
    const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    setTxDriveLevel(clamped * kTxDriveMax / 100);
}

void Hl2Backend::setTxDriveLevel(int level)
{
    if (!m_metis)
        return;
    QMetaObject::invokeMethod(m_metis, "setTxDriveLevel", Qt::QueuedConnection,
        Q_ARG(int, level));
}

void Hl2Backend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/, quint64 requestId,
                                 const QVariant& /*arg*/)
{
    // No HL2 extension verbs yet; honor the async contract without hanging.
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
    put("forwardPowerW", QStringLiteral("Forward (W, approx)"),
        m_telemetry.forwardPowerRaw
            ? QVariant(directionalWatts(*m_telemetry.forwardPowerRaw)) : QVariant());
    put("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),
        opt(m_telemetry.reversePowerRaw));
    put("reversePowerW", QStringLiteral("Reverse (W, approx)"),
        m_telemetry.reversePowerRaw
            ? QVariant(directionalWatts(*m_telemetry.reversePowerRaw)) : QVariant());
    // Meaningful without calibration — it is a ratio of two readings from the
    // same converter, so the unknown scale cancels. Absent below the noise
    // floor, where a ratio of two noise samples is not a mismatch reading.
    {
        QVariant swr;
        if (m_telemetry.forwardPowerRaw && m_telemetry.reversePowerRaw) {
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
    put("rxFrequencyHz", QStringLiteral("Slice frequency (Hz)"),
        static_cast<qulonglong>(m_rxFreqHz < 0 ? 0 : m_rxFreqHz));
    put("ncoHz", QStringLiteral("DDC / pan centre (Hz)"),
        static_cast<qulonglong>(m_ncoHz < 0 ? 0 : m_ncoHz));

    section("sampleRateHz", QStringLiteral("Link"));
    put("sampleRateHz", QStringLiteral("IQ sample rate (Hz)"), m_sampleRateHz);
    // Cumulative EP6 sequence gaps. The oracle is blunt that UDP loss on a
    // marginal link is the most common cause of "the audio sounds wrong", so
    // this is the first number to look at when it does.
    put("droppedPackets", QStringLiteral("Dropped EP6 packets"),
        static_cast<qulonglong>(m_drops));
    return h;
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
    setTxFrequency(m_rxFreqHz);
    // NOT the drive level. connectRadio() already asserts a safe 0 before the
    // link comes up, and by the time this runs RadioModel has pushed the
    // operator's actual RF power — emit connected() above is synchronous, so
    // resetting here silently undid it and the radio transmitted at drive 0
    // with the PA disabled. Caught by measurement: forward power went to 0.

    if (m_dsp) {
        QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(m_mode)));
        QMetaObject::invokeMethod(m_dsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(m_filterLowHz)),
            Q_ARG(double, static_cast<double>(m_filterHighHz)));
        QMetaObject::invokeMethod(m_dsp, "setAudioMuted", Qt::QueuedConnection,
            Q_ARG(bool, false));
    }
    if (m_txDsp) {
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(m_mode)));
        QMetaObject::invokeMethod(m_txDsp, "reset", Qt::QueuedConnection);
    }
    // How far this pan may be zoomed, which on this radio is simply the range of
    // DDC rates it can run. Pushed here for the same reason everything else in
    // this function is: nothing above the seam can derive it. The GUI's fallback
    // clamp is a FlexLib model table, and "Hermes-Lite 2" falls through it to
    // 5.4 MHz — fourteen times more than the widest window this receiver has, so
    // the operator could zoom out into spectrum that was never sampled and the
    // uncovered part rendered as black bars.
    emit panBandwidthLimitsChanged(
        QString::fromLatin1(kPanId),
        static_cast<double>(kIqSampleRatesHz[0]) / 1.0e6,
        static_cast<double>(maxIqSampleRateHz()) / 1.0e6);

    // What the RF Gain slider is allowed to ask for, and where it currently
    // sits. Pushed here for the same reason as everything else in this
    // function: the model's default is Flex's -8..+32 in 8 dB steps, learned
    // from a "display pan rfgain_info" command that answers nothing on this
    // radio, so without this the slider would misrepresent both the range and
    // the resolution of the AD9866's LNA.
    emit panRfGainInfoChanged(QString::fromLatin1(kPanId),
                              kLnaGainMinDb, kLnaGainMaxDb, kLnaGainStepDb);
    emit panRfGainChanged(QString::fromLatin1(kPanId), m_lnaGainDb);

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
        0.0, 41.0,     QStringLiteral("Forward power (uncalibrated)"));
    def(3, QStringLiteral("TX"),  QStringLiteral("REFPWR"),  QStringLiteral("dBm"),
        0.0, 41.0,     QStringLiteral("Reflected power (uncalibrated)"));
    def(4, QStringLiteral("TX"),  QStringLiteral("SWR"),     QStringLiteral("SWR"),
        1.0, 10.0,     QStringLiteral("Standing wave ratio"));
    def(5, QStringLiteral("RAD"), QStringLiteral("PATEMP"),  QStringLiteral("degC"),
        0.0, 100.0,    QStringLiteral("PA temperature"));
    def(6, QStringLiteral("TX"),  QStringLiteral("MICPEAK"), QStringLiteral("dBFS"),
        -100.0, 0.0,   QStringLiteral("Microphone peak"));
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
    // noise floor, not a calibrated power level.
    static constexpr int kMinForwardCountsForSwr = 16;
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
    if (t.forwardPowerRaw)
        emit meterUpdate(QStringLiteral("TX:FWDPWR"),
                         wattsToDbm(directionalWatts(*t.forwardPowerRaw)));
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
    if (!m_metis)
        return;
    const int oc = static_cast<int>(ocFilterByteForHz(m_rxFreqHz));
    if (oc == m_ocFilterByte)
        return;
    const int previous = m_ocFilterByte;
    m_ocFilterByte = oc;

    // INFO, not debug. There is no readback: the gateware forwards this byte to
    // the filter board over I2C and nothing comes back, so this log line is the
    // ONLY evidence of what the relays were told to do. A support log captured
    // after the fact has to already contain it. (aether.hl2)
    qCInfo(lcHl2).nospace()
        << "HL2 band filter: " << QString::asprintf("0x%02X", oc)
        << " (" << ocFilterName(static_cast<std::uint8_t>(oc)) << ") for "
        << QString::number(m_rxFreqHz / 1.0e6, 'f', 6) << " MHz"
        << " — was "
        << (previous < 0 || previous > 0x7F
                ? QStringLiteral("unset")
                : QString::asprintf("0x%02X", previous))
        << ", trigger=" << reason;

    QMetaObject::invokeMethod(m_metis, "setBandFilter", Qt::QueuedConnection,
        Q_ARG(int, oc));
}

void Hl2Backend::emitSliceState()
{
    SliceDelta d;
    d.panId = QString::fromLatin1(kPanId);
    d.frequency = m_rxFreqHz / 1.0e6;   // MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    // The HL2 has exactly one receiver and one transmitter, so its single slice
    // IS the transmit slice. Leaving this unset meant txSlice() was null and
    // every key attempt died in RadioModel's interlock with "No transmit slice
    // is assigned" -- before the backend was ever asked, which is why the
    // refusal was silent from down here.
    //
    // Publishing it unconditionally is correct rather than convenient: there is
    // no second slice for the assignment to be a choice between.
    d.txSlice = true;
    d.active = true;
    emit sliceChanged(kSliceId, d);
}

void Hl2Backend::emitPanState()
{
    // The pan centre is the NCO, NOT the slice. This is the whole point of the
    // decoupling: the display describes where the receiver's window is, and the
    // slice moves inside it.
    emit panCenterBandwidthChanged(QString::fromLatin1(kPanId), m_ncoHz / 1.0e6,
                                   static_cast<double>(m_sampleRateHz) / 1.0e6);
}

}  // namespace AetherSDR::hl2
