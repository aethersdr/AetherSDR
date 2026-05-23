#include "PipeWireNativeRxSource.h"
#include "PipeWireNativeContext.h"
#include "LogManager.h"

#include <pipewire/pipewire.h>
#include <pipewire/keys.h>
#include <pipewire/stream.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <QString>
#include <QByteArray>

#include <algorithm>
#include <cstring>

namespace MasterSDR {

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels   = 1;

// Trampoline so we can register a callback whose signature exactly matches
// libpipewire's pw_stream_events::state_changed (which uses enum pw_stream_state)
// without UB.  PipeWireNativeRxSource::onStateChanged keeps an int signature so
// the header can stay free of <pipewire/stream.h>.
void onStateChangedTrampoline(void* userdata,
                              enum pw_stream_state old_state,
                              enum pw_stream_state state,
                              const char* error)
{
    PipeWireNativeRxSource::onStateChanged(
        userdata,
        static_cast<int>(old_state),
        static_cast<int>(state),
        error);
}

const pw_stream_events kStreamEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = &onStateChangedTrampoline,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = nullptr,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = &PipeWireNativeRxSource::onProcess,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

} // namespace

PipeWireNativeRxSource::PipeWireNativeRxSource(int channel)
    : m_channel(channel)
{}

PipeWireNativeRxSource::~PipeWireNativeRxSource()
{
    close();
}

bool PipeWireNativeRxSource::open()
{
    if (m_stream) {
        return true;
    }

    auto& ctx = PipeWireNativeContext::instance();
    if (!ctx.acquire()) {
        qCWarning(lcDax) << "PipeWireNativeRxSource: failed to acquire context for ch" << m_channel;
        return false;
    }
    m_contextAcquired = true;

    const QByteArray nodeName = QString("MasterSDR-dax-%1").arg(m_channel).toUtf8();
    const QByteArray nodeDesc = QString("MasterSDR DAX %1").arg(m_channel).toUtf8();

    // Latency strategy:
     //   node.latency       — the *request*: 256-sample quantum (~5.3 ms @ 48 kHz)
     //   node.force-quantum — *forces* the graph cycle to 256 samples while our
     //                         node is active.  Without this, any other client
     //                         (e.g. WSJT-X's Qt PulseAudio backend) requesting
     //                         a longer buffer drags the negotiated graph
     //                         quantum up, undoing our latency hint.
     //   node.force-rate    — pin graph rate to 48 kHz so it can't fall back to
     //                         a slower clock during negotiation.
     //   node.always-process— keep draining the ring even when no client is
     //                         connected, so we don't accumulate backlog
     //                         between "DAX enabled" and "WSJT-X connected".
     //   pulse.min.{req,frag,quantum}
     //                      — clamp the PipeWire pulse-compat fragment pool
     //                         that PulseAudio-API clients sit behind.  This
     //                         is the hidden ~200 ms buffer Qt's PulseAudio
     //                         backend negotiates by default (4–8 fragments
     //                         × 50 ms each).  PipeWire's pulse module reads
     //                         these source-side properties when sizing each
     //                         capturing pulse client's ring, so any client
     //                         (WSJT-X, fldigi, …) connecting to us inherits
     //                         the small 256-sample fragment cap regardless
     //                         of what its own backend requested.  Confirmed
     //                         via pw-cat (native PipeWire client at 5.3 ms)
     //                         vs QtPulseAudio:<pid> (50 ms quantum + 200 ms
     //                         pulse fragment buffer).
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,           "Audio",
        PW_KEY_MEDIA_CATEGORY,       "Playback",  // we play audio INTO the graph; clients capture from us
        PW_KEY_MEDIA_CLASS,          "Audio/Source",
        PW_KEY_MEDIA_ROLE,           "Production",
        PW_KEY_NODE_NAME,            nodeName.constData(),
        PW_KEY_NODE_DESCRIPTION,     nodeDesc.constData(),
        PW_KEY_NODE_LATENCY,         "256/48000",
        PW_KEY_NODE_RATE,            "1/48000",
        PW_KEY_NODE_FORCE_QUANTUM,   "256",
        PW_KEY_NODE_FORCE_RATE,      "48000",
        PW_KEY_NODE_ALWAYS_PROCESS,  "true",
        "pulse.min.req",             "256/48000",
        "pulse.default.req",         "256/48000",
        "pulse.min.frag",            "256/48000",
        "pulse.default.frag",        "256/48000",
        "pulse.min.quantum",         "256/48000",
        "pulse.default.quantum",     "256/48000",
        nullptr);

    ctx.lock();
    m_stream = pw_stream_new(ctx.core(), nodeName.constData(), props);
    if (!m_stream) {
        ctx.unlock();
        qCWarning(lcDax) << "PipeWireNativeRxSource: pw_stream_new failed for ch" << m_channel;
        ctx.release();
        m_contextAcquired = false;
        return false;
    }

    pw_stream_add_listener(m_stream, &m_listenerHook, &kStreamEvents, this);

    spa_audio_info_raw info{};
    info.format   = SPA_AUDIO_FORMAT_F32_LE;
    info.rate     = kSampleRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS);

    int rc = pw_stream_connect(
        m_stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        flags,
        params, 1);
    ctx.unlock();

    if (rc < 0) {
        qCWarning(lcDax) << "PipeWireNativeRxSource: pw_stream_connect failed for ch" << m_channel
                         << "err=" << spa_strerror(rc);
        close();
        return false;
    }

    qCInfo(lcDax) << "PipeWireNativeRxSource: ch" << m_channel
                  << "connected (48 kHz mono float32, node.latency=256/48000)";
    return true;
}

