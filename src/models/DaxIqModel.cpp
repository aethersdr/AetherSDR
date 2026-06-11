#include "DaxIqModel.h"
#include "core/LogManager.h"

#include <QDebug>
#include <QDir>
#include <QProcess>
#include <QtEndian>
#include <cmath>
#include <cstring>
#ifndef Q_OS_WIN
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>   // mkfifo, mkdir, chmod, lstat, S_ISFIFO
#include <cerrno>       // errno
#endif

namespace AetherSDR {

#ifndef Q_OS_WIN
namespace {

// Per-user FIFO dir ($XDG_RUNTIME_DIR/aethersdr, else /run/user/<uid>/aethersdr,
// else /tmp/aethersdr-<uid>), created 0700. Mirrors PipeWireAudioBridge::daxFifoDir
// — owner-only, NOT world-writable /tmp (GHSA-x8xf-4g5v-ppf9).
QString iqFifoDir()
{
    QString base = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (base.isEmpty()) {
        const QString runUser = QStringLiteral("/run/user/%1").arg(::getuid());
        base = QDir(runUser).exists() ? runUser
                                      : QStringLiteral("/tmp/aethersdr-%1").arg(::getuid());
    }
    const QString dir = base + QStringLiteral("/aethersdr");
    if (::mkdir(dir.toUtf8().constData(), 0700) != 0 && errno != EEXIST)
        return {};
    ::chmod(dir.toUtf8().constData(), 0700);
    return dir;
}

QString iqPipePath(int channel)
{
    const QString dir = iqFifoDir();
    return dir.isEmpty() ? QString()
                         : QStringLiteral("%1/iq-%2.pipe").arg(dir).arg(channel);
}

// TOCTOU-safe owner-only FIFO (mirrors PipeWireAudioBridge::makeOwnedFifo):
// mkfifo(0600); on EEXIST, only reuse it if it is our own FIFO, else refuse.
bool makeOwnedFifo(const QString& path)
{
    const QByteArray pb = path.toUtf8();
    if (::mkfifo(pb.constData(), 0600) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st{};
    if (::lstat(pb.constData(), &st) != 0 || !S_ISFIFO(st.st_mode)
        || st.st_uid != ::getuid())
        return false;                       // refuse to clobber a foreign file
    if (::unlink(pb.constData()) != 0) return false;
    return ::mkfifo(pb.constData(), 0600) == 0;
}

// Synchronous pactl; returns the loaded module index (0 on failure).
// Mirrors PipeWireAudioBridge::runPactl — replaces the old fire-and-forget
// QProcess::startDetached so we KNOW the module loaded (and can unload by index).
quint32 runPactlSync(const QStringList& args)
{
    QProcess proc;
    proc.start(QStringLiteral("pactl"), args);
    if (!proc.waitForFinished(5000)) {
        qCWarning(lcAudio) << "DaxIqWorker: pactl timed out:" << args;
        return 0;
    }
    if (proc.exitCode() != 0) {
        qCWarning(lcAudio) << "DaxIqWorker: pactl failed:"
                           << proc.readAllStandardError().trimmed();
        return 0;
    }
    bool ok = false;
    const quint32 idx = proc.readAllStandardOutput().trimmed().toUInt(&ok);
    return ok ? idx : 0;
}

} // namespace
#endif // !Q_OS_WIN

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
    bool isNew = false;
    if (idx < 0 && ch >= 1 && ch <= NUM_CHANNELS) {
        idx = channelIndex(ch);
        m_streams[idx].streamId = streamId;
        m_streams[idx].exists = true;
        isNew = true;
        qCDebug(lcProtocol) << "DaxIqModel: new IQ stream ch" << ch
                            << "id" << Qt::hex << streamId;
    }

    if (idx < 0) return;

    auto& s = m_streams[idx];

