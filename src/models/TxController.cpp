#include "TxController.h"
#include "RadioModel.h"

#include <utility>

namespace AetherSDR {

TxController::TxController(RadioModel* radio, TransmitModel::PttSource source)
    : m_radio(radio), m_producer(radio ? radio->registerTxProducer() : TxCoordinator::Producer{}),
      m_connection(m_producer.request()), m_source(source)
{
}

TxController::~TxController()
{
    if (!m_scopeOwner) {
        invalidate();
    }
}

TxController::TxController(InputScopeTag, std::shared_ptr<TxController> source,
                           const TxCoordinator::Request& input)
    : m_radio(source->m_radio), m_producer(source->m_producer),
      m_connection(input), m_source(source->m_source),
      m_scopeOwner(std::move(source)), m_scopeInput(input)
{
}

TxController::TxController(NativeDeviceTag, RadioModel* radio, QObject* device)
    : m_radio(radio), m_producer(radio->registerTxProducer(device)),
      m_source(TransmitModel::PttSource::Mox), m_nativeDevice(true)
{
}

std::shared_ptr<TxController> TxController::forNativeDevice(RadioModel* radio, QObject* device)
{
    if (!radio || !device || QThread::currentThread() != radio->thread()) { return {}; }
    return std::make_shared<TxController>(NativeDeviceTag{}, radio, device);
}

TxCoordinator::Request TxController::captureRawInput() const
{
    return m_nativeDevice ? m_producer.request() : TxCoordinator::Request{};
}

void TxController::discardDeviceInputs() const
{
    if (m_nativeDevice) { m_producer.discardInputs(); }
}

void TxController::cleanupDeviceInputs()
{
    if (m_nativeDevice && m_radio && QThread::currentThread() == m_radio->thread()) {
        m_radio->abortTxProducerInputs(m_producer, m_source);
    }
}

std::shared_ptr<TxController> TxController::captureInputScope(const std::shared_ptr<TxController>& source)
{
    if (!source || !source->valid() || source->m_nativeDevice) { return {}; }
    if (source->m_scopeOwner) { return source; }
    return captureInputScope(source, source->m_producer.request());
}

std::shared_ptr<TxController> TxController::captureInputScope(const std::shared_ptr<TxController>& source,
                                                           const TxCoordinator::Request& input)
{
    if (!source || !source->valid() || !source->m_producer.ownsRequest(input)) { return {}; }
    // A view has exactly one original input; never relabel it or create a
    // second level whose held inputs would no longer live on the producer.
    if (source->m_scopeOwner) {
        return input.sameRequest(source->m_scopeInput) ? source : nullptr;
    }
    return std::make_shared<TxController>(InputScopeTag{}, source, input);
}

bool TxController::sameController(const std::shared_ptr<TxController>& other) const
{
    return other && m_producer.sameProducer(other->m_producer);
}

bool TxController::valid() const
{
    return m_radio && QThread::currentThread() == m_radio->thread()
        && (m_nativeDevice ? m_producer.valid() : m_connection.valid())
        && (!m_scopeOwner || m_scopeInput.valid());
}

bool TxController::originalSessionCurrent() const
{
    return m_radio && QThread::currentThread() == m_radio->thread()
        && m_connection.originalSessionCurrent();
}

bool TxController::belongsTo(const RadioModel* radio) const
{
    return m_radio && m_radio == radio;
}

std::size_t TxController::index(Activity activity)
{
    switch (activity) {
    case Activity::Mox: return 0;
    case Activity::Tune: return 1;
    case Activity::Atu: return 2;
    case Activity::CwKey: return 3;
    case Activity::CwPtt: return 4;
    case Activity::Cwx: return 5;
    }
    Q_UNREACHABLE();
}

TxController::Input TxController::capture(Activity activity)
{
    if (!valid() || m_nativeDevice) {
        return {};
    }
    Input& input = m_scopeOwner ? m_scopeOwner->m_inputs[index(activity)] : m_inputs[index(activity)];
    if (!input.m_request.valid()) {
        input.m_radio = m_radio;
        input.m_request = m_scopeOwner ? m_scopeInput.derive() : m_producer.request();
        input.m_activity = activity;
        input.m_source = m_source;
    }
    return input;
}

TxController::Input TxController::current(Activity activity) const
{
    const Input input = m_scopeOwner ? m_scopeOwner->m_inputs[index(activity)] : m_inputs[index(activity)];
    if (m_scopeOwner && !input.m_request.sameInputEpoch(m_scopeInput)) { return {}; }
    return input;
}

TxController::Input TxController::captureProgram(Activity activity)
{
    Input input;
    if (valid() && !m_nativeDevice) {
        input.m_radio = m_radio;
        input.m_request = m_scopeOwner ? m_scopeInput : m_producer.request();
        input.m_activity = activity;
        input.m_source = m_source;
    }
    return input;
}

bool TxController::hasWork() const
{
    if (!m_radio || QThread::currentThread() != m_radio->thread()) {
        return false;
    }
    return m_radio->txProducerHasWork(m_producer);
}

void TxController::setAdmissionObserver(std::function<void()> observer)
{
    if (m_radio) {
        m_radio->setTxProducerAdmissionObserver(m_producer, std::move(observer));
    }
}

void TxController::invalidate()
{
    if (m_scopeOwner) {
        if (m_invalidating) { return; }
        m_invalidating = true;
        Input root;
        root.m_radio = m_radio;
        root.m_request = m_scopeInput;
        const auto inputs = m_scopeOwner->m_inputs;
        for (Activity activity : {Activity::Cwx, Activity::CwKey, Activity::CwPtt,
                                  Activity::Atu, Activity::Tune, Activity::Mox}) {
            root.m_activity = activity;
            root.abort();
        }
        for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
            if (it->m_request.derivedFrom(root.m_request)) {
                it->abort();
            }
        }
        return;
    }
    // The producer fence is atomic. Engine cleanup remains owner-thread only.
    m_producer.invalidate();
    if (!m_radio || QThread::currentThread() != m_radio->thread() || m_invalidating) {
        return;
    }
    m_invalidating = true;
    const auto inputs = std::exchange(m_inputs, {}); // retain normal reported tails during cleanup
    m_radio->abortTxProducerInputs(m_producer, m_source);
    (void)inputs;
}