void PipeWireNativeRxSource::close()
{
    if (m_stream) {
        auto& ctx = PipeWireNativeContext::instance();
        ctx.lock();
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
        ctx.unlock();
    }
    if (m_contextAcquired) {
        PipeWireNativeContext::instance().release();
        m_contextAcquired = false;
    }
}

void PipeWireNativeRxSource::feedAudio(const float* samples, uint32_t count)
{
    if (!m_stream || count == 0) {
        return;
    }

    const uint32_t writeIdx = m_writeIdx.load(std::memory_order_relaxed);
    const uint32_t readIdx  = m_readIdx.load(std::memory_order_acquire);
    const uint32_t pending  = writeIdx - readIdx;
    const uint32_t space    = RING_SIZE - pending;

    // SPSC invariant: only the producer touches m_writeIdx, only the consumer
    // touches m_readIdx.  On overflow, drop the *newest* incoming samples that
    // wouldn't fit rather than evicting from the consumer side — evicting via
    // the producer would race the consumer's in-flight memcpy when wraparound
    // exposes a slot it had already claimed.  Latency is still capped at the
    // ring size; only the choice of which dropouts you take changes.  In
    // practice the consumer (PipeWire RT loop) only stalls under extreme
    // system load, so the steady-state behaviour is unaffected.
    const uint32_t toCopy = std::min(count, space);
    if (toCopy == 0) {
        return;
    }

    // Copy in up to two contiguous spans (handles wraparound).
    const uint32_t startMasked = writeIdx & RING_MASK;
    const uint32_t firstSpan   = std::min(toCopy, RING_SIZE - startMasked);
    std::memcpy(&m_ring[startMasked], samples, firstSpan * sizeof(float));
    if (toCopy > firstSpan) {
        std::memcpy(&m_ring[0], samples + firstSpan, (toCopy - firstSpan) * sizeof(float));
    }

    m_writeIdx.fetch_add(toCopy, std::memory_order_release);
}

void PipeWireNativeRxSource::onStateChanged(
    void* userdata, int old, int state, const char* error)
{
    auto* self = static_cast<PipeWireNativeRxSource*>(userdata);
    if (error) {
        qCWarning(lcDax) << "PipeWireNativeRxSource: ch" << self->m_channel
                         << "state error:" << error;
    }
    qCDebug(lcDax) << "PipeWireNativeRxSource: ch" << self->m_channel
                   << "state" << old << "->" << state;
}

void PipeWireNativeRxSource::onProcess(void* userdata)
{
    auto* self = static_cast<PipeWireNativeRxSource*>(userdata);
    pw_buffer* b = pw_stream_dequeue_buffer(self->m_stream);
    if (!b) {
        return;
    }

    spa_buffer* spa_buf = b->buffer;
    if (!spa_buf->datas[0].data) {
        pw_stream_queue_buffer(self->m_stream, b);
        return;
    }

    auto*    dst       = static_cast<float*>(spa_buf->datas[0].data);
    uint32_t maxFrames = spa_buf->datas[0].maxsize / sizeof(float);
    uint32_t reqFrames = (b->requested > 0) ? static_cast<uint32_t>(b->requested) : maxFrames;
    uint32_t frames    = std::min(reqFrames, maxFrames);

    const uint32_t writeIdx = self->m_writeIdx.load(std::memory_order_acquire);
    const uint32_t readIdx  = self->m_readIdx.load(std::memory_order_relaxed);
    const uint32_t pending  = writeIdx - readIdx;
    const uint32_t toRead   = std::min(frames, pending);

    if (toRead > 0) {
        const uint32_t startMasked = readIdx & RING_MASK;
        const uint32_t firstSpan   = std::min(toRead, RING_SIZE - startMasked);
        std::memcpy(dst, &self->m_ring[startMasked], firstSpan * sizeof(float));
        if (toRead > firstSpan) {
            std::memcpy(dst + firstSpan, &self->m_ring[0], (toRead - firstSpan) * sizeof(float));
        }
        self->m_readIdx.fetch_add(toRead, std::memory_order_release);
    }

    // Pad with silence if the ring is short of the requested frames.
    if (toRead < frames) {
        std::memset(dst + toRead, 0, (frames - toRead) * sizeof(float));
    }

    spa_buf->datas[0].chunk->offset = 0;
    spa_buf->datas[0].chunk->stride = sizeof(float);
    spa_buf->datas[0].chunk->size   = frames * sizeof(float);

    pw_stream_queue_buffer(self->m_stream, b);
}

} // namespace MasterSDR
