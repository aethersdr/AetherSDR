#include "Data2gSessionModel.h"

#ifdef HAVE_DATA2G
#include "core/hfdata/Data2gMonitor.h"
#include "core/hfdata/Data2gSession.h"
#endif

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AetherSDR {

struct Data2gSessionModel::Impl {
#ifdef HAVE_DATA2G
    using Step = hfdata::Data2gSessionStep;

    // One session's run. Shared with the run's worker, which can outlive both
    // the run and a queued call to the owner.
    struct Run {
        QMutex mutex;
        Data2gSessionModel* owner = nullptr;   // null once the run is over
        std::atomic<bool> quit{false};
        std::deque<QVector<float>> input;
        std::deque<Step> output;
        qsizetype frames = 0;
        bool workerScheduled = false;
        bool outputScheduled = false;
        // step() on the worker; the request functions from either thread.
        std::unique_ptr<hfdata::Data2gSession> session;
    };

    class Worker final : public QObject {
    public:
        explicit Worker(std::shared_ptr<Run> run) : m_run(std::move(run)) {}

        // `all`: until the queue is empty (tests). Otherwise a few blocks per
        // event, so shutdown is never stuck behind a backlog.
        void drain(bool all)
        {
            for (int count = 0; all || count < 8; ++count) {
                QVector<float> input;
                {
                    QMutexLocker lock(&m_run->mutex);
                    if (!m_run->owner || m_run->input.empty()) {
                        m_run->workerScheduled = false;
                        return;
                    }
                    input = std::move(m_run->input.front());
                    m_run->input.pop_front();
                    m_run->frames -= input.size();
                }
                process(input);
            }
            QMetaObject::invokeMethod(this, [this] { drain(false); }, Qt::QueuedConnection);
        }

    private:
        // A decode cannot be abandoned once started, so step a tenth of a
        // second at a time and look for shutdown in between.
        void process(const QVector<float>& input)
        {
            static constexpr qsizetype kChunk = kSampleRateHz / 10;
            for (qsizetype offset = 0; offset < input.size(); offset += kChunk) {
                if (m_run->quit.load())
                    return;
                const qsizetype n = std::min(kChunk, input.size() - offset);
                Step step = m_run->session->step(std::span<const float>(
                    input.constData() + offset, static_cast<std::size_t>(n)));
                const bool news = !step.transmission.empty() || step.abortTx || !step.events.empty()
                    || !step.received.empty() || step.peerHeard || step.dataActive
                    || step.state != m_lastState;
                m_lastState = step.state;
                if (!news)
                    continue;
                QMutexLocker lock(&m_run->mutex);
                if (!m_run->owner)
                    return;
                m_run->output.push_back(std::move(step));
                scheduleOutput(m_run);
            }
        }

        std::shared_ptr<Run> m_run;
        hfdata::Data2gLinkState m_lastState{hfdata::Data2gLinkState::Idle};
    };

    // Caller holds Run::mutex, which fences this against the run ending.
    static void scheduleOutput(const std::shared_ptr<Run>& run)
    {
        if (!run->owner || run->outputScheduled)
            return;
        run->outputScheduled = true;
        Data2gSessionModel* target = run->owner;
        QMetaObject::invokeMethod(target, [target, run] {
            target->m_impl->publish(run);
        }, Qt::QueuedConnection);
    }

    std::shared_ptr<Run> run;
    Worker* worker = nullptr;
    QThread* thread = nullptr;
    std::vector<QThread*> stopping;
#endif

    Data2gSessionModel* owner = nullptr;
    Data2gTransmitSink* sink = nullptr;
    Limits limits;
    std::function<qint64()> clockMs;
    QElapsedTimer wallClock;
    QTimer tick;

    QString myCall;
    QString peer;
    State state = State::Idle;
    bool armed = false;
    bool aborting = false;
    double driveDb = 0.0;
    Stats stats;

    std::optional<PcmStreamDescriptor> lastSource;
    qint64 runStartMs = -1;      // clock time of the run's first audio
    qint64 fedSamples = 0;
    qint64 lastPeerMs = 0;
    qint64 lastDataMs = 0;
    qint64 connectStartMs = 0;
    qint64 closingAtMs = -1;     // when the closing-ID window ends

    qint64 now() const { return clockMs(); }

    void setState(State next)
    {
        if (state == next)
            return;
        state = next;
        emit owner->stateChanged(state);
    }

    void setArmed(bool next)
    {
        if (armed == next)
            return;
        armed = next;
        emit owner->armedChanged(armed);
    }

#ifdef HAVE_DATA2G
    static State stateOf(hfdata::Data2gLinkState state)
    {
        switch (state) {
        case hfdata::Data2gLinkState::Connecting:
            return State::Connecting;
        case hfdata::Data2gLinkState::Connected:
            return State::Connected;
        case hfdata::Data2gLinkState::Disconnecting:
            return State::Disconnecting;
        case hfdata::Data2gLinkState::Idle:
            break;
        }
        return State::Idle;
    }

    void startRun()
    {
        run = std::make_shared<Run>();
        run->owner = owner;
        run->session = std::make_unique<hfdata::Data2gSession>(myCall.toStdString(), kSampleRateHz);
        thread = new QThread;
        thread->setObjectName(QStringLiteral("Data2gSession"));
        worker = new Worker(run);
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        thread->start();
        lastSource.reset();
        runStartMs = -1;
        fedSamples = 0;
        closingAtMs = -1;
        tick.start();
    }

    // Ends the run. A step in flight cannot be abandoned, so unless `wait` is
    // set the thread is left to finish it and is reaped when it has.
    void endRun(bool wait)
    {
        tick.stop();
        if (!run)
            return;
        {
            QMutexLocker lock(&run->mutex);
            run->owner = nullptr;
            run->quit = true;
            run->input.clear();
            run->output.clear();
            run->frames = 0;
        }
        QThread* ending = std::exchange(thread, nullptr);
        worker = nullptr;
        run.reset();
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

    // Caller holds Run::mutex.
    void scheduleWorker()
    {
        if (!worker || run->workerScheduled)
            return;
        run->workerScheduled = true;
        Worker* target = worker;
        QMetaObject::invokeMethod(target, [target] { target->drain(false); }, Qt::QueuedConnection);
    }

    void enqueue(const QVector<float>& mono)
    {
        if (!run || mono.isEmpty())
            return;
        if (runStartMs < 0)
            runStartMs = now();
        fedSamples += mono.size();
        bool overflow = false;
        {
            QMutexLocker lock(&run->mutex);
            run->input.push_back(mono);
            run->frames += mono.size();
            overflow = run->frames * 1000 / kSampleRateHz > limits.maxBacklogMs;
            scheduleWorker();
        }
        if (overflow)
            abort(QStringLiteral("The session engine fell behind the audio."));
    }

    QByteArray forTransmit(const std::vector<float>& mono) const
    {
        const float gain = static_cast<float>(std::pow(10.0, driveDb / 20.0));
        QByteArray pcm(static_cast<qsizetype>(mono.size() * 2 * sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(pcm.data());
        for (std::size_t i = 0; i < mono.size(); ++i) {
            const float v = std::clamp(mono[i] * gain, -1.0f, 1.0f);
            out[2 * i] = v;
            out[2 * i + 1] = v;
        }
        return pcm;
    }

    void publish(const std::shared_ptr<Run>& box)
    {
        if (box != run)
            return;   // a run that has ended
        std::deque<Step> output;
        {
            QMutexLocker lock(&box->mutex);
            output.swap(box->output);
            box->outputScheduled = false;
        }
        const QPointer<Data2gSessionModel> guard(owner);
        for (Step& step : output) {
            if (box != run)
                return;   // aborted while delivering
            deliver(step);
            if (!guard)
                return;
        }
    }

    void deliver(Step& step)
    {
        const std::shared_ptr<Run> current = run;
        const qint64 t = now();
        if (step.peerHeard)
            lastPeerMs = t;
        if (step.dataActive)
            lastDataMs = t;
        if (step.abortTx)
            sink->stop();

        if (!step.transmission.empty()) {
            // The gate. Nothing below this line runs for an unarmed model.
            if (!armed || aborting) {
                ++stats.dropped;
            } else if (sink->isActive()) {
                // The engine keeps time by its audio; the transmitter keeps
                // real time. Two at once means the two have come apart.
                abort(QStringLiteral("A transmission was still on the air when the next was due."));
                return;
            } else {
                QString error;
                if (sink->start(forTransmit(step.transmission), error)) {
                    ++stats.transmissions;
                } else {
                    abort(QStringLiteral("Transmit was refused: %1").arg(error));
                    return;
                }
            }
        }

        for (const std::string& event : step.events) {
            const QString text = QString::fromUtf8(event);
            emit owner->sessionEvent(text);
            if (current != run)
                return;
            if (text.startsWith(QLatin1String("REFUSED")) && state == State::Connecting) {
                abort(text);
                return;
            }
        }
        if (!step.received.empty()) {
            emit owner->dataReceived(QByteArray(reinterpret_cast<const char*>(step.received.data()),
                                                static_cast<qsizetype>(step.received.size())));
            if (current != run)
                return;
        }

        const State next = stateOf(step.state);
        if (next == State::Idle && state != State::Idle)
            closingAtMs = t + limits.closingMs;
        if (next == State::Connected && state != State::Connected)
            lastPeerMs = lastDataMs = t;
        setState(next);
    }

    // The watchdog, the closing window and the audio clock, about once a
    // second and after every settle().
    void supervise()
    {
        if (!run)
            return;
        const qint64 t = now();

        // The engine keeps time by sample count. If the receive tap stops
        // delivering while the transmitter is busy, feed silence in its place.
        if (runStartMs >= 0 && sink->isActive()) {
            const qint64 expected = (t - runStartMs) * kSampleRateHz / 1000;
            const qint64 behind = expected - fedSamples;
            if (behind > kSampleRateHz * 3 / 20) {
                stats.paddedMs += static_cast<quint64>(behind * 1000 / kSampleRateHz);
                enqueue(QVector<float>(static_cast<qsizetype>(behind), 0.0f));
                if (!run)
                    return;
            }
        }

        if (state == State::Connecting && t - connectStartMs > limits.connectingMs) {
            abort(QStringLiteral("The connect attempt ran past its limit."));
        } else if (state == State::Connected && t - lastPeerMs > limits.linkLostMs) {
            abort(QStringLiteral("Watchdog: the other station has not been heard."));
        } else if (state == State::Connected && t - lastDataMs > limits.idleMs) {
            abort(QStringLiteral("Watchdog: the session has been idle."));
        } else if (state == State::Idle && closingAtMs >= 0 && t >= closingAtMs && !sink->isActive()) {
            // The session is over and its closing ID has had its window.
            endRun(false);
            setArmed(false);
        }
    }
#endif

    void abort(const QString& reason)
    {
#ifdef HAVE_DATA2G
        if (!run || aborting)
            return;
        aborting = true;
        setArmed(false);
        sink->stop();
        run->session->requestAbort();
        endRun(false);
        setState(State::Idle);
        aborting = false;
        emit owner->sessionAborted(reason);
#else
        Q_UNUSED(reason);
#endif
    }
};

Data2gSessionModel::Data2gSessionModel(Data2gTransmitSink* sink, QObject* parent)
    : Data2gSessionModel(sink, Limits{}, {}, parent)
{
}

Data2gSessionModel::Data2gSessionModel(Data2gTransmitSink* sink, Limits limits,
                                       std::function<qint64()> clockMs, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<AetherSDR::Data2gSessionModel::State>();
    m_impl->owner = this;
    m_impl->sink = sink;
    m_impl->limits = limits;
    m_impl->wallClock.start();
    m_impl->clockMs = clockMs ? std::move(clockMs)
                              : std::function<qint64()>([impl = m_impl.get()] { return impl->wallClock.elapsed(); });
    m_impl->tick.setInterval(50);
#ifdef HAVE_DATA2G
    connect(&m_impl->tick, &QTimer::timeout, this, [this] { m_impl->supervise(); });
#endif
    if (sink) {
        sink->onFinished = [this](bool ok, const QString& why) {
            if (!ok && m_impl->armed && !m_impl->aborting)
                m_impl->abort(QStringLiteral("The transmission failed: %1").arg(why));
        };
    }
}

Data2gSessionModel::~Data2gSessionModel()
{
    if (m_impl->sink)
        m_impl->sink->onFinished = {};
#ifdef HAVE_DATA2G
    if (m_impl->run) {
        if (m_impl->sink)
            m_impl->sink->stop();
        m_impl->endRun(true);
    }
    for (QThread* thread : std::exchange(m_impl->stopping, {})) {
        thread->disconnect(this);
        thread->wait();
        delete thread;
    }
#endif
}

bool Data2gSessionModel::available()
{
#ifdef HAVE_DATA2G
    return true;
#else
    return false;
#endif
}

void Data2gSessionModel::setMyCall(const QString& call)
{
    m_impl->myCall = call.trimmed().toUpper();
}

QString Data2gSessionModel::myCall() const
{
    return m_impl->myCall;
}

bool Data2gSessionModel::connectTo(const QString& peerCall, int bandwidthHz, QString& error)
{
#ifdef HAVE_DATA2G
    Impl& impl = *m_impl;
    if (impl.run) {
        error = QStringLiteral("A session is already under way.");
        return false;
    }
    if (!impl.sink) {
        error = QStringLiteral("No transmitter is attached.");
        return false;
    }
    if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_NO_TX")) {
        error = QStringLiteral("Transmit is disabled for this run (AETHER_AUTOMATION_NO_TX).");
        return false;
    }
    if (impl.myCall.isEmpty()) {
        error = QStringLiteral("Set your call sign first.");
        return false;
    }
    hfdata::Data2gBandwidth bandwidth;
    switch (bandwidthHz) {
    case 500:
        bandwidth = hfdata::Data2gBandwidth::Hz500;
        break;
    case 1200:
        bandwidth = hfdata::Data2gBandwidth::Hz1200;
        break;
    case 2400:
        bandwidth = hfdata::Data2gBandwidth::Hz2400;
        break;
    default:
        error = QStringLiteral("Bandwidth must be 500, 1200 or 2400 Hz.");
        return false;
    }
    const QString refusal = impl.sink->startRefusal();
    if (!refusal.isEmpty()) {
        error = refusal;
        return false;
    }

    // Decode on the session's own thread alone, as the monitor does.
    static std::once_flag poolSized;
    std::call_once(poolSized, [] { hfdata::Data2gMonitor::setDecodeThreads(1); });

    impl.startRun();
    if (!impl.run->session->isValid()) {
        impl.endRun(true);
        error = QStringLiteral("%1 is not a call sign Data2G can send.").arg(impl.myCall);
        return false;
    }
    impl.peer = peerCall.trimmed().toUpper();
    impl.connectStartMs = impl.now();
    impl.run->session->setKeyLeadSeconds(hfdata::Data2gSession::kDefaultKeyLeadSeconds);
    impl.run->session->requestConnect(impl.peer.toStdString(), bandwidth);
    impl.setArmed(true);
    impl.setState(State::Connecting);
    return true;
#else
    Q_UNUSED(peerCall);
    Q_UNUSED(bandwidthHz);
    error = QStringLiteral("This build does not include Data2G.");
    return false;
#endif
}

void Data2gSessionModel::disconnectSession()
{
#ifdef HAVE_DATA2G
    if (m_impl->run)
        m_impl->run->session->requestDisconnect();
#endif
}

void Data2gSessionModel::abortSession(const QString& reason)
{
    m_impl->abort(reason);
}

bool Data2gSessionModel::sendData(const QByteArray& bytes)
{
#ifdef HAVE_DATA2G
    if (!m_impl->run || m_impl->state != State::Connected || bytes.isEmpty())
        return false;
    m_impl->run->session->send(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    return true;
#else
    Q_UNUSED(bytes);
    return false;
#endif
}

void Data2gSessionModel::setDriveDb(double db)
{
    m_impl->driveDb = std::clamp(db, kMinDriveDb, kMaxDriveDb);
}

double Data2gSessionModel::driveDb() const
{
    return m_impl->driveDb;
}

void Data2gSessionModel::feedFrom(const PcmStreamDescriptor& source, const QVector<float>& mono)
{
#ifdef HAVE_DATA2G
    Impl& impl = *m_impl;
    if (!impl.run)
        return;
    if (impl.lastSource && *impl.lastSource != source) {
        impl.abort(QStringLiteral("The receive source changed."));
        return;
    }
    impl.lastSource = source;
    impl.enqueue(mono);
#else
    Q_UNUSED(source);
    Q_UNUSED(mono);
#endif
}

void Data2gSessionModel::receiverChanged()
{
    m_impl->abort(QStringLiteral("The receiver was retuned or changed mode."));
}

Data2gSessionModel::State Data2gSessionModel::state() const
{
    return m_impl->state;
}

bool Data2gSessionModel::isArmed() const
{
    return m_impl->armed;
}

QString Data2gSessionModel::peer() const
{
    return m_impl->peer;
}

Data2gSessionModel::Stats Data2gSessionModel::stats() const
{
    return m_impl->stats;
}

void Data2gSessionModel::settle()
{
#ifdef HAVE_DATA2G
    Impl& impl = *m_impl;
    if (!impl.run)
        return;
    {
        QMutexLocker lock(&impl.run->mutex);
        impl.run->workerScheduled = true;
    }
    Impl::Worker* worker = impl.worker;
    QMetaObject::invokeMethod(worker, [worker] { worker->drain(true); }, Qt::BlockingQueuedConnection);
    impl.publish(impl.run);
    impl.supervise();
#endif
}

} // namespace AetherSDR
