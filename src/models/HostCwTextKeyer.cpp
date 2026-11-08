#include "HostCwTextKeyer.h"

#include "core/CwxLocalKeyer.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <chrono>
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

bool HostCwTextKeyer::send(const QString& text)
{
    if (!m_model || text.isEmpty()
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
        // Worker thread: derive() and the queued hop take no model state.
        m_keyer->setOnKeyDownChange(
            [model = m_model.data(), input = m_input, source = m_source, element = TxCoordinator::Request{}](
                bool down, std::chrono::steady_clock::time_point when) mutable {
                if (down) {
                    element = input.derive();
                }
                model->queueProducerCwKeyEdge(element, down, source, 0, 0, when);
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
    m_input = {};
    m_keyer.reset();   // joins the worker; its final key-up finds a closed request
    if (m_model) {
        m_model->abortTxProducerInputs(m_producer, TransmitModel::PttSource::Dax);
    }
}

} // namespace AetherSDR
