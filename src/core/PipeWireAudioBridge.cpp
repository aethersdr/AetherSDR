#include "PipeWireAudioBridge.h"
#include "LogManager.h"

#ifdef HAVE_PIPEWIRE_NATIVE
#include "PipeWireNativeRxSource.h"
#endif

#include <QTimer>
#include <QProcess>
#include <QDateTime>
#include <QDir>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace AetherSDR {

namespace {

// Resolve the per-user DAX FIFO directory.  Creates it 0700 if missing.
// Returns empty QString on irrecoverable failure (caller logs and bails).
// Path precedence: $XDG_RUNTIME_DIR → /run/user/<uid>/ → /tmp/aethersdr-<uid>/.
// See GHSA-x8xf-4g5v-ppf9 — pre-fix code created world-writable FIFOs in
// /tmp/aethersdr-dax-*.pipe, exposing the operator's TX path to local-user
// audio injection (an FCC liability for licensed stations).
QString daxFifoDir()
{
    QString base = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (base.isEmpty()) {
        const QString runUser = QStringLiteral("/run/user/%1").arg(::getuid());
        base = QDir(runUser).exists()
            ? runUser
            : QStringLiteral("/tmp/aethersdr-%1").arg(::getuid());
    }
    const QString dir = base + QStringLiteral("/aethersdr");
    if (::mkdir(dir.toUtf8().constData(), 0700) != 0 && errno != EEXIST) {
        qCWarning(lcDax) << "PipeWireAudioBridge: mkdir failed for"
                         << dir << ":" << strerror(errno);
        return {};
    }
    // Tighten perms in case the dir pre-existed with looser settings.
    ::chmod(dir.toUtf8().constData(), 0700);
    return dir;
}

// Atomic-ish owner-only FIFO creation.  Tries mkfifo(0600) first; on EEXIST,
// verifies the existing entry is our FIFO before unlinking and retrying.
// Refuses to clobber symlinks or files owned by other users — closes the
// /tmp-FIFO TOCTOU vector documented in GHSA-x8xf-4g5v-ppf9.
bool makeOwnedFifo(const QString& path)
{
    const QByteArray pathBytes = path.toUtf8();
    if (::mkfifo(pathBytes.constData(), 0600) == 0) return true;
    if (errno != EEXIST) {
        qCWarning(lcDax) << "PipeWireAudioBridge: mkfifo failed for"
                         << path << ":" << strerror(errno);
        return false;
    }
    struct stat st{};
    if (::lstat(pathBytes.constData(), &st) != 0
        || !S_ISFIFO(st.st_mode)
        || st.st_uid != ::getuid()) {
        qCWarning(lcDax) << "PipeWireAudioBridge: refusing to replace foreign file at"
                         << path;
        return false;
    }
    if (::unlink(pathBytes.constData()) != 0) {
        qCWarning(lcDax) << "PipeWireAudioBridge: unlink stale FIFO failed for"
                         << path << ":" << strerror(errno);
        return false;
    }
    if (::mkfifo(pathBytes.constData(), 0600) != 0) {
        qCWarning(lcDax) << "PipeWireAudioBridge: mkfifo (retry) failed for"
                         << path << ":" << strerror(errno);
        return false;
    }
    return true;
}

} // namespace

PipeWireAudioBridge::PipeWireAudioBridge(QObject* parent)
    : QObject(parent)
{}

