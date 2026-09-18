#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QVector>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR {

// RFC #5468 A1. These are PRODUCER formats, independent of the audio device
// and of fixed-rate decoder domains. No 48 kHz producer is enabled by A1.
enum class PcmLayout { Mono, Stereo };
enum class PcmPurpose { Speaker, Slice, Auxiliary };

struct PcmFormat {
    int sampleRateHz = 24000;
    PcmLayout layout = PcmLayout::Stereo;
    int channels() const { return layout == PcmLayout::Mono ? 1 : 2; }
    bool valid() const
    {
        return (sampleRateHz == 24000 || sampleRateHz == 48000)
            && (layout == PcmLayout::Mono || layout == PcmLayout::Stereo);
    }
    bool operator==(const PcmFormat&) const = default;
};

struct PcmStreamDescriptor {
    quint64 source = 0;             // process-local producer identity
    quint64 session = 0;            // connection, distinct from source/slot
    quint64 formatGeneration = 0;   // changes even on a 24 -> 48 -> 24 cycle
    quint64 receiverInstance = 0;   // F4 Handle::instance, never the slot
    int sliceId = -1;               // F4 Handle::slot, or -1 for a mix
    PcmPurpose purpose = PcmPurpose::Speaker;
    PcmFormat format;
    bool operator==(const PcmStreamDescriptor&) const = default;
};

namespace detail {
// Revocation is the only mutable field. It lets a queued frame fail closed
// after stop/reconnect/removal, even before a consumer's event loop runs a
// reset. This token owns no radio, DSP, QObject or callback resources.
struct PcmEpoch {
    const PcmStreamDescriptor descriptor;
    std::atomic<bool> active{true};
    explicit PcmEpoch(PcmStreamDescriptor value) : descriptor(value) {}
};
} // namespace detail

class PcmProducer;
class PcmFrameGate;

// Owning native-endian, interleaved IEEE float32 PCM. Metadata and samples are
// immutable to consumers and copied together by Qt queued delivery. Neither a
// borrowed callback buffer nor a mutable global rate can reinterpret a frame.
class PcmFrame final {
public:
    static constexpr qsizetype kMaxFrames = 65536;
    PcmFrame() = default; // invalid, suitable for QVariant/queued metatypes
    const PcmStreamDescriptor& stream() const
    {
        static const PcmStreamDescriptor empty;
        return m_epoch ? m_epoch->descriptor : empty;
    }
    const QVector<float>& samples() const { return m_samples; }
    qsizetype frameCount() const { return m_samples.size() / stream().format.channels(); }
    quint64 firstSample() const { return m_firstSample; }
    bool discontinuity() const { return m_discontinuity; }
    bool current() const
    {
        return m_epoch && m_epoch->active.load(std::memory_order_acquire);
    }

    // Compatibility boundary ONLY. Refuse formats the existing 24 kHz stereo
    // consumers cannot interpret; never resample, downmix, clip or relabel.
    // A2-A5 replace these boundaries as their consumers become rate-aware.
    QByteArray legacyStereo24() const
    {
        if (!current() || stream().format != PcmFormat{}) {
            return {};
        }
        return QByteArray(reinterpret_cast<const char*>(m_samples.constData()),
                         m_samples.size() * static_cast<qsizetype>(sizeof(float)));
    }

private:
    friend class PcmProducer;
    friend class PcmFrameGate;
    std::shared_ptr<const detail::PcmEpoch> m_epoch;
    QVector<float> m_samples;
    quint64 m_firstSample = 0;
    bool m_discontinuity = false;
};

// One execution context produces a stream. start()/stop()/format changes run
// on that same context. invalidate() may overlap produce()/queued delivery,
// but must not overlap start(), setFormat() or destruction. The caller must
// join the producer before destroying it. Only the tiny epoch token survives.
class PcmProducer final {
public:
    PcmProducer() : m_source(s_nextSource.fetch_add(1, std::memory_order_relaxed)) {}
    ~PcmProducer() { invalidate(); }
    PcmProducer(const PcmProducer&) = delete;
    PcmProducer& operator=(const PcmProducer&) = delete;

    // Explicit F4 session/instance values may be supplied. Legacy adapters use
    // local monotonic sessions and the unique producer id as receiver instance.
    bool start(PcmPurpose purpose = PcmPurpose::Speaker, int sliceId = -1,
               PcmFormat format = {}, quint64 session = 0,
               quint64 receiverInstance = 0)
    {
        if (!format.valid() || m_source == 0 || (session != 0 && session <= m_session)
            || (purpose != PcmPurpose::Speaker && purpose != PcmPurpose::Slice
                && purpose != PcmPurpose::Auxiliary)
            || (purpose == PcmPurpose::Slice ? sliceId < 0 : sliceId != -1)
            || m_session == std::numeric_limits<quint64>::max()) {
            return false;
        }
        invalidate();
        m_session = session ? session : m_session + 1;
        m_generation = 1;
        const PcmStreamDescriptor descriptor{m_source, m_session, m_generation,
            purpose == PcmPurpose::Slice ? (receiverInstance ? receiverInstance : m_source) : 0,
            sliceId, purpose, format};
        m_epoch = std::make_shared<detail::PcmEpoch>(descriptor);
        m_nextSample = 0;
        m_first = true;
        return true;
    }

