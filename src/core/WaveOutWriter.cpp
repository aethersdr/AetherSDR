#include "core/WaveOutWriter.h"
#include "core/LogManager.h"
#include <QMediaDevices>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

// ---------------------------------------------------------------------------
// WfmRingBuffer
// ---------------------------------------------------------------------------

void WfmRingBuffer::write(const char* data, int len)
{
    QMutexLocker lk(&m_mutex);
    const int space = kCapacity - m_available;
    if (len > space) len = space;          // drop oldest-would-be-written on overflow
    for (int i = 0; i < len; ++i) {
        m_buf[m_writePos] = data[i];
        m_writePos = (m_writePos + 1) % kCapacity;
    }
    m_available += len;
}

int WfmRingBuffer::read(char* out, int maxLen)
{
    QMutexLocker lk(&m_mutex);
    const int n = std::min(maxLen, m_available);
    for (int i = 0; i < n; ++i) {
        out[i] = m_buf[m_readPos];
        m_readPos = (m_readPos + 1) % kCapacity;
    }
    m_available -= n;
    return n;
}

int WfmRingBuffer::available() const
{
    QMutexLocker lk(&m_mutex);
    return m_available;
}

// ---------------------------------------------------------------------------
// WaveOutWriter
// ---------------------------------------------------------------------------

WaveOutWriter::WaveOutWriter(QObject* parent)
    : QObject(parent)
{}

WaveOutWriter::~WaveOutWriter()
{
    close();
}

bool WaveOutWriter::open(const QString& deviceId, int sampleRate, int channelCount)
{
    close();

    QAudioDevice found;
    const auto outputs = QMediaDevices::audioOutputs();
    qCDebug(lcAudio) << "WaveOutWriter::open looking for" << deviceId
                     << "among" << outputs.size() << "devices";
    for (const QAudioDevice& dev : outputs) {
        if (dev.id() == deviceId.toUtf8()) {
            found = dev;
            break;
        }
    }
    if (found.isNull()) {
        qCDebug(lcAudio) << "WaveOutWriter::open: device not found —" << deviceId;
        return false;
    }

    // Prefer Float32 — universally supported by all Qt audio backends and
    // virtual audio cables. Fall back to the device's preferred format only
    // if Float32 is somehow not available.
    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(channelCount);
    fmt.setSampleFormat(QAudioFormat::Float);

    if (!found.isFormatSupported(fmt)) {
        qCDebug(lcAudio) << "WaveOutWriter::open: Float32 not supported by"
                         << found.description() << "— falling back to preferred format";
        fmt = found.preferredFormat();
    }

    m_sink = new QAudioSink(found, fmt, this);

    // Push mode: start() returns the QIODevice we write into.
    m_io = m_sink->start();

    if (!m_io || m_sink->error() != QAudio::NoError) {
        qCDebug(lcAudio) << "WaveOutWriter::open: QAudioSink::start() failed, error="
                         << m_sink->error();
        delete m_sink;
        m_sink = nullptr;
        m_io   = nullptr;
        return false;
    }

    // Drive writes at 10 ms intervals: each tick asks the sink how many bytes
    // it can accept (bytesFree) and serves exactly that many from the ring
    // buffer.  This lets the audio driver control the effective data rate
    // without blocking or flooding the sink.
    m_timer = new QTimer(this);
    m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &WaveOutWriter::pushData);
    m_timer->start();

    m_deviceName = found.description();
    qCDebug(lcAudio) << "WaveOutWriter opened (timer-push mode):" << m_deviceName
                     << "rate=" << fmt.sampleRate()
                     << "ch="   << fmt.channelCount()
                     << "fmt="  << fmt.sampleFormat();
    return true;
}

void WaveOutWriter::close()
{
    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
        m_io   = nullptr;
    }
    m_deviceName.clear();
    qCDebug(lcAudio) << "WaveOutWriter closed";
}

void WaveOutWriter::write(const QByteArray& pcm)
{
    if (pcm.isEmpty()) return;
    m_ring.write(pcm.constData(), pcm.size());
}

void WaveOutWriter::pushData()
{
    if (!m_io || !m_sink) return;
    const int free = static_cast<int>(m_sink->bytesFree());
    if (free <= 0) return;

    QByteArray buf(free, 0);
    m_ring.read(buf.data(), free);
    // Always push `free` bytes, zero-padded when the ring runs dry — the
    // same underflow policy as SkyRoof's RingBuffer.ReadBytes. A virtual
    // audio cable must never starve: HiFi Cable / VB-Cable loop their
    // internal buffer when the writer stalls mid-stream, which a soundmodem
    // sees as loud repeating garbage instead of silence.
    m_io->write(buf.constData(), free);
}

} // namespace AetherSDR
