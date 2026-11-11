#include "Data2gReceiveModel.h"

#ifdef HAVE_DATA2G
#include "core/hfdata/Data2gMonitor.h"
#endif

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <deque>
#include <span>
#include <utility>

namespace AetherSDR {

struct Data2gReceiveModel::Impl {
    struct Input {
        QVector<float> samples;
        quint64 generation = 0;
    };
    struct Output {
        quint64 generation = 0;
        QVector<Data2gHeardBurst> bursts;
        quint64 faults = 0;
    };
    // Shared with the worker, which can outlive a queued call to the owner.
    struct State {
        QMutex mutex;
        Data2gReceiveModel* owner = nullptr;   // null once the model is going
        std::atomic<quint64> generation{1};    // bumped by every reset
        std::atomic<bool> quit{false};
        std::deque<Input> input;
        std::deque<Output> output;
        qsizetype frames = 0;
        bool workerScheduled = false;
        bool outputScheduled = false;
        static constexpr qsizetype kMaxFrames =
            static_cast<qsizetype>(kSampleRateHz) * kMaxQueuedSeconds;

        // Called under mutex.
        void retire()
        {
            ++generation;
            input.clear();
            output.clear();
            frames = 0;
        }
    };

#ifdef HAVE_DATA2G
    class Worker final : public QObject {
    public:
        explicit Worker(std::shared_ptr<State> state)
            : m_state(std::move(state)), m_monitor(kSampleRateHz)
        {
            m_monitor.setStamp([](double) {
                return QDateTime::currentDateTimeUtc()
                    .toString(QStringLiteral("HH:mm:ss'Z'")).toStdString();
            });
        }

        void drain()
        {
            // A few blocks per event, so shutdown and resets are never stuck
            // behind a backlog.
            for (int count = 0; count < 8; ++count) {
                Input input;
                {
                    QMutexLocker lock(&m_state->mutex);
                    if (!m_state->owner || m_state->input.empty()) {
                        m_state->workerScheduled = false;
                        return;
                    }
                    input = std::move(m_state->input.front());
                    m_state->input.pop_front();
                    m_state->frames -= input.samples.size();
                }
                if (input.generation != m_generation) {
                    m_monitor.reset();
                    m_generation = input.generation;
                }
                if (input.generation != m_state->generation.load()) {
                    continue;   // queued before a reset that has since happened
                }
                decode(input);
            }
            QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
        }

    private:
        // A decode cannot be abandoned once started, so feed a fraction of a
        // second at a time and look for shutdown or a reset in between.
        void decode(const Input& input)
        {
            static constexpr qsizetype kChunk = kSampleRateHz / 5;
            Output output;
            output.generation = input.generation;
            const quint64 faultsBefore = m_monitor.faultCount();
            for (qsizetype offset = 0; offset < input.samples.size(); offset += kChunk) {
                if (m_state->quit.load() || input.generation != m_state->generation.load()) {
                    return;
                }
                const qsizetype n = std::min(kChunk, input.samples.size() - offset);
                const auto heard = m_monitor.feed(std::span<const float>(
                    input.samples.constData() + offset, static_cast<std::size_t>(n)));
                for (const hfdata::Data2gBurst& burst : heard) {
                    output.bursts.append({QDateTime::currentDateTimeUtc(),
                                          QString::fromUtf8(burst.head),
                                          QString::fromUtf8(burst.text)});
                }
            }
            output.faults = m_monitor.faultCount() - faultsBefore;
            if (output.bursts.isEmpty() && output.faults == 0) {
                return;
            }
            QMutexLocker lock(&m_state->mutex);
            if (!m_state->owner || output.generation != m_state->generation.load()) {
                return;
            }
            m_state->output.push_back(std::move(output));
            scheduleOutput(m_state);
        }

        std::shared_ptr<State> m_state;
        hfdata::Data2gMonitor m_monitor;
        quint64 m_generation = 0;
    };
#endif

    Data2gReceiveModel* owner = nullptr;
    std::shared_ptr<State> state = std::make_shared<State>();
    QObject* worker = nullptr;
    QThread thread;
    bool enabled = false;
    Stats stats;
    QElapsedTimer lastAudio;   // invalid until the first block since enabling
    static constexpr qint64 kAudioStaleMs = 2000;

    // Caller holds State::mutex, which fences this against the owner's
    // destructor clearing State::owner.
    static void scheduleOutput(const std::shared_ptr<State>& state)
    {
        if (!state->owner || state->outputScheduled) {
            return;
        }
        state->outputScheduled = true;
        Data2gReceiveModel* target = state->owner;
        QMetaObject::invokeMethod(target, [target, state] {
            target->m_impl->publish(state);
        }, Qt::QueuedConnection);
    }

