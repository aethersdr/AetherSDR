#include "DaxIqModel.h"
#include "core/LogManager.h"

#include <QDebug>
#include <QProcess>
#include <QtEndian>
#include <cmath>
#include <cstring>
#ifndef Q_OS_WIN
#include <unistd.h>
#include <fcntl.h>
#endif

namespace MasterSDR {

// ─── DaxIqModel ──────────────────────────────────────────────────────────────

DaxIqModel::DaxIqModel(QObject* parent)
    : QObject(parent)
{
    for (int i = 0; i < NUM_CHANNELS; ++i)
        m_streams[i].channel = i + 1;

    m_worker = new DaxIqWorker;
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &DaxIqWorker::levelReady, this, &DaxIqModel::iqLevelReady);
    m_workerThread.start();
}

DaxIqModel::~DaxIqModel()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

const DaxIqModel::IqStream& DaxIqModel::stream(int channel) const
{
    static const IqStream empty;
    int idx = channelIndex(channel);
    if (idx < 0 || idx >= NUM_CHANNELS) return empty;
    return m_streams[idx];
}

void DaxIqModel::createStream(int channel)
{
    if (channel < 1 || channel > NUM_CHANNELS) return;
    emit commandReady(QString("stream create type=dax_iq daxiq_channel=%1").arg(channel));
}

void DaxIqModel::removeStream(int channel)
{
    int idx = channelIndex(channel);
    if (idx < 0 || idx >= NUM_CHANNELS) return;
    if (!m_streams[idx].exists || m_streams[idx].streamId == 0) return;
    emit commandReady(QString("stream remove 0x%1").arg(m_streams[idx].streamId, 0, 16));
}

void DaxIqModel::setSampleRate(int channel, int rate)
{
    int idx = channelIndex(channel);
    if (idx < 0 || idx >= NUM_CHANNELS) return;
    if (!m_streams[idx].exists || m_streams[idx].streamId == 0) return;
    emit commandReady(QString("stream set 0x%1 daxiq_rate=%2")
        .arg(m_streams[idx].streamId, 0, 16).arg(rate));
}

void DaxIqModel::applyStreamStatus(quint32 streamId, const QMap<QString, QString>& kvs)
{
    // Find which channel this stream belongs to
    int ch = -1;
    if (kvs.contains("daxiq_channel"))
        ch = kvs["daxiq_channel"].toInt();

    // Try to find existing stream by ID
    int idx = -1;
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        if (m_streams[i].streamId == streamId && m_streams[i].exists) {
            idx = i;
            break;
        }
    }

    // New stream — assign by channel
    if (idx < 0 && ch >= 1 && ch <= NUM_CHANNELS) {
        idx = channelIndex(ch);
        m_streams[idx].streamId = streamId;
        m_streams[idx].exists = true;
        qCDebug(lcProtocol) << "DaxIqModel: new IQ stream ch" << ch
                            << "id" << Qt::hex << streamId;
    }

    if (idx < 0) return;

    auto& s = m_streams[idx];

    if (kvs.contains("daxiq_rate")) {
        int newRate = kvs["daxiq_rate"].toInt();
        if (newRate != s.sampleRate) {
            s.sampleRate = newRate;
            // Recreate pipe at new sample rate
            QMetaObject::invokeMethod(m_worker, [this, ch = s.channel, newRate] {
                m_worker->destroyPipe(ch);
                m_worker->createPipe(ch, newRate);
            });
        }
    }
    if (kvs.contains("pan"))
        s.panId = kvs["pan"];
    if (kvs.contains("active"))
        s.active = kvs["active"] == "1";

    emit streamChanged(s.channel);
}

void DaxIqModel::handleStreamRemoved(quint32 streamId)
{
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        if (m_streams[i].streamId == streamId) {
            int ch = m_streams[i].channel;
            m_streams[i] = IqStream{};
            m_streams[i].channel = ch;
            QMetaObject::invokeMethod(m_worker, [this, ch] {
                m_worker->destroyPipe(ch);
            });
            emit streamChanged(ch);
            qCDebug(lcProtocol) << "DaxIqModel: removed IQ stream ch" << ch;
            return;
        }
    }
}

void DaxIqModel::feedRawIqPacket(int channel, const QByteArray& rawPayload, int sampleRate)
{
    QMetaObject::invokeMethod(m_worker,
        [this, channel, rawPayload, sampleRate] {
            m_worker->processIqPacket(channel, rawPayload, sampleRate);
        });
}

// ─── DaxIqWorker ─────────────────────────────────────────────────────────────