    bool setFormat(PcmFormat format)
    {
        if (!m_epoch || !m_epoch->active.load(std::memory_order_acquire)
            || !format.valid() || m_generation == std::numeric_limits<quint64>::max()) {
            return false;
        }
        if (format == m_epoch->descriptor.format) {
            return true;
        }
        PcmStreamDescriptor descriptor = m_epoch->descriptor;
        descriptor.format = format;
        descriptor.formatGeneration = ++m_generation;
        invalidate();
        m_epoch = std::make_shared<detail::PcmEpoch>(descriptor);
        m_nextSample = 0;
        m_first = true;
        return true;
    }

    void invalidate() const
    {
        if (m_epoch) {
            m_epoch->active.store(false, std::memory_order_release);
        }
    }

    std::optional<PcmFrame> produce(QVector<float> samples,
                                    std::optional<quint64> firstSample = std::nullopt,
                                    bool discontinuity = false)
    {
        if (!m_epoch || !m_epoch->active.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const int channels = m_epoch->descriptor.format.channels();
        if (samples.isEmpty() || samples.size() % channels != 0
            || samples.size() / channels > PcmFrame::kMaxFrames) {
            return std::nullopt;
        }
        const quint64 count = static_cast<quint64>(samples.size() / channels);
        const quint64 position = firstSample.value_or(m_nextSample);
        if (position < m_nextSample || (position != m_nextSample && !discontinuity)
            || position > std::numeric_limits<quint64>::max() - count) {
            return std::nullopt;
        }
        // Detach explicitly: Qt containers can also wrap read-only external
        // storage. A queued frame must own that storage independently.
        samples.detach();
        for (float sample : std::as_const(samples)) {
            if (!std::isfinite(sample)) {
                return std::nullopt;
            }
        }
        PcmFrame frame;
        frame.m_epoch = m_epoch;
        frame.m_samples = std::move(samples);
        frame.m_firstSample = position;
        frame.m_discontinuity = m_first || discontinuity;
        m_nextSample = position + count;
        m_first = false;
        return frame;
    }

    // Existing producers all publish 24 kHz stereo. Copy into typed, owned
    // storage before queueing; QByteArray::fromRawData must not escape borrowed.
    std::optional<PcmFrame> legacyStereo24(const QByteArray& pcm)
    {
        constexpr qsizetype kFrameBytes = 2 * sizeof(float);
        if (!m_epoch || m_epoch->descriptor.format != PcmFormat{}
            || pcm.isEmpty() || pcm.size() % kFrameBytes != 0
            || pcm.size() / kFrameBytes > PcmFrame::kMaxFrames) {
            return std::nullopt;
        }
        QVector<float> samples(pcm.size() / static_cast<qsizetype>(sizeof(float)));
        std::memcpy(samples.data(), pcm.constData(), static_cast<std::size_t>(pcm.size()));
        return produce(std::move(samples));
    }

private:
    inline static std::atomic<quint64> s_nextSource{1};
    const quint64 m_source;
    quint64 m_session = 0;
    quint64 m_generation = 0;
    std::shared_ptr<detail::PcmEpoch> m_epoch;
    quint64 m_nextSample = 0;
    bool m_first = true;
};

// Receiver-side bounded replay guard. Separate consumers have separate cursors;
// reading a slice tap never consumes the speaker's frame. New streams are
// admitted only with a live producer token; traffic cannot resurrect one.
//
// The guard is deliberately ONE-SIDED: it refuses a frame at or behind the
// cursor (replay, duplicate, reorder) and admits one ahead of it. A forward gap
// is not an attack, it is "this consumer missed frames" — which every consumer
// that can be detached from a running producer does legitimately. Playback mute
// is the live example: MainWindow disconnects the Flex speaker feed, and
// MainWindow_Session returns early for a seam backend, while the producer keeps
// counting. Refusing the gap would leave that consumer's cursor permanently
// behind a live epoch, and since the cursor only advances on an accepted frame
// there is no way back — RX audio would stay silent until the next reconnect.
class PcmFrameGate final {
public:
    static constexpr std::size_t kMaxStreams = 32;
    bool accept(const PcmFrame& frame)
    {
        if (!frame.current()) {
            return false;
        }
        Cursor* available = nullptr;
        for (Cursor& cursor : m_cursors) {
            const std::shared_ptr<const detail::PcmEpoch> epoch = cursor.epoch.lock();
            if (epoch == frame.m_epoch) {
                if (frame.firstSample() < cursor.nextSample) {
                    return false;
                }
                cursor.nextSample = frame.firstSample() + frame.frameCount();
                return true;
            }
            if (!epoch || !epoch->active.load(std::memory_order_acquire)) {
                available = &cursor;
            }
        }
        if (!available) {
            return false;
        }
        available->epoch = frame.m_epoch;
        available->nextSample = frame.firstSample() + frame.frameCount();
        return true;
    }
private:
    struct Cursor {
        std::weak_ptr<const detail::PcmEpoch> epoch;
        quint64 nextSample = 0;
    };
    std::array<Cursor, kMaxStreams> m_cursors;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::PcmFrame)
