#include "Data2gReceiveModel.h"

#ifdef HAVE_DATA2G
#include "core/hfdata/Data2gMonitor.h"
#endif

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QTimeZone>

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AetherSDR {

struct Data2gReceiveModel::Impl {
    struct Input {
        QVector<float> samples;
        quint64 generation = 0;
        qint64 arrivedMs = 0;   // UTC, when the block's last sample reached feed()
    };
    struct Output {
        QVector<Data2gHeardBurst> bursts;
        quint64 faults = 0;
    };
    // One monitoring run, from enable to disable. Shared with the run's
    // worker, which can outlive both the run and a queued call to the owner.
    struct State {
        QMutex mutex;
        Data2gReceiveModel* owner = nullptr;   // null once the run is over
        std::atomic<quint64> generation{1};    // bumped by every reset
        std::atomic<bool> quit{false};
        std::deque<Input> input;
        std::deque<Output> output;
        qsizetype frames = 0;
        bool workerScheduled = false;
        bool outputScheduled = false;
        static constexpr qsizetype kMaxFrames =
            static_cast<qsizetype>(kSampleRateHz) * kMaxQueuedSeconds;

        // Called under mutex. Audio not yet decoded belongs to the stream
        // that just ended and goes. Bursts already decoded were heard whole
        // and stay queued for the owner.
        void retire()
        {
            ++generation;
            input.clear();
            frames = 0;
        }
    };

#ifdef HAVE_DATA2G
    class Worker final : public QObject {
    public:
        explicit Worker(std::shared_ptr<State> state)
            : m_state(std::move(state)), m_monitor(kSampleRateHz)
        {
            // Decode on this thread alone: the core's pool would otherwise
            // leave helper threads behind for the life of the process, for
            // no measurable gain on the monitor.
            static std::once_flag poolSized;
            std::call_once(poolSized, [] { hfdata::Data2gMonitor::setDecodeThreads(1); });
            m_monitor.setStamp([this](double startSeconds) {
                return startedAt(startSeconds)
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
                    m_originMs = kNoOrigin;
                    m_fed = 0;
                }
                if (input.generation != m_state->generation.load()) {
                    continue;   // queued before a reset that has since happened
                }
                decode(input);
            }
            QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
        }

    private:
        static constexpr qint64 kNoOrigin = std::numeric_limits<qint64>::min();

        // A burst is decoded only once it has arrived whole, and the queue
        // can run behind, so "now" is late by the burst's length or more. A
        // burst is timed by its position in the stream instead, and the
        // stream is anchored to the wall clock by each block's arrival, so a
        // gap in the audio moves the anchor and not the bursts after it.
        QDateTime startedAt(double startSeconds) const
        {
            if (m_originMs == kNoOrigin) {
                return QDateTime::currentDateTimeUtc();
            }
            return QDateTime::fromMSecsSinceEpoch(
                m_originMs + static_cast<qint64>(startSeconds * 1000.0), QTimeZone::UTC);
        }

        // A decode cannot be abandoned once started, so feed a fraction of a
        // second at a time and look for shutdown or a reset in between.
        void decode(const Input& input)
        {
            static constexpr qsizetype kChunk = kSampleRateHz / 5;
            Output output;
            const quint64 faultsBefore = m_monitor.faultCount();
            // The block's last sample arrived at arrivedMs, and the monitor's
            // clock will read m_fed + size there.
            m_originMs = input.arrivedMs
                - (m_fed + input.samples.size()) * qint64(1000) / kSampleRateHz;
            for (qsizetype offset = 0; offset < input.samples.size(); offset += kChunk) {
                if (m_state->quit.load() || input.generation != m_state->generation.load()) {
                    break;   // what the earlier chunks completed is still reported
                }
                const qsizetype n = std::min(kChunk, input.samples.size() - offset);
                const quint64 faultsNow = m_monitor.faultCount();
                const auto heard = m_monitor.feed(std::span<const float>(
                    input.samples.constData() + offset, static_cast<std::size_t>(n)));
                m_fed += n;
                if (m_monitor.faultCount() != faultsNow) {
                    // A fault restarted the monitor: its clock is at zero
                    // where the next chunk starts.
                    m_fed = 0;
                    m_originMs = input.arrivedMs
                        - (input.samples.size() - offset - n) * qint64(1000) / kSampleRateHz;
                }
                for (const hfdata::Data2gBurst& burst : heard) {
                    output.bursts.append({startedAt(burst.startSeconds),
                                          QDateTime::currentDateTimeUtc(),
                                          QString::fromUtf8(burst.head),
                                          QString::fromUtf8(burst.text)});
                }
            }
            output.faults = m_monitor.faultCount() - faultsBefore;
            if (output.bursts.isEmpty() && output.faults == 0) {
                return;
            }
            // A reset since this block was queued does not unhear a burst
            // that completed; only the end of the run does.
            QMutexLocker lock(&m_state->mutex);
            if (!m_state->owner) {
                return;
            }
            m_state->output.push_back(std::move(output));
            scheduleOutput(m_state);
        }

        std::shared_ptr<State> m_state;
        hfdata::Data2gMonitor m_monitor;
        quint64 m_generation = 0;
        qint64 m_originMs = kNoOrigin;   // UTC of the monitor's time zero
        qint64 m_fed = 0;                // samples fed since the monitor's time zero
    };
#endif

    Data2gReceiveModel* owner = nullptr;
    // The current run: all null while disabled. The worker thread exists only
    // while the monitor is on.
    std::shared_ptr<State> state;
    QObject* worker = nullptr;
    QThread* thread = nullptr;
    // Threads of ended runs that are still finishing a decode chunk.
    std::vector<QThread*> stopping;
    bool enabled = false;
    Stats stats;
    std::optional<PcmStreamDescriptor> lastSource;
    QElapsedTimer lastAudio;   // invalid until the first block since enabling
    static constexpr qint64 kAudioStaleMs = 2000;

    void startRun()
    {
#ifdef HAVE_DATA2G
        state = std::make_shared<State>();
        state->owner = owner;
        thread = new QThread;
        thread->setObjectName(QStringLiteral("Data2gReceive"));
        auto* created = new Worker(state);
        worker = created;
        created->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, created, &QObject::deleteLater);
        thread->start();
#endif
    }

    // Ends the run. A decode chunk in flight cannot be abandoned, so unless
    // `wait` is set the thread is left to finish it and is reaped when it has.
    void endRun(bool wait)
    {
        if (!state) {
            return;
        }
        {
            QMutexLocker lock(&state->mutex);
            state->owner = nullptr;
            state->quit = true;
            state->retire();
            state->output.clear();
        }
        QThread* ending = std::exchange(thread, nullptr);
        worker = nullptr;
        state.reset();
        if (wait) {
            ending->quit();
            ending->wait();
            delete ending;
            return;
        }
        stopping.push_back(ending);
        QObject::connect(ending, &QThread::finished, owner, [this, ending] {
            std::erase(stopping, ending);
            ending->deleteLater();
        });
        ending->quit();
    }

    // Caller holds State::mutex, which fences this against the run ending
    // and clearing State::owner.
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
        if (box != state) {
            return;   // a run that has ended
        }
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
                // A listener may disable the model from this signal.
                if (box != state) {
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
    m_impl->owner = this;
}

Data2gReceiveModel::~Data2gReceiveModel()
{
    Impl& d = *m_impl;
    d.endRun(/*wait=*/true);
    for (QThread* ending : d.stopping) {
        ending->wait();
        delete ending;
    }
    d.stopping.clear();
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
    Impl& d = *m_impl;
    if (d.enabled == enabled) {
        return;
    }
    d.enabled = enabled;
    d.lastAudio.invalidate();
    d.lastSource.reset();
    // Either way the next audio starts a new stream.
    if (enabled) {
        d.startRun();
    } else {
        d.endRun(/*wait=*/false);
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
    if (!d.enabled || !d.state) {
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
            d.state->input.push_back({mono, d.state->generation.load(),
                                      QDateTime::currentMSecsSinceEpoch()});
            d.state->frames += mono.size();
            d.scheduleWorker();
        }
    }
    if (overflow) {
        ++d.stats.overflows;
        emit statsChanged();
    }
}

void Data2gReceiveModel::feedFrom(const PcmStreamDescriptor& source, const QVector<float>& mono)
{
    Impl& d = *m_impl;
    if (!d.enabled) {
        return;
    }
    const bool changed = d.lastSource && *d.lastSource != source;
    d.lastSource = source;
    feed(mono, changed);
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
