#include "core/backends/hl2/Hl2Backend.h"

#include <cmath>

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QByteArray>
#include <QHostAddress>

#include <cstdint>

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

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("hl2-io"));
    m_metis->moveToThread(m_ioThread);
    m_dsp->moveToThread(m_ioThread);
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
            [this](const std::vector<float>& pcm) { emit audioFrameReady(floatBytes(pcm)); });
    connect(m_dsp, &Hl2RxDsp::meterUpdate, this,
            [this](float dbfs) {
        // Same reference as the spectrum -- a meter that moved on a gain change
        // while the trace stayed put would be its own kind of lie.
        emit meterUpdate(QStringLiteral("s-meter"),
                         static_cast<float>(m_dbRef.toDbm(dbfs)));
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
    c.canTransmit = false;              // RX-only: the engine TX guard denies keying
    c.txPowerMaxWatts = 0.0;
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

    emitSliceState();
    emitPanState();
}

void Hl2Backend::setSliceMode(int /*sliceId*/, const QString& mode)
{
    m_mode = mode;
    if (m_dsp)
        QMetaObject::invokeMethod(m_dsp, "setMode", Qt::QueuedConnection,
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

void Hl2Backend::setKeying(bool /*key*/)
{
    // RX-only. capabilities().canTransmit is false, so the engine guard already
    // denies keying above the seam; this is a defensive no-op.
}

void Hl2Backend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/, quint64 requestId,
                                 const QVariant& /*arg*/)
{
    // No HL2 extension verbs yet; honor the async contract without hanging.
    if (requestId != 0)
        emit extensionError(requestId, QStringLiteral("hl2: no extension verbs implemented"));
}

void Hl2Backend::emitSliceState()
{
    SliceDelta d;
    d.panId = QString::fromLatin1(kPanId);
    d.frequency = m_rxFreqHz / 1.0e6;   // MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
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
