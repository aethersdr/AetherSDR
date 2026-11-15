#include "Ax25ReceiveModel.h"

#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <deque>
#include <optional>
#include <tuple>
#include <utility>

namespace AetherSDR {

struct Ax25ReceiveModel::Impl {
    struct Input {
        DecoderPcmBlock block;
        quint64 generation = 0;
    };
    struct Output {
        PcmEpochLease source;
        quint64 generation = 0;
        QVector<Ax25DecodedFrame> frames;
        std::optional<Ax25DecoderDiagnostics> diagnostics;
    };
    struct State {
        QMutex mutex;
        std::shared_ptr<std::atomic<quint64>> generation =
            std::make_shared<std::atomic<quint64>>(1);
        Ax25ReceiveModel* owner = nullptr;
        QObject* worker = nullptr;
        std::deque<Input> input;
        std::deque<Output> output;
        qsizetype frames = 0;
        bool workerScheduled = false;
        bool outputScheduled = false;
        bool resetNotification = false;
        bool diagnosticsLogging = false;
        Ax25DemodConfig config;
        quint64 configuration = 1;
        static constexpr qsizetype kMaxFrames = 65536;
        static constexpr std::size_t kMaxBlocks = 256;

        // Called under mutex. Retire BEFORE any callbacks or worker operation;
        // a result already queued to the owner must become stale immediately.
        void retire()
        {
            ++*generation;
            input.clear();
            output.clear();
            frames = 0;
        }
    };

    class Worker final : public QObject {
    public:
        explicit Worker(std::shared_ptr<State> state)
            : m_state(std::move(state)), m_shim(this)
        {
            connect(&m_shim, &AetherAx25LibmodemShim::frameDecoded, this,
                [this](const Ax25DecodedFrame& frame) {
                    if (m_active && current(*m_active)) {
                        m_active->frames.append(frame);
                    }
                });
            connect(&m_shim, &AetherAx25LibmodemShim::diagnosticsUpdated, this,
                [this](const Ax25DecoderDiagnostics& diagnostics) {
                    if (m_active && current(*m_active)) {
                        m_active->diagnostics = diagnostics;
                    }
                });
        }

        void drain()
        {
            // Yield regularly to shutdown/configuration work. The owner and
            // producer cannot create one queued event per incoming PCM block.
            for (int count = 0; count < 8; ++count) {
                Input input;
                Ax25DemodConfig config;
                quint64 generation = 0;
                quint64 configuration = 0;
                bool logging = false;
                {
                    QMutexLocker lock(&m_state->mutex);
                    if (!m_state->owner) {
                        m_state->workerScheduled = false;
                        return;
                    }
                    generation = m_state->generation->load();
                    configuration = m_state->configuration;
                    config = m_state->config;
                    logging = m_state->diagnosticsLogging;
                    if (!m_state->input.empty()) {
                        input = std::move(m_state->input.front());
                        m_state->input.pop_front();
                        m_state->frames -= input.block.samples.size();
                    }
                }
                // Configure/reset signals have no active PCM context and cannot
                // borrow a new generation for an older result.
                m_active.reset();
                if (configuration != m_configuration) {
                    m_shim.configure(config);
                    m_configuration = configuration;
                    m_generation = generation;
                } else if (generation != m_generation) {
                    m_shim.reset();
                    m_generation = generation;
                }
                m_shim.setDiagnosticsLoggingEnabled(logging);
                if (input.generation && input.generation == generation
                    && input.block.current()
                    && generation == m_state->generation->load()) {
                    // Preserve the feed's original context throughout synchronous
                    // shim callbacks, even if the owner resets concurrently.
                    m_active = Output{input.block.source, input.generation, {}, {}};
                    m_shim.feedAudio(QByteArray(
                        reinterpret_cast<const char*>(input.block.samples.constData()),
                        input.block.samples.size() * static_cast<qsizetype>(sizeof(float))),
                        DecoderPcmBlock::kSampleRateHz);
                    if (current(*m_active)
                        && (!m_active->frames.isEmpty() || m_active->diagnostics)) {
                        QMutexLocker lock(&m_state->mutex);
                        if (m_state->owner && current(*m_active)) {
                            if (m_state->output.size() == State::kMaxBlocks) {
                                m_state->retire();
                                m_state->resetNotification = true;
                            } else {
                                m_state->output.push_back(std::move(*m_active));
                            }
                            scheduleOutput(m_state);
                        }
                    }
                    m_active.reset();
                }
                {
                    QMutexLocker lock(&m_state->mutex);
                    if (m_state->input.empty()
                        && generation == m_state->generation->load()
                        && configuration == m_state->configuration
                        && logging == m_state->diagnosticsLogging) {
                        m_state->workerScheduled = false;
                        return;
                    }
                }
            }
            QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
        }

