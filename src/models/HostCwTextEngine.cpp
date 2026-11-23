#include "HostCwTextEngine.h"

#include "core/CwxLocalKeyer.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <utility>

namespace AetherSDR {

HostCwTextEngine::HostCwTextEngine(RadioModel* model, Ended ended)
    : m_model(model)
    , m_ended(std::move(ended))
{}

HostCwTextEngine::~HostCwTextEngine()
{
    close();
}

int HostCwTextEngine::pendingChars() const
{
    int n = m_keyer ? m_keyer->pendingChars() : 0;
    for (const Waiting& message : m_waiting) {
        n += static_cast<int>(message.text.size());
    }
    return n;
}

bool HostCwTextEngine::admitsLength(int chars) const
{
    return chars > 0 && chars <= kMaxMessageChars && pendingChars() + chars <= kMaxPendingChars;
}

bool HostCwTextEngine::playingFor(const TxCoordinator::Request& sender) const
{
    return m_run && m_sender.sameRequest(sender);
}

bool HostCwTextEngine::hasTextFor(const TxCoordinator::Request& sender) const
{
    return playingFor(sender) || std::any_of(m_waiting.cbegin(), m_waiting.cend(), [&sender](const Waiting& message) {
        return message.sender.sameRequest(sender);
    });
}

bool HostCwTextEngine::play(const TxCoordinator::Request& sender, const QString& text, int wpm,
                           TxCoordinator::Completion done)
{
    if (m_closed || !m_model || text.isEmpty() || wpm <= 0 || (m_run && !m_sender.sameRequest(sender))) {
        return false;
    }
    if (!m_run) {
        m_producer = m_model->registerTxProducerFor(sender);
        m_input = m_producer.request();
        if (!m_input.valid()) {
            m_producer.invalidate();
            m_producer = {};
            m_input = {};
            return false;
        }
        m_sender = sender;
        m_run = std::make_shared<char>();
        m_keyer = std::make_unique<CwxLocalKeyer>();
        // Word spacing between runs: 7 units at this run's speed after the last key-up.
        if (m_lastKeyUp != std::chrono::steady_clock::time_point{}) {
            m_keyer->setNotBefore(m_lastKeyUp + std::chrono::milliseconds(7 * std::max(1, int(1200.0 / wpm))));
        }
        m_runKeyUp = std::make_shared<std::atomic<std::chrono::steady_clock::rep>>(0);
        const std::weak_ptr<char> run = m_run;
        // Model thread. A live run token means `this` is alive: endRun() drops it before the keyer goes.
        const std::function<void()> onRefused = [this, run] { if (!run.expired()) { refused(run); } };
        RadioModel* model = m_model.data();
        // Worker thread: derive() and the queued hops take no model state.
        m_keyer->setOnKeyDownChange(
            [model, input = m_input, sender, onRefused, keyUp = m_runKeyUp, element = TxCoordinator::Request{}](
                bool down, std::chrono::steady_clock::time_point when) mutable {
                if (down) {
                    // A sender whose producer was invalidated keys nothing more.
                    element = sender.valid() ? input.derive() : TxCoordinator::Request{};
                } else {
                    keyUp->store(when.time_since_epoch().count(), std::memory_order_relaxed);
                }
                model->queueProducerCwKeyEdge(element, down, QStringLiteral("host-cw-text"), 0, 0, when,
                                              /*hostTextGates=*/true,
                                              down ? onRefused : std::function<void()>{});
            });
        // Queued behind the run's last key-up, so the model sees that edge first.
        m_keyer->setOnDrained([model, this, run] {
            QMetaObject::invokeMethod(model, [this, run] { if (!run.expired()) { drained(run); } }, Qt::QueuedConnection);
        });
    }
    m_done.push_back(std::move(done));
    m_keyer->start(text, wpm);
    return true;
}

bool HostCwTextEngine::hold(Waiting message)
{
    if (m_closed || message.text.isEmpty()) {
        return false;
    }
    int at = static_cast<int>(m_waiting.size());
    for (int i = at - 1; i >= 0; --i) {
        if (m_waiting.at(i).sender.sameRequest(message.sender)) {
            at = i + 1;
            break;
        }
    }
    m_waiting.insert(at, std::move(message));
    return true;
}

std::optional<HostCwTextEngine::Waiting> HostCwTextEngine::takeNext()
{
    while (!m_waiting.isEmpty()) {
        if (!m_waiting.front().sender.valid()) {
            m_waiting.removeFirst();
            continue;
        }
        if (m_run && !m_sender.sameRequest(m_waiting.front().sender)) {
            return std::nullopt;
        }
        return m_waiting.takeFirst();
    }
    return std::nullopt;
}

void HostCwTextEngine::dropWaiting(const TxCoordinator::Request& sender)
{
    m_waiting.removeIf([&sender](const Waiting& message) { return message.sender.sameRequest(sender); });
}

void HostCwTextEngine::endRun()
{
    // Belt and braces: invalidate() below fences the same elements; this
    // fences them before the keyer joins.
    m_producer.discardInputs();
    m_run.reset();
    m_input = {};
    m_keyer.reset();   // joins the worker; its last key-up finds a closed request
    if (m_runKeyUp) {
        if (const auto up = m_runKeyUp->load(std::memory_order_relaxed); up != 0) {
            m_lastKeyUp = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(up));
        }
        m_runKeyUp.reset();
    }
    if (m_model) {
        m_model->abortTxProducerInputs(m_producer, TransmitModel::PttSource::Mox);
    }
    m_producer.invalidate();
    m_producer = {};
    m_sender = {};
    std::vector<TxCoordinator::Completion> done = std::exchange(m_done, {});
    done.clear();   // the sender's Cwx deliveries finish now
}

void HostCwTextEngine::drained(const std::weak_ptr<char>& run)
{
    if (!run.lock() || !m_keyer || m_keyer->pendingChars() > 0) {
        return;
    }
    const TxCoordinator::Request sender = m_sender;
    endRun();
    if (m_ended) {
        m_ended(sender, false);
    }
}

void HostCwTextEngine::refused(const std::weak_ptr<char>& run)
{
    if (!run.lock()) {
        return;
    }
    const TxCoordinator::Request sender = m_sender;
    endRun();
    if (m_ended) {
        m_ended(sender, true);
    }
}

void HostCwTextEngine::stop(const TxCoordinator::Request& sender)
{
    dropWaiting(sender);
    if (playingFor(sender)) {
        endRun();
    }
}

void HostCwTextEngine::stopAll()
{
    m_waiting.clear();
    if (m_run) {
        endRun();
    }
}

void HostCwTextEngine::close()
{
    m_closed = true;
    stopAll();
}

} // namespace AetherSDR