    // Caller holds State::mutex.
    void scheduleWorker()
    {
#ifdef HAVE_DATA2G
        if (!worker || state->workerScheduled) {
            return;
        }
        state->workerScheduled = true;
        auto* target = static_cast<Worker*>(worker);
        QMetaObject::invokeMethod(target, [target] { target->drain(); }, Qt::QueuedConnection);
#endif
    }

    void publish(const std::shared_ptr<State>& box)
    {
        std::deque<Output> output;
        {
            QMutexLocker lock(&box->mutex);
            output.swap(box->output);
            box->outputScheduled = false;
        }
        const QPointer<Data2gReceiveModel> guard(owner);
        bool changed = false;
        for (const Output& item : output) {
            if (item.faults) {
                stats.faults += item.faults;
                changed = true;
            }
            for (const Data2gHeardBurst& burst : item.bursts) {
                // A listener may disable or reset the model from this signal.
                if (!enabled || item.generation != box->generation.load()) {
                    break;
                }
                ++stats.bursts;
                changed = true;
                emit owner->burstHeard(burst);
                if (!guard) {
                    return;
                }
            }
        }
        if (changed) {
            emit owner->statsChanged();
        }
    }
};

Data2gReceiveModel::Data2gReceiveModel(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<AetherSDR::Data2gHeardBurst>();
    Impl& d = *m_impl;
    d.owner = this;
    d.state->owner = this;
#ifdef HAVE_DATA2G
    auto* worker = new Impl::Worker(d.state);
    d.worker = worker;
    worker->moveToThread(&d.thread);
    connect(&d.thread, &QThread::finished, worker, &QObject::deleteLater);
    d.thread.setObjectName(QStringLiteral("Data2gReceive"));
    d.thread.start();
#endif
}

Data2gReceiveModel::~Data2gReceiveModel()
{
    Impl& d = *m_impl;
    {
        QMutexLocker lock(&d.state->mutex);
        d.state->owner = nullptr;
        d.state->quit = true;
        d.state->retire();
    }
    if (d.thread.isRunning()) {
        d.thread.quit();
        d.thread.wait();
    }
}

bool Data2gReceiveModel::available()
{
#ifdef HAVE_DATA2G
    return true;
#else
    return false;
#endif
}

QString Data2gReceiveModel::upstreamVersion()
{
#ifdef HAVE_DATA2G
    return QString::fromLatin1(hfdata::Data2gMonitor::upstreamVersion());
#else
    return {};
#endif
}

void Data2gReceiveModel::setEnabled(bool enabled)
{
    enabled = enabled && available();
    if (m_impl->enabled == enabled) {
        return;
    }
    m_impl->enabled = enabled;
    m_impl->lastAudio.invalidate();
    // Either way the next audio starts a new stream.
    {
        QMutexLocker lock(&m_impl->state->mutex);
        m_impl->state->retire();
    }
    emit enabledChanged(enabled);
}

bool Data2gReceiveModel::isEnabled() const
{
    return m_impl->enabled;
}

void Data2gReceiveModel::feed(const QVector<float>& mono, bool discontinuity)
{
    Impl& d = *m_impl;
    if (!d.enabled) {
        return;
    }
    if (!mono.isEmpty()) {
        d.lastAudio.start();
    }
    bool overflow = false;
    {
        QMutexLocker lock(&d.state->mutex);
        if (discontinuity) {
            d.state->retire();
        }
        if (mono.isEmpty()) {
            return;
        }
        if (d.state->frames + mono.size() > Impl::State::kMaxFrames) {
            d.state->retire();
            overflow = true;
        }
        if (mono.size() <= Impl::State::kMaxFrames) {
            d.state->input.push_back({mono, d.state->generation.load()});
            d.state->frames += mono.size();
            d.scheduleWorker();
        }
    }
    if (overflow) {
        ++d.stats.overflows;
        emit statsChanged();
    }
}

void Data2gReceiveModel::reset()
{
    QMutexLocker lock(&m_impl->state->mutex);
    m_impl->state->retire();
}

bool Data2gReceiveModel::audioFlowing() const
{
    return m_impl->enabled && m_impl->lastAudio.isValid()
        && m_impl->lastAudio.elapsed() < Impl::kAudioStaleMs;
}

Data2gReceiveModel::Stats Data2gReceiveModel::stats() const
{
    return m_impl->stats;
}

} // namespace AetherSDR