    private:
        bool current(const Output& output) const
        {
            return output.generation == m_state->generation->load()
                && output.source.current();
        }
        std::shared_ptr<State> m_state;
        AetherAx25LibmodemShim m_shim;
        quint64 m_generation = 0;
        quint64 m_configuration = 0;
        std::optional<Output> m_active;
    };

    Ax25ReceiveModel* owner;
    std::unique_ptr<DecoderAudioModel> audio;
    std::shared_ptr<State> state = std::make_shared<State>();
    QThread thread;
    bool enabled = false;

    // Caller holds State::mutex, fencing invocation against owner destruction.
    static void scheduleOutput(const std::shared_ptr<State>& state)
    {
        if (!state->owner || state->outputScheduled) {
            return;
        }
        state->outputScheduled = true;
        Ax25ReceiveModel* target = state->owner;
        QMetaObject::invokeMethod(target, [target, state] {
            target->m_impl->publish(state);
        }, Qt::QueuedConnection);
    }

    void scheduleWorker()
    {
        if (!state->worker || state->workerScheduled) {
            return;
        }
        state->workerScheduled = true;
        auto* worker = static_cast<Worker*>(state->worker);
        QMetaObject::invokeMethod(worker, [worker] { worker->drain(); }, Qt::QueuedConnection);
    }

    void retire()
    {
        {
            QMutexLocker lock(&state->mutex);
            state->retire();
            state->resetNotification = false;
            scheduleWorker();
        }
        const QPointer<Ax25ReceiveModel> guard(owner);
        emit owner->sourceReset();
        if (guard) {
            emit owner->statusChanged();
        }
    }

    void enqueue(const DecoderPcmBlock& block)
    {
        if (!enabled || !block.current()) {
            return;
        }
        const QPointer<Ax25ReceiveModel> guard(owner);
        bool overflow = false;
        quint64 generation = 0;
        {
            QMutexLocker lock(&state->mutex);
            if (block.samples.size() > State::kMaxFrames) {
                state->retire();
                overflow = true;
            } else {
                if (state->input.size() == State::kMaxBlocks
                    || state->frames + block.samples.size() > State::kMaxFrames) {
                    state->retire();
                    overflow = true;
                }
                generation = state->generation->load();
                state->input.push_back({block, generation});
                state->frames += block.samples.size();
            }
            scheduleWorker();
        }
        if (overflow) {
            emit owner->sourceReset();
            if (!guard) {
                return;
            }
        }
        if (generation && enabled && block.current()
            && generation == state->generation->load()) {
            emit owner->pcmReady(block, {block.source, generation, state->generation});
        }
    }