bool TxController::Input::valid() const
{
    return m_radio && QThread::currentThread() == m_radio->thread() && m_request.valid();
}

bool TxController::Input::belongsTo(const RadioModel* radio) const
{
    return m_radio && m_radio == radio;
}

bool TxController::Input::belongsTo(const CwxModel* model) const
{
    return m_radio && &m_radio->cwxModel() == model;
}

bool TxController::Input::active() const
{
    return m_radio && QThread::currentThread() == m_radio->thread()
        && m_radio->producerRequestHasWork(m_request);
}

bool TxController::Input::start(bool twoTone) const
{
    if (!valid()) {
        return false;
    }
    bool accepted = false;
    const TxCoordinator::Request request = m_request;
    switch (m_activity) {
    case Activity::Mox:
        accepted = m_radio->requestProducerPttOn(request, m_source);
        break;
    case Activity::Tune:
        accepted = m_radio->requestProducerTune(request, true, twoTone);
        break;
    case Activity::Atu:
        accepted = m_radio->requestProducerAtu(request, true);
        break;
    case Activity::CwKey:
    case Activity::CwPtt:
        accepted = m_radio->requestProducerCw(request, true, m_activity == Activity::CwPtt);
        break;
    case Activity::Cwx:
        return false;
    }
    return accepted;
}

bool TxController::Input::send(const QString& text, std::function<void()> admitted) const
{
    if (!valid() || m_activity != Activity::Cwx) {
        return false;
    }
    const TxCoordinator::Request request = m_request;
    const QString message = text;
    return m_radio->requestProducerCwx(request, message, std::move(admitted));
}

bool TxController::Input::sendMacro(int index, std::function<void()> admitted) const
{
    if (!valid() || m_activity != Activity::Cwx || index < 1 || index > 12) { return false; }
    const TxCoordinator::Request request = m_request;
    return m_radio->requestProducerCwx(request, {}, std::move(admitted), index);
}

bool TxController::Input::sendChar(const QString& text) const
{
    if (!valid() || m_activity != Activity::Cwx) { return false; }
    const TxCoordinator::Request request = m_request;
    return m_radio->requestProducerCwx(request, text, {}, 0, true);
}

void TxController::Input::stop() const
{
    if (!m_radio || QThread::currentThread() != m_radio->thread()) {
        return;
    }
    const TxCoordinator::Request request = m_request;
    switch (m_activity) {
    case Activity::Mox: m_radio->requestProducerPttOff(request, m_source); break;
    case Activity::Tune: (void)m_radio->requestProducerTune(request, false); break;
    case Activity::Atu: (void)m_radio->requestProducerAtu(request, false); break;
    case Activity::Cwx: m_radio->abortProducerCwx(request); break;
    case Activity::CwKey:
    case Activity::CwPtt:
        (void)m_radio->requestProducerCw(request, false, m_activity == Activity::CwPtt);
        break;
    }
}

void TxController::Input::abort() const
{
    if (!m_radio || QThread::currentThread() != m_radio->thread()) {
        return;
    }
    if (m_activity == Activity::Mox) {
        const TxCoordinator::Request request = m_request;
        m_radio->abortProducerPtt(request, m_source);
    } else {
        stop();
    }
}

TxCoordinator::Context TxController::Input::media() const
{
    return valid() ? m_radio->captureTxMedia(m_request) : TxCoordinator::Context{};
}

bool TxController::Input::bypassAtu() const
{
    const TxCoordinator::Request request = m_request;
    return m_activity == Activity::Atu && m_radio && QThread::currentThread() == m_radio->thread()
        && m_radio->requestProducerAtuBypass(request);
}

TxController::Input TxController::Input::derive() const
{
    Input input = *this;
    input.m_request = valid() ? m_request.derive() : TxCoordinator::Request{};
    return input;
}

} // namespace AetherSDR
