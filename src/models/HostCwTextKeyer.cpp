#include "HostCwTextKeyer.h"

#include "core/CwxLocalKeyer.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QMetaObject>

#include <chrono>
#include <utility>

namespace AetherSDR {

HostCwTextKeyer::HostCwTextKeyer(RadioModel* model, Ended ended)
    : m_model(model)
    , m_ended(std::move(ended))
{}

HostCwTextKeyer::~HostCwTextKeyer()
{
    close();
}

int HostCwTextKeyer::pendingChars() const
{
    int n = m_keyer ? m_keyer->pendingChars() : 0;
    for (const Waiting& message : m_waiting) {
        n += static_cast<int>(message.text.size());
    }
    return n;
}

bool HostCwTextKeyer::admitsLength(int chars) const
{
    return chars > 0 && chars <= kMaxMessageChars && pendingChars() + chars <= kMaxPendingChars;
}

bool HostCwTextKeyer::playingFor(const TxCoordinator::Request& sender) const
{
    return m_run && m_sender.sameRequest(sender);
}

bool HostCwTextKeyer::play(const TxCoordinator::Request& sender, const QString& text, int wpm,
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
        const std::weak_ptr<char> run = m_run;
        // Model thread; `this` outlives every live run token.
        const std::function<void()> onRefused = [this, run] { refused(run); };
        RadioModel* model = m_model.data();
        // Worker thread: derive() and the queued hops take no model state.
        m_keyer->setOnKeyDownChange(
            [model, input = m_input, onRefused, element = TxCoordinator::Request{}](
                bool down, std::chrono::steady_clock::time_point when) mutable {
                if (down) {
                    element = input.derive();
                }
                model->queueProducerCwKeyEdge(element, down, QStringLiteral("host-cw-text"), 0, 0, when,
                                              /*hostTextGates=*/true,
                                              down ? onRefused : std::function<void()>{});
            });
        // Queued behind the run's last key-up, so the model sees that edge first.
        m_keyer->setOnDrained([model, this, run] {
            QMetaObject::invokeMethod(model, [this, run] { drained(run); }, Qt::QueuedConnection);
        });
    }
    m_done.push_back(std::move(done));
    m_keyer->start(text, wpm);
    return true;
}

bool HostCwTextKeyer::hold(Waiting message)
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

std::optional<HostCwTextKeyer::Waiting> HostCwTextKeyer::takeNext()
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

void HostCwTextKeyer::dropWaiting(const TxCoordinator::Request& sender)
{
    m_waiting.removeIf([&sender](const Waiting& message) { return message.sender.sameRequest(sender); });
}

void HostCwTextKeyer::endRun()
{
    // Fence first: an element the worker emits from here on derives from a
    // dead input and is refused at admission.
    m_producer.discardInputs();
    m_run.reset();
    m_input = {};
    m_keyer.reset();   // joins the worker; its last key-up finds a closed request
    if (m_model) {
        m_model->abortTxProducerInputs(m_producer, TransmitModel::PttSource::Mox);
    }
    m_producer.invalidate();
    m_producer = {};
    m_sender = {};
    std::vector<TxCoordinator::Completion> done = std::exchange(m_done, {});
    done.clear();   // the sender's Cwx deliveries finish now
}

void HostCwTextKeyer::drained(const std::weak_ptr<char>& run)
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

void HostCwTextKeyer::refused(const std::weak_ptr<char>& run)
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

void HostCwTextKeyer::stop(const TxCoordinator::Request& sender)
{
    dropWaiting(sender);
    if (playingFor(sender)) {
        endRun();
    }
}

void HostCwTextKeyer::stopAll()
{
    m_waiting.clear();
    if (m_run) {
        endRun();
    }
}

void HostCwTextKeyer::close()
{
    m_closed = true;
    stopAll();
}

} // namespace AetherSDR
