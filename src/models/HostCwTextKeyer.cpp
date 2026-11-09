#include "HostCwTextKeyer.h"

#include "core/CwxLocalKeyer.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace AetherSDR {

HostCwTextKeyer::HostCwTextKeyer(RadioModel* model, QString source)
    : m_model(model)
    , m_source(std::move(source))
{}

HostCwTextKeyer::~HostCwTextKeyer()
{
    stop();
    m_producer.invalidate();
}

bool HostCwTextKeyer::admitsLength(int chars) const
{
    const int pending = m_queuedChars + (m_keyer ? m_keyer->pendingChars() : 0);
    return chars <= kMaxMessageChars && pending + chars <= kMaxPendingChars;
}

void HostCwTextKeyer::noteQueued(int chars)
{
    m_queuedChars += chars;
}

bool HostCwTextKeyer::send(const QString& text)
{
    m_queuedChars = std::max(0, m_queuedChars - static_cast<int>(text.size()));
    if (m_closed || !m_model || text.isEmpty() || !admitsLength(static_cast<int>(text.size()))
        || m_model->hostCwTextRefusal() != RadioModel::HostCwTextRefusal::None) {
        return false;
    }
    // A reconnect or stop() invalidates the input; elements derived from it
    // can then no longer key, so the next send starts a fresh run.
    if (!m_keyer || !m_input.valid()) {
        m_keyer.reset();
        if (!m_producer.valid()) {
            m_producer = m_model->registerTxProducer();
        }
        m_input = m_producer.request();
        if (!m_input.valid()) {
            return false;
        }
        m_keyer = std::make_unique<CwxLocalKeyer>();
        m_run = std::make_shared<char>();
        // A refused key-down ends the message, once, and only for the run it
        // belongs to: the token dies with stop(), so a stale refusal is inert.
        // Runs on the model thread; `this` outlives every live token.
        const std::function<void()> refused = [this, run = std::weak_ptr<char>(m_run)] {
            if (run.lock()) {
                stop();
            }
        };
        // Worker thread: derive() and the queued hop take no model state.
        m_keyer->setOnKeyDownChange(
            [model = m_model.data(), input = m_input, source = m_source, refused,
             element = TxCoordinator::Request{}](
                bool down, std::chrono::steady_clock::time_point when) mutable {
                if (down) {
                    element = input.derive();
                }
                model->queueProducerCwKeyEdge(element, down, source, 0, 0, when,
                                              /*hostTextGates=*/true, down ? refused : std::function<void()>{});
            });
    }
    m_keyer->start(text, m_model->transmitModel().cwSpeed());
    return true;
}

void HostCwTextKeyer::stop()
{
    // Fence first: an element the worker emits from here on derives from a
    // dead input and is refused at admission.
    m_producer.discardInputs();
    m_run.reset();
    m_input = {};
    m_keyer.reset();   // joins the worker; its final key-up finds a closed request
    if (m_model) {
        m_model->abortTxProducerInputs(m_producer, TransmitModel::PttSource::Dax);
    }
}

void HostCwTextKeyer::close()
{
    m_closed = true;
    stop();
}

} // namespace AetherSDR