    void publish(const std::shared_ptr<State>& box)
    {
        std::deque<Output> output;
        bool reset = false;
        {
            QMutexLocker lock(&box->mutex);
            output.swap(box->output);
            box->outputScheduled = false;
            reset = std::exchange(box->resetNotification, false);
        }
        const QPointer<Ax25ReceiveModel> guard(owner);
        if (reset) {
            emit owner->sourceReset();
            if (!guard) {
                return;
            }
        }
        for (const Output& item : output) {
            const auto current = [&] {
                return enabled && item.source.current()
                    && item.generation == box->generation->load();
            };
            for (const Ax25DecodedFrame& frame : item.frames) {
                if (!current()) {
                    break;
                }
                emit owner->frameDecoded(frame, {item.source, item.generation, box->generation});
                if (!guard) {
                    return;
                }
            }
            if (item.diagnostics && current()) {
                emit owner->diagnosticsUpdated(*item.diagnostics,
                    {item.source, item.generation, box->generation});
                if (!guard) {
                    return;
                }
            }
        }
    }
};

Ax25ReceiveModel::Ax25ReceiveModel(RadioModel& radio, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    Impl& d = *m_impl;
    d.owner = this;
    d.state->owner = this;
    auto* worker = new Impl::Worker(d.state);
    d.state->worker = worker;
    worker->moveToThread(&d.thread);
    connect(&d.thread, &QThread::finished, worker, &QObject::deleteLater);
    d.thread.setObjectName(QStringLiteral("Ax25Receive"));
    d.thread.start();
    d.audio = std::make_unique<DecoderAudioModel>(radio, DecoderAudioModel::Consumer::Ax25);
    connect(d.audio.get(), &DecoderAudioModel::sourceReset, this, [this] { m_impl->retire(); });
    connect(d.audio.get(), &DecoderAudioModel::pcmReady, this,
        [this](const DecoderPcmBlock& block) { m_impl->enqueue(block); });
    connect(d.audio.get(), &DecoderAudioModel::routeStatusChanged,
        this, &Ax25ReceiveModel::routeStatusChanged);
}

Ax25ReceiveModel::~Ax25ReceiveModel()
{
    Impl& d = *m_impl;
    {
        QMutexLocker lock(&d.state->mutex);
        d.state->owner = nullptr;
        d.state->retire();
    }
    d.audio.reset();
    d.thread.quit();
    d.thread.wait();
}

void Ax25ReceiveModel::setSlice(SliceModel* slice)
{
    m_impl->audio->setSlice(slice);
}

void Ax25ReceiveModel::setEnabled(bool enabled)
{
    if (m_impl->enabled == enabled) {
        return;
    }
    m_impl->enabled = enabled;
    m_impl->audio->setEnabled(enabled);
}

void Ax25ReceiveModel::configure(const Ax25DemodConfig& config)
{
    {
        QMutexLocker lock(&m_impl->state->mutex);
        const Ax25DemodConfig& previous = m_impl->state->config;
        if (std::tie(previous.profile, previous.sampleRate, previous.baud,
                     previous.markHz, previous.spaceHz, previous.polarity,
                     previous.vhfMode, previous.txPreambleFlags)
            == std::tie(config.profile, config.sampleRate, config.baud,
                        config.markHz, config.spaceHz, config.polarity,
                        config.vhfMode, config.txPreambleFlags)) {
            return;
        }
        m_impl->state->config = config;
        ++m_impl->state->configuration;
        m_impl->state->retire();
        m_impl->state->resetNotification = false;
        m_impl->scheduleWorker();
    }
    const QPointer<Ax25ReceiveModel> guard(this);
    emit sourceReset();
    if (guard) {
        emit statusChanged();
    }
}

void Ax25ReceiveModel::reset()
{
    m_impl->retire();
}

void Ax25ReceiveModel::setDiagnosticsLoggingEnabled(bool enabled)
{
    QMutexLocker lock(&m_impl->state->mutex);
    m_impl->state->diagnosticsLogging = enabled;
    m_impl->scheduleWorker();
}

QObject* Ax25ReceiveModel::workerForTest() const
{
    return m_impl->state->worker;
}

Ax25ReceiveModel::QueueState Ax25ReceiveModel::queuesForTest() const
{
    QMutexLocker lock(&m_impl->state->mutex);
    return {m_impl->state->frames, m_impl->state->input.size(),
            m_impl->state->output.size(), m_impl->state->workerScheduled,
            m_impl->state->outputScheduled};
}

bool Ax25ReceiveModel::isEnabled() const
{
    return m_impl->enabled;
}

DecoderAudioModel::RouteStatus Ax25ReceiveModel::routeStatus() const
{
    return m_impl->audio->routeStatus();
}

} // namespace AetherSDR
