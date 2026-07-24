#include "core/backends/sim/SimBackend.h"

#include <QtEndian>
#include <QThread>

#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"

namespace AetherSDR {

SimBackend::SimBackend(QObject* parent) : IRadioBackend(parent)
{
    // Frame cadence: kFrameLen samples at kSampleRate ≈ 5.33 ms. A CoarseTimer
    // is fine — feedAudioData() buffers, and demo audio needs no sample-accurate
    // pacing. Started on connect, stopped on disconnect.
    m_audioTimer.setTimerType(Qt::CoarseTimer);
    m_audioTimer.setInterval(
        (NoiseMixer::kFrameLen * 1000) / NoiseMixer::kSampleRate);
    connect(&m_audioTimer, &QTimer::timeout, this, &SimBackend::onAudioTick);
    // Give the demo something audible out of the box: a pink-noise floor plus a
    // birdie carrier — a scene that immediately shows what NR/notch can do.
    m_audio.setEnabled(NoiseMixer::Channel::Pink, true);
    m_audio.setLevelDb(NoiseMixer::Channel::Pink, -22.0);
    m_audio.setEnabled(NoiseMixer::Channel::Birdie, true);
    m_audio.setLevelDb(NoiseMixer::Channel::Birdie, -18.0);
    m_audio.setKnob(NoiseMixer::Channel::Birdie, QStringLiteral("hz"), 1200.0);

    // ---- Path B (RFC #4288): own a RadioConnection + PanadapterStream in
    // synthetic-demo mode, mirroring FlexBackend's ctor (same load-bearing #502
    // order: panStream thread FIRST, then connection). RadioModel harvests these
    // via connection()/panStream() and drives them exactly as it drives Flex's.
    // The synthetic behaviour is entirely inside those classes
    // (RadioConnection::startSyntheticDemoConnect on a demo target,
    // PanadapterStream::tickSyntheticDemo) — no new wire logic here.
    m_networkThread = new QThread(this);
    m_networkThread->setObjectName("SimPanadapterStream");
    m_panStream = new PanadapterStream;   // no parent — moved to thread
    m_panStream->moveToThread(m_networkThread);
    connect(m_networkThread, &QThread::started, m_panStream, &PanadapterStream::init);
    m_networkThread->start();

    m_connThread = new QThread(this);
    m_connThread->setObjectName("SimRadioConnection");
    m_connection = new RadioConnection;   // no parent — moved to thread
    m_connection->moveToThread(m_connThread);
    connect(m_connThread, &QThread::started, m_connection, &RadioConnection::init);
    m_connThread->start();

    // Re-emit wire lifecycle as the interface's own signals (as FlexBackend does).
    connect(m_connection, &RadioConnection::connected,
            this, &IRadioBackend::connected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &IRadioBackend::disconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &IRadioBackend::connectionError);
}

SimBackend::~SimBackend()
{
    // Mirror FlexBackend teardown: sever our lifecycle observation first, then
    // tear down connection (BlockingQueued disconnect → deleteLater → thread
    // quit/wait), then panStream — the #502 order.
    if (m_connection)
        disconnect(m_connection, nullptr, this, nullptr);

    if (m_connection && m_connThread && m_connThread->isRunning()) {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
        m_connection->deleteLater();
        m_connThread->quit();
        m_connThread->wait(3000);
    } else {
        delete m_connection;
    }
    m_connection = nullptr;

    if (m_panStream && m_networkThread && m_networkThread->isRunning()) {
        QMetaObject::invokeMethod(m_panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        m_panStream->deleteLater();
        m_networkThread->quit();
        m_networkThread->wait(3000);
    } else {
        delete m_panStream;
    }
    m_panStream = nullptr;
}

QByteArray SimBackend::toStereoBytes(const QVector<float>& mono)
{
    // AudioEngine::feedAudioData() wants 24 kHz STEREO float32: duplicate each
    // mono sample into L and R. Little-endian float32 (native x86/ARM order the
    // engine's downstream DSP reads).
    QByteArray out;
    out.resize(mono.size() * 2 * static_cast<int>(sizeof(float)));
    auto* p = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < mono.size(); ++i) {
        p[2 * i] = mono[i];
        p[2 * i + 1] = mono[i];
    }
    return out;
}

void SimBackend::onAudioTick()
{
    if (!m_connected) {
        return;
    }
    // Muted while keyed — a demo radio must never sound live on TX (Principle VI).
    const QVector<float> frame = m_keyed
        ? QVector<float>(NoiseMixer::kFrameLen, 0.0f)
        : m_audio.mixFrame();
    emit audioFrameReady(toStereoBytes(frame));

    // A panadapter row a few times a second (not every audio frame — the display
    // updates far slower than audio). ~20 fps at the 5.33 ms tick ≈ every 9th.
    if (++m_audioFrames % 9 == 0) {
        constexpr int kBins = 1024;
        constexpr double kFloorDbm = -120.0;
        constexpr double kAudioSpanHz = 8000.0;   // show ±4 kHz around the VFO
        const QVector<float> row =
            m_audio.spectrum(kBins, kFloorDbm, kAudioSpanHz, kBins / 2);
        QByteArray bytes(reinterpret_cast<const char*>(row.constData()),
                         row.size() * static_cast<int>(sizeof(float)));
        emit spectrumFrameReady(kPanId, bytes);
    }
}

QString SimBackend::demoModelName() { return QStringLiteral("AetherSDR Demo"); }
QString SimBackend::demoSerial()    { return QStringLiteral("DEMO-0001"); }

RadioCapabilities SimBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("sim");
    caps.model  = demoModelName();
    caps.maxSlices = 1;          // Phase 1: a single slice. Phase 2 raises this.
    caps.maxPanadapters = 1;
    caps.sampleRatesHz = {};     // no data plane yet
    // A simulator must never look like something that can key a transmitter
    // (Principle VI). TX stays off in the skeleton.
    caps.canTransmit = false;
    caps.txPowerMaxWatts = 0.0;
    caps.hasTuner = false;
    caps.hasAmplifier = false;
    caps.hasExtendedDsp = false;
    return caps;
}

void SimBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    if (m_connected) {
        return;   // idempotent — a second connect is a no-op, not a reset
    }
    m_connected = true;
    emit connected();
    emit capabilitiesChanged();
    emitInitialState();
    m_audioTimer.start();   // begin delivering synthetic RX audio + spectrum
}

void SimBackend::disconnectRadio()
{
    if (!m_connected) {
        return;
    }
    m_audioTimer.stop();
    m_connected = false;
    emit sliceRemoved(kSliceId);
    emit disconnected();
}

bool SimBackend::isConnected() const { return m_connected; }

void SimBackend::emitInitialState()
{
    // Radio-global snapshot: identity + free-slot count. Present-only fields.
    RadioDelta radio;
    radio.model = demoModelName();
    radio.nickname = QStringLiteral("Demo Simulator");
    radio.slicesAvailable = capabilities().maxSlices;
    emit radioChanged(radio);

    // One active slice on a sensible default, so the UI has something live to
    // show the moment demo mode connects.
    SliceDelta slice;
    slice.letter = QStringLiteral("A");
    slice.frequency = m_sliceFreqMhz;
    slice.mode = m_sliceMode;
    slice.filterLow = m_filterLowHz;
    slice.filterHigh = m_filterHighHz;
    slice.active = true;
    slice.inUse = true;
    emit sliceChanged(kSliceId, slice);
}

void SimBackend::setSliceFrequency(int sliceId, double hz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceFreqMhz = hz / 1.0e6;
    SliceDelta d;
    d.frequency = m_sliceFreqMhz;
    emit sliceChanged(kSliceId, d);   // radio is authoritative: echo the change back (Principle II)
}

void SimBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceMode = mode;
    SliceDelta d;
    d.mode = m_sliceMode;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    SliceDelta d;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setKeying(bool key)
{
    // RX-only (capabilities().canTransmit == false): the engine TX guard above
    // the seam already denies keying. We DON'T transmit — but we do record the
    // intent so onAudioTick() mutes the synthetic RX while "keyed", so the demo
    // never plays receive audio over a (would-be) transmit (Principle VI).
    m_keyed = key;
}

void SimBackend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/,
                                 quint64 requestId, const QVariant& /*arg*/)
{
    // No vendor extensions in the skeleton. A requestId of 0 means "no reply
    // expected"; anything else gets an explicit error so a caller correlating a
    // reply is never left waiting.
    if (requestId != 0) {
        emit extensionError(requestId,
                            QStringLiteral("sim backend has no extensions"));
    }
}

}  // namespace AetherSDR
