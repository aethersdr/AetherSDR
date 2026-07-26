#include "core/backends/hl2/Hl2Backend.h"

#include <cmath>

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include "core/AutomationBridgeSettings.h"

#include <QByteArray>
#include <QHostAddress>
#include <QLoggingCategory>

#include <cstdint>

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

WdspChannel::Mode modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    if (u == QLatin1String("CWL"))  return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("CWU"))  return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("FM"))   return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;
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
    connect(m_dsp, &Hl2RxDsp::meterUpdate, this,
            [this](float dbfs) {
        // Same reference as the spectrum -- a meter that moved on a gain change
        // while the trace stayed put would be its own kind of lie.
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), m_dbRef.toDbm(dbfs));
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
    c.sampleRatesHz = {48000, 96000, 192000, 384000};
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

    // Shift by the slice's offset from the NCO, with the SAME sign. Measured,
    // not reasoned: hl2_shift_test sweeps the stage and finds the mapping is
    // exactly audio = tone + shift for positive values (a tone 800 Hz below the
    // NCO lands at 2800 Hz of audio with the slice 2 kHz above it). The
    // intuition that "bringing a signal down to baseband must be negative" is
    // backwards here, and negative shifts push the signal out of the passband.
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

    emitSliceState();
    emitPanState();
}

void Hl2Backend::setSliceMode(int /*sliceId*/, const QString& mode)
{
    m_mode = mode;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(mode)));
    // The transmit sideband follows the slice. Without this, switching to LSB
    // would receive on the lower sideband and still transmit on the upper.
    if (m_txDsp)
        QMetaObject::invokeMethod(m_txDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(mode)));
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
    // Neutral vocabulary -> WDSP RXA AGC mode. WDSP also has "long" (1), which
    // the slice model's four-way control never produces, so it is unreachable
    // here rather than silently aliased onto something else.
    const QString m = mode.trimmed().toLower();
    int wdspAgc = 3;                                   // medium: WDSP's own default
    if (m == QLatin1String("off"))        wdspAgc = 0;
    else if (m == QLatin1String("slow"))  wdspAgc = 2;
    else if (m == QLatin1String("med"))   wdspAgc = 3;
    else if (m == QLatin1String("fast"))  wdspAgc = 4;

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
    def(2, QStringLiteral("TX"),  QStringLiteral("FWDPWR"),  QStringLiteral("dBm"),
        0.0, 50.0,     QStringLiteral("Forward power"));
    def(3, QStringLiteral("TX"),  QStringLiteral("REFPWR"),  QStringLiteral("dBm"),
        0.0, 50.0,     QStringLiteral("Reflected power"));
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
    // Raw directional counts, logged rather than published: they are what a
    // future calibration curve will be built from, and they are the only way to
    // tell "the radio reports no power" from "we never asked".
    if (t.forwardPowerRaw && (*t.forwardPowerRaw != m_lastFwdRaw)) {
        m_lastFwdRaw = *t.forwardPowerRaw;
        qCDebug(lcHl2Tx) << "HL2 directional: fwd" << *t.forwardPowerRaw
                         << "rev" << t.reversePowerRaw.value_or(-1);
    }
    // TX IQ FIFO depth — the oracle calls this the most important number in the
    // protocol. A queue-fed transmission can starve the radio's buffer in a way
    // a per-packet generated tone never can, so this is what distinguishes
    // "the audio is wrong" from "the audio never arrived".
    if (m_keyed && t.txFifoCount)
        qCDebug(lcHl2Tx) << "HL2 fifo:" << *t.txFifoCount
                         << "under" << t.txFifoUnderflow.value_or(false)
                         << "over" << t.txFifoOverflow.value_or(false);
    if (t.temperatureRaw)
        emit meterUpdate(QStringLiteral("RAD:PATEMP"), temperatureCelsius(*t.temperatureRaw));

    m_telemetry = t;
    if (t.adcOverload && *t.adcOverload != m_adcOverload) {
        m_adcOverload = *t.adcOverload;
        if (m_adcOverload)
            qWarning() << "Hl2Backend: ADC OVERLOAD — reduce LNA gain or attenuate";
    }
}

double Hl2Backend::temperatureCelsius(int raw)
{
    // AD9866 on-die temperature via the HL2's instrumentation ADC. The scaling
    // below is the Hermes-Lite 2 wiki's published formula. It is NOT verified
    // against a reference thermometer here, so treat it as indicative.
    return (3.26 * (static_cast<double>(raw) / 4096.0) - 0.5) / 0.01;
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