DaxIqWorker::DaxIqWorker(QObject* parent)
    : QObject(parent)
{
    for (int i = 0; i < DaxIqModel::NUM_CHANNELS; ++i) {
        m_pipeFds[i] = -1;
        m_sampleCount[i] = 0;
        m_sumSq[i] = 0.0;
    }
}

DaxIqWorker::~DaxIqWorker()
{
#ifndef Q_OS_WIN
    for (int i = 0; i < DaxIqModel::NUM_CHANNELS; ++i) {
        if (m_pipeFds[i] >= 0) {
            ::close(m_pipeFds[i]);
            m_pipeFds[i] = -1;
        }
    }
#endif
}

void DaxIqWorker::createPipe(int channel, int sampleRate)
{
#ifndef Q_OS_WIN
    int idx = channel - 1;
    if (idx < 0 || idx >= DaxIqModel::NUM_CHANNELS) return;
    if (m_pipeFds[idx] >= 0) destroyPipe(channel);

    QString pipePath = QString("/tmp/MasterSDR-iq-%1.pipe").arg(channel);

    // Create PulseAudio pipe source for IQ data (float32 stereo)
    QString cmd = QString(
        "pactl load-module module-pipe-source "
        "source_name=MasterSDR-iq-%1 "
        "file=%2 "
        "format=float32le rate=%3 channels=2 "
        "source_properties=device.description=\"MasterSDR\\ IQ\\ %1\"")
        .arg(channel).arg(pipePath).arg(sampleRate);

    QProcess::startDetached("bash", {"-c", cmd});

    // Open the pipe for writing (non-blocking to avoid stalling if no reader)
    // Give PulseAudio a moment to create the pipe
    QThread::msleep(200);
    int fd = ::open(pipePath.toLocal8Bit().constData(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        qCWarning(lcAudio) << "DaxIqWorker: cannot open IQ pipe" << pipePath;
        return;
    }
    m_pipeFds[idx] = fd;
    qCDebug(lcAudio) << "DaxIqWorker: opened IQ pipe ch" << channel
                      << "rate" << sampleRate;
#else
    Q_UNUSED(channel); Q_UNUSED(sampleRate);
#endif
}

void DaxIqWorker::destroyPipe(int channel)
{
#ifndef Q_OS_WIN
    int idx = channel - 1;
    if (idx < 0 || idx >= DaxIqModel::NUM_CHANNELS) return;
    if (m_pipeFds[idx] >= 0) {
        ::close(m_pipeFds[idx]);
        m_pipeFds[idx] = -1;
    }
    // Unload the PulseAudio module
    QString cmd = QString(
        "pactl list short modules | grep MasterSDR-iq-%1 | awk '{print $1}' | "
        "xargs -r -n1 pactl unload-module").arg(channel);
    QProcess::startDetached("bash", {"-c", cmd});
#else
    Q_UNUSED(channel);
#endif
}

void DaxIqWorker::processIqPacket(int channel, const QByteArray& rawPayload, int sampleRate)
{
    Q_UNUSED(sampleRate);
    int idx = channel - 1;
    if (idx < 0 || idx >= DaxIqModel::NUM_CHANNELS) return;

    const int numFloats = rawPayload.size() / 4;
    const int numSamples = numFloats / 2;  // I/Q pairs

    // Byte-swap float32 big-endian → native (little-endian)
    QByteArray swapped(rawPayload.size(), Qt::Uninitialized);
    const quint32* src = reinterpret_cast<const quint32*>(rawPayload.constData());
    quint32* dst = reinterpret_cast<quint32*>(swapped.data());
    for (int i = 0; i < numFloats; ++i)
        dst[i] = qFromBigEndian(src[i]);

    // Compute RMS magnitude for metering (every ~100ms worth of samples)
    const float* floats = reinterpret_cast<const float*>(swapped.constData());
    for (int i = 0; i < numSamples; ++i) {
        float I = floats[2 * i];
        float Q = floats[2 * i + 1];
        m_sumSq[idx] += static_cast<double>(I * I + Q * Q);
    }
    m_sampleCount[idx] += numSamples;

    // Emit level every ~2400 samples (~100ms at 24k, ~50ms at 48k)
    if (m_sampleCount[idx] >= 2400) {
        float rms = static_cast<float>(std::sqrt(m_sumSq[idx] / m_sampleCount[idx]));
        emit levelReady(channel, rms);
        m_sampleCount[idx] = 0;
        m_sumSq[idx] = 0.0;
    }

    // Write to pipe (non-blocking, Linux/macOS only)
#ifndef Q_OS_WIN
    if (m_pipeFds[idx] >= 0) {
        ::write(m_pipeFds[idx], swapped.constData(), swapped.size());
    }
#endif
}

} // namespace MasterSDR