    // Create the IQ pipe when the stream first appears (isNew), not only on a
    // rate CHANGE. The default daxiq_rate=48000 equals IqStream::sampleRate{48000},
    // so a fresh enable at the default rate produced no rate delta and the pipe
    // was never built (no module-pipe-source, no /tmp/aethersdr-iq-N.pipe, no
    // node for WS2 to name). Guard merged: fire on existence OR rate change, and
    // keep it OUTSIDE the daxiq_rate-present check so a status message that
    // establishes the stream without carrying daxiq_rate still triggers it.
    // destroyPipe is idempotent (m_pipeFds[idx]>=0 guard), so destroy-then-create
    // is safe even with no prior pipe.
    {
        int newRate = kvs.contains("daxiq_rate") ? kvs["daxiq_rate"].toInt()
                                                  : s.sampleRate;
        if (isNew || newRate != s.sampleRate) {
            s.sampleRate = newRate;
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

    const QString pipePath = iqPipePath(channel);
    if (pipePath.isEmpty()) {
        qCWarning(lcAudio) << "DaxIqWorker: cannot resolve IQ FIFO dir for ch" << channel;
        return;
    }

    // Create the owner-only FIFO ourselves first, THEN load the pipe-source.
    // (1) mkfifo-first removes the create-race: the old code fire-and-forgot
    //     `pactl` and raced a 200ms sleep to open a FIFO module-pipe-source had
    //     not created yet -> ENOENT "cannot open IQ pipe", no node ever appeared.
    // (2) 0600 under $XDG_RUNTIME_DIR/aethersdr (not /tmp/aethersdr-iq-N.pipe,
    //     which was prw-rw-rw-) closes the world-writable-FIFO IQ-injection
    //     vector, mirroring the DAX-audio hardening (GHSA-x8xf-4g5v-ppf9).
    if (!makeOwnedFifo(pipePath)) {
        qCWarning(lcAudio) << "DaxIqWorker: mkfifo failed for" << pipePath;
        return;
    }

    // Load module-pipe-source SYNCHRONOUSLY so we know it succeeded and capture
    // the module index for a clean unload. source_properties is single-quoted so
    // the spaced device.description survives pipewire-pulse's module-arg parser
    // (mirrors the DAX RX/TX naming fix); node.description drives the label shown
    // in qpwgraph / JACK / WSJT-X.
    const quint32 modIdx = runPactlSync({
        "load-module", "module-pipe-source",
        QStringLiteral("source_name=aethersdr-iq-%1").arg(channel),
        QStringLiteral("file=%1").arg(pipePath),
        QStringLiteral("format=float32le"),
        QStringLiteral("rate=%1").arg(sampleRate),
        QStringLiteral("channels=2"),
        QStringLiteral("source_properties='device.description=\"AetherSDR DAX IQ %1\"'").arg(channel),
    });
    if (modIdx == 0) {
        ::unlink(pipePath.toLocal8Bit().constData());
        qCWarning(lcAudio) << "DaxIqWorker: pactl load-module failed for IQ ch" << channel;
        return;
    }
    m_pipeModuleIdx[idx] = modIdx;

    // Open the write end. O_RDWR (not O_WRONLY) never ENXIOs even if the module's
    // read side has not attached yet; we only ever write this fd.
    int fd = ::open(pipePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        qCWarning(lcAudio) << "DaxIqWorker: cannot open IQ pipe" << pipePath
                           << strerror(errno);
        runPactlSync({"unload-module", QString::number(modIdx)});
        ::unlink(pipePath.toLocal8Bit().constData());
        m_pipeModuleIdx[idx] = 0;
        return;
    }
    m_pipeFds[idx] = fd;
    qCDebug(lcAudio) << "DaxIqWorker: opened IQ pipe ch" << channel << "rate" << sampleRate
                     << "module" << modIdx << "path" << pipePath;
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
    // Unload the pipe-source module by its stored index (clean, no grep race),
    // then remove the FIFO we created.
    if (m_pipeModuleIdx[idx] != 0) {
        runPactlSync({"unload-module", QString::number(m_pipeModuleIdx[idx])});
        m_pipeModuleIdx[idx] = 0;
    }
    const QString pipePath = iqPipePath(channel);
    if (!pipePath.isEmpty())
        ::unlink(pipePath.toLocal8Bit().constData());
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

    // dax_iq payloads are LITTLE-endian float32 (the radio reports
    // payload_endian=little for this stream type — unlike pan/wf/meter/audio
    // which are big-endian network order). Reading them big-endian byte-reverses
    // the floats into denormals ≈ 0, which flat-lines the RMS meter AND writes
    // garbage to the format=float32le pipe. Read little-endian (a correct no-op
    // on an LE host) so both the meter and the pipe carry real IQ.
    QByteArray swapped(rawPayload.size(), Qt::Uninitialized);
    const quint32* src = reinterpret_cast<const quint32*>(rawPayload.constData());
    quint32* dst = reinterpret_cast<quint32*>(swapped.data());
    for (int i = 0; i < numFloats; ++i)
        dst[i] = qFromLittleEndian(src[i]);

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

} // namespace AetherSDR