PipeWireAudioBridge::~PipeWireAudioBridge()
{
    close();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

// Unload any stale aethersdr pipe modules from a previous crashed session
static void cleanupStaleModules()
{
    QProcess proc;
    proc.start("pactl", {"list", "modules", "short"});
    if (!proc.waitForFinished(3000)) return;

    for (const auto& line : proc.readAllStandardOutput().split('\n')) {
        if (line.contains("aethersdr-")) {
            auto parts = line.split('\t');
            if (parts.size() >= 1) {
                QProcess::execute("pactl", {"unload-module", parts[0].trimmed()});
            }
        }
    }
}

bool PipeWireAudioBridge::open()
{
    if (m_open) return true;

    cleanupStaleModules();

    // Create 4 RX sources (radio → apps).  When libpipewire-0.3 is available
    // at build time, prefer native pw_stream sources — those let us set
    // PW_KEY_NODE_LATENCY directly and avoid the kernel FIFO entirely, which
    // is the path to <100 ms WSJT-X DT.  Fall back per-channel to the legacy
    // module-pipe-source FIFO if the native open fails (e.g. PipeWire not
    // running, only PulseAudio).
    for (int i = 0; i < NUM_CHANNELS; ++i) {
#ifdef HAVE_PIPEWIRE_NATIVE
        auto native = std::make_unique<PipeWireNativeRxSource>(i + 1);
        if (native->open()) {
            m_nativeRx[i] = std::move(native);
            continue;
        }
        qCInfo(lcDax) << "PipeWireAudioBridge: native pw_stream unavailable for ch"
                      << (i + 1) << "— falling back to module-pipe-source";
#endif
        if (!loadPipeSource(i)) {
            qCWarning(lcDax) << "PipeWireAudioBridge: failed to create RX pipe" << (i + 1);
            close();
            return false;
        }
    }

    // Create TX pipe sink (apps → radio)
    if (!loadPipeSink()) {
        qCWarning(lcDax) << "PipeWireAudioBridge: failed to create TX pipe";
        close();
        return false;
    }

    // Poll TX pipe for incoming audio from apps
    m_txReadTimer = new QTimer(this);
    m_txReadTimer->setInterval(5);
    m_txReadTimer->setTimerType(Qt::PreciseTimer);
    connect(m_txReadTimer, &QTimer::timeout, this, &PipeWireAudioBridge::readTxPipe);
    m_txReadTimer->start();

    m_open.store(true, std::memory_order_release);
    qCInfo(lcDax) << "PipeWireAudioBridge: opened — 4 RX sources + 1 TX sink";
    return true;
}

void PipeWireAudioBridge::close()
{
    // Tell the audio fast path we're shutting down before we tear down
    // the native sources it might still be writing into.
    m_open.store(false, std::memory_order_release);

    if (m_silenceTimer) {
        m_silenceTimer->stop();
        delete m_silenceTimer;
        m_silenceTimer = nullptr;
    }
    m_transmitting.store(false, std::memory_order_release);

    if (m_txReadTimer) {
        m_txReadTimer->stop();
        delete m_txReadTimer;
        m_txReadTimer = nullptr;
    }

#ifdef HAVE_PIPEWIRE_NATIVE
    // Tear down native pw_stream sources (drops their context refcount).
    for (auto& src : m_nativeRx) {
        src.reset();
    }
#endif

    // Close pipe file descriptors
    for (auto& rx : m_rx) {
        if (rx.fd >= 0) { ::close(rx.fd); rx.fd = -1; }
    }
    if (m_tx.fd >= 0) { ::close(m_tx.fd); m_tx.fd = -1; }

    unloadModules();

    // Remove pipe files
    for (auto& rx : m_rx) {
        if (!rx.pipePath.isEmpty()) {
            ::unlink(rx.pipePath.toUtf8().constData());
            rx.pipePath.clear();
        }
    }
    if (!m_tx.pipePath.isEmpty()) {
        ::unlink(m_tx.pipePath.toUtf8().constData());
        m_tx.pipePath.clear();
    }

    qCInfo(lcDax) << "PipeWireAudioBridge: closed";
}

// ── Module loading via pactl ─────────────────────────────────────────────────

static uint32_t runPactl(const QStringList& args)
{
    QProcess proc;
    proc.start("pactl", args);
    if (!proc.waitForFinished(5000)) {
        qCWarning(lcDax) << "PipeWireAudioBridge: pactl timed out:" << args;
        return 0;
    }
    if (proc.exitCode() != 0) {
        qCWarning(lcDax) << "PipeWireAudioBridge: pactl failed:" << proc.readAllStandardError().trimmed();
        return 0;
    }
    // pactl load-module returns the module index
    bool ok = false;
    uint32_t idx = proc.readAllStandardOutput().trimmed().toUInt(&ok);
    return ok ? idx : 0;
}

bool PipeWireAudioBridge::loadPipeSource(int index)
{
    // FIFO lives in the per-user XDG runtime directory with mode 0600.
    // Pre-fix code used /tmp/aethersdr-dax-%1.pipe with mode 0666, which
    // any local user could read (operator RX audio) or write (operator
    // TX audio with the licensed callsign — an FCC liability).
    // See GHSA-x8xf-4g5v-ppf9.
    const QString fifoDir = daxFifoDir();
    if (fifoDir.isEmpty()) return false;
    auto pipePath = QStringLiteral("%1/dax-%2.pipe").arg(fifoDir).arg(index + 1);
    auto sourceName = QStringLiteral("aethersdr-dax-%1").arg(index + 1);
    auto sourceDesc = QStringLiteral("AetherSDR DAX %1").arg(index + 1);

    if (!makeOwnedFifo(pipePath)) return false;

    // Load PulseAudio pipe-source module.
    // Format/rate match the PipeWire graph (48 kHz float32) so PipeWire does
    // not insert a resampler on this node — that resampler is the dominant
    // remaining latency source after pipe_size was reduced (see issue #1008).
    // node.latency=256/48000 (~5.3 ms quantum) tells PipeWire to negotiate a
    // small quantum for this node instead of falling back to clock.quantum-limit.
    uint32_t modIdx = runPactl({
        "load-module", "module-pipe-source",
        QStringLiteral("file=%1").arg(pipePath),
        QStringLiteral("source_name=%1").arg(sourceName),
        QStringLiteral("source_properties=device.description=\"%1\" node.latency=256/48000").arg(sourceDesc),
        QStringLiteral("format=float32le"),
        QStringLiteral("rate=%1").arg(PIPE_RATE),
        QStringLiteral("channels=%1").arg(PIPE_CHANNELS),
    });

    if (modIdx == 0) {
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    // Open the pipe for writing (non-blocking to avoid hanging if no reader)
    int fd = ::open(pipePath.toUtf8().constData(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        qCWarning(lcDax) << "PipeWireAudioBridge: open pipe failed:" << strerror(errno);
        runPactl({"unload-module", QString::number(modIdx)});
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    // Cap kernel FIFO capacity so back-pressure surfaces quickly instead of
    // accumulating ~1.3 s of audio in the default 64 KB pipe buffer.
    if (::fcntl(fd, F_SETPIPE_SZ, PIPE_KERNEL_BUF) < 0) {
        qCDebug(lcDax) << "PipeWireAudioBridge: F_SETPIPE_SZ failed (non-fatal):" << strerror(errno);
    }

    m_rx[index].fd = fd;
    m_rx[index].moduleIndex = modIdx;
    m_rx[index].pipePath = pipePath;

    qCDebug(lcDax) << "PipeWireAudioBridge: RX" << (index + 1) << "pipe source loaded, module" << modIdx;
    return true;
}

bool PipeWireAudioBridge::loadPipeSink()
{
    // FIFO lives in the per-user XDG runtime directory with mode 0600.
    // See GHSA-x8xf-4g5v-ppf9.
    const QString fifoDir = daxFifoDir();
    if (fifoDir.isEmpty()) return false;
    auto pipePath = QStringLiteral("%1/tx.pipe").arg(fifoDir);
    auto sinkName = QStringLiteral("aethersdr-tx");
    auto sinkDesc = QStringLiteral("AetherSDR TX");

    if (!makeOwnedFifo(pipePath)) return false;

    // Use a small pipe buffer (2048 bytes ~ 42ms at 24kHz mono s16le)
    // to keep latency low for digital modes like FT8/FT4.
    uint32_t modIdx = runPactl({
        "load-module", "module-pipe-sink",
        QStringLiteral("file=%1").arg(pipePath),
        QStringLiteral("sink_name=%1").arg(sinkName),
        QStringLiteral("sink_properties=device.description=\"%1\"").arg(sinkDesc),
        QStringLiteral("format=s16le"),
        QStringLiteral("rate=%1").arg(TX_RATE),
        QStringLiteral("channels=%1").arg(TX_CHANNELS),
        QStringLiteral("pipe_size=2048"),
    });

    if (modIdx == 0) {
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    // Open the pipe for reading (non-blocking)
    int fd = ::open(pipePath.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        qCWarning(lcDax) << "PipeWireAudioBridge: open TX pipe failed:" << strerror(errno);
        runPactl({"unload-module", QString::number(modIdx)});
        ::unlink(pipePath.toUtf8().constData());
        return false;
    }

    m_tx.fd = fd;
    m_tx.moduleIndex = modIdx;
    m_tx.pipePath = pipePath;

    qCDebug(lcDax) << "PipeWireAudioBridge: TX pipe sink loaded, module" << modIdx;
    return true;
}

void PipeWireAudioBridge::unloadModules()
{
    for (auto& rx : m_rx) {
        if (rx.moduleIndex > 0) {
            runPactl({"unload-module", QString::number(rx.moduleIndex)});
            rx.moduleIndex = 0;
        }
    }
    if (m_tx.moduleIndex > 0) {
        runPactl({"unload-module", QString::number(m_tx.moduleIndex)});
        m_tx.moduleIndex = 0;
    }
}

// ── Audio I/O ────────────────────────────────────────────────────────────────

void PipeWireAudioBridge::setGain(float g)
{
    m_gain = std::clamp(g, 0.0f, 1.0f);
    for (int i = 0; i < NUM_CHANNELS; ++i)
        m_channelGain[i].store(m_gain, std::memory_order_relaxed);
}

void PipeWireAudioBridge::setChannelGain(int channel, float g)
{
    if (channel >= 1 && channel <= NUM_CHANNELS)
        m_channelGain[channel - 1].store(std::clamp(g, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PipeWireAudioBridge::setTxGain(float g)
{
    m_txGain = std::clamp(g, 0.0f, 1.0f);
}

void PipeWireAudioBridge::feedDaxAudio(int channel, const QByteArray& pcm)
{
    // This slot may run on PanadapterStream's network thread via
    // Qt::DirectConnection — keep it lock-free.  All Qt-thread-affine work
    // (timer stop, etc.) is deferred to the main-thread silence timer
    // below, which observes m_lastAudioMs.
    if (!m_open.load(std::memory_order_acquire)) return;
    if (channel < 1 || channel > NUM_CHANNELS) return;

    // Note "real audio just arrived" so the main-thread silence timer can
    // stop itself on its next tick.  No locks, no Qt API touched here.
    m_lastAudioMs.store(QDateTime::currentMSecsSinceEpoch(),
                        std::memory_order_relaxed);

    // Input is 24 kHz stereo float32 from the radio.  Output is 48 kHz mono
    // float32 — matches PipeWire graph rate so no in-graph resampler runs.
    // Linear-interpolate between input samples for the off-grid output:
    //   in:   x[0]   x[1]   x[2]   ...
    //   out:  m[0]a  x[0]   m[1]a  x[1]   m[2]a  x[2] ...
    //   m[n]a = (prev + x[n]) / 2
    // prev is held across packets to avoid a click at packet boundaries.
    const auto* src = reinterpret_cast<const float*>(pcm.constData());
    const int stereoFloats = pcm.size() / static_cast<int>(sizeof(float));
    const int monoSamplesIn = stereoFloats / 2;
    if (monoSamplesIn <= 0) return;

    const float chGain = m_channelGain[channel - 1].load(std::memory_order_relaxed);
    const int outSamples = monoSamplesIn * 2;
    QByteArray out(outSamples * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(out.data());

    float prev = m_rxLastSample[channel - 1];
    for (int i = 0; i < monoSamplesIn; ++i) {
        const float cur = (src[i * 2] + src[i * 2 + 1]) * 0.5f * chGain;
        dst[i * 2]     = (prev + cur) * 0.5f;  // interpolated midpoint
        dst[i * 2 + 1] = cur;                  // on-grid input sample
        prev = cur;
    }
    m_rxLastSample[channel - 1] = prev;

#ifdef HAVE_PIPEWIRE_NATIVE
    if (m_nativeRx[channel - 1]) {
        m_nativeRx[channel - 1]->feedAudio(dst, static_cast<uint32_t>(outSamples));
    } else
#endif
    {
        auto& rx = m_rx[channel - 1];
        if (rx.fd < 0) return;
        ::write(rx.fd, out.constData(), out.size());
    }

    // Calculate RMS for meter display (every ~100ms = ~10 packets at 24kHz)
    static int meterCount[NUM_CHANNELS]{};
    if (++meterCount[channel - 1] % 10 == 0) {
        float sum = 0;
        for (int i = 0; i < outSamples; ++i) {
            sum += dst[i] * dst[i];
        }
        const float rms = std::sqrt(sum / std::max(1, outSamples));
        emit daxRxLevel(channel, rms);
    }
}

void PipeWireAudioBridge::setTransmitting(bool tx)
{
    m_transmitting.store(tx, std::memory_order_release);

    if (tx) {
        // Start a timer that feeds silence into all RX pipes so the
        // module-pipe-source clock keeps advancing during TX.  Without this,
        // the pipe underruns and WSJT-X sees a timing gap equal to the TX
        // duration, causing cumulative DT drift.
        if (!m_silenceTimer) {
            m_silenceTimer = new QTimer(this);
            m_silenceTimer->setInterval(20);
            m_silenceTimer->setTimerType(Qt::PreciseTimer);
            connect(m_silenceTimer, &QTimer::timeout,
                    this, &PipeWireAudioBridge::feedSilenceToAllPipes);
        }
        m_silenceElapsed.start();
        m_silenceTimer->start();
    }
    // NOTE: silence timer is stopped in feedDaxAudio() when real RX audio
    // arrives, not here — the radio hasn't resumed DAX RX audio yet at
    // this point (interlock is still transitioning).
}

void PipeWireAudioBridge::feedSilenceToAllPipes()
{
    if (!m_open.load(std::memory_order_acquire)) return;

    // Self-stop: if real DAX audio has arrived in the last 50 ms, the radio
    // has resumed RX — drop the silence fill.  This used to live in
    // feedDaxAudio() but had to move here so the audio fast path can run on
    // any thread without touching QTimer (QTimer is thread-affine).
    const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastMs = m_lastAudioMs.load(std::memory_order_relaxed);
    if (lastMs != 0 && (nowMs - lastMs) < 50) {
        m_silenceTimer->stop();
        m_transmitting.store(false, std::memory_order_release);
        return;
    }

    // Compute the exact number of mono float32 silence samples based on
    // elapsed wall-clock time.  This avoids cumulative drift from QTimer
    // jitter (same approach as the macOS VirtualAudioBridge fix).
    const qint64 elapsedNs = m_silenceElapsed.nsecsElapsed();
    m_silenceElapsed.start();

    // Pipe runs at PIPE_RATE (48 kHz) mono float32.
    const int monoSamples = static_cast<int>(
        elapsedNs * PIPE_RATE / 1000000000LL);
    if (monoSamples <= 0) return;

    // Write silence (zero float32 samples) to each active RX path.
    QByteArray silence(monoSamples * static_cast<int>(sizeof(float)), '\0');
    const auto* silenceFloat = reinterpret_cast<const float*>(silence.constData());
    for (int i = 0; i < NUM_CHANNELS; ++i) {
#ifdef HAVE_PIPEWIRE_NATIVE
        if (m_nativeRx[i]) {
            m_nativeRx[i]->feedAudio(silenceFloat, static_cast<uint32_t>(monoSamples));
        } else
#endif
        if (m_rx[i].fd >= 0) {
            ::write(m_rx[i].fd, silence.constData(), silence.size());
        }
        // Reset upsample state to silence so the first real packet after TX
        // doesn't interpolate from a stale audio sample.
        m_rxLastSample[i] = 0.0f;
    }
}

void PipeWireAudioBridge::readTxPipe()
{
    if (m_tx.fd < 0) return;

    // Drain all available data from the TX pipe (int16 mono from apps).
    // Reading in a loop avoids bufferbloat when the timer fires late.
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(m_tx.fd, buf, sizeof(buf));
        if (n <= 0) break;

        // Convert int16 mono → float32 stereo with TX gain
        int monoSamples = n / sizeof(int16_t);
        const auto* src = reinterpret_cast<const int16_t*>(buf);
        QByteArray out(monoSamples * 2 * sizeof(float), Qt::Uninitialized);  // stereo
        auto* dst = reinterpret_cast<float*>(out.data());
        for (int i = 0; i < monoSamples; ++i) {
            float v = (src[i] / 32768.0f) * m_txGain;
            dst[i * 2]     = v;  // left
            dst[i * 2 + 1] = v;  // right (duplicate)
        }

        emit txAudioReady(out);

        // TX level meter (every ~100ms)
        static int txMeterCount = 0;
        if (++txMeterCount % 10 == 0) {
            float sum = 0;
            for (int i = 0; i < monoSamples; ++i)
                sum += (src[i] / 32768.0f) * (src[i] / 32768.0f);
            emit daxTxLevel(std::sqrt(sum / std::max(1, monoSamples)));
        }
    }
}

} // namespace AetherSDR
