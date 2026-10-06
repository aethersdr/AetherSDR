#include "DaxReceiveModel.h"
#ifdef HAVE_WEBSOCKETS
#include "RadioModel.h"
#include "SliceModel.h"
#include "core/TciServer.h"
#include "core/TciRxConverter.h"
#include <QPointer>
#include <QScopeGuard>
#include <array>
#include <span>

namespace AetherSDR {
struct DaxReceiveModel::State {
    struct Channel {
        TciRxBinding binding;
        PcmEpochLease epoch;
        quint64 nextSample = 0;
        std::shared_ptr<TciRxConverter> converter;
    };
    struct Pin {
        PcmEpochLease epoch;
        std::shared_ptr<std::atomic<bool>> owner;
    };
    QPointer<RadioModel> model;
    QPointer<TciServer> routing;
    std::array<Channel,8> channels;
    QHash<int,Pin> pins;
    PcmFrameGate gate;
    bool enabled = false;
    bool processing = false;
};

DaxReceiveModel::DaxReceiveModel(RadioModel& model, TciServer& routing, QObject* parent)
    : QObject(parent), m_state(std::make_unique<State>())
{
    Q_ASSERT(model.thread() == thread() && routing.thread() == thread());
    m_state->model = &model;
    m_state->routing = &routing;
    connect(&model, &RadioModel::backendSliceAudioFrameReady, this, &DaxReceiveModel::receive);
    connect(&routing, &TciServer::rxBindingsChanged, this, &DaxReceiveModel::refreshBindings);
    connect(&model, &RadioModel::capabilitiesChanged, this, &DaxReceiveModel::refreshBindings);
    connect(&model, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected) { setEnabled(false); }
    });
    connect(&routing, &QObject::destroyed, this, [this] { setEnabled(false); });
}

DaxReceiveModel::~DaxReceiveModel() = default;

void DaxReceiveModel::setEnabled(bool enabled)
{
    m_state->enabled = enabled;
    refreshBindings();
}

void DaxReceiveModel::refreshBindings()
{
    QPointer<DaxReceiveModel> self(this);
    std::array<TciRxBinding,8> next;
    if (m_state->enabled && m_state->model && m_state->routing) {
        const auto capability = m_state->model->backendCapabilities().receiveAudioExport;
        if (capability) {
            for (SliceModel* slice : m_state->model->slices()) {
                const TciRxBinding binding = m_state->routing->sliceRxBinding(slice->sliceId());
                if (binding.current() && binding.trx >= 0 && binding.trx < 8
                    && binding.trx < capability->maximumReceivers) {
                    next[binding.trx] = binding;
                }
            }
        }
    }
    for (int i = 0; i < 8; ++i) {
        State::Channel& channel = m_state->channels[i];
        if (channel.binding.alive == next[i].alive) { continue; }
        channel = {};
        channel.binding = next[i];
        const int sliceId = next[i].current() ? next[i].sliceId : -1;
        emit channelReset(i+1);
        if (!self) { return; }
        emit channelSliceChanged(i+1,sliceId);
        if (!self) { return; }
    }
}

void DaxReceiveModel::receive(int sliceId, const PcmFrame& frame)
{
    const PcmFrame input = frame;
    if (m_state->processing || !m_state->enabled || !m_state->model || !m_state->routing
        || !input.current() || input.stream().purpose != PcmPurpose::Slice
        || input.stream().sliceId != sliceId) { return; }
    const auto capability = m_state->model->backendCapabilities().receiveAudioExport;
    if (!capability || !capability->sampleRatesHz.contains(input.stream().format.sampleRateHz)) { return; }
    const TciRxBinding binding = m_state->routing->sliceRxBinding(sliceId);
    if (!binding.current() || binding.trx < 0 || binding.trx >= 8
        || binding.trx >= capability->maximumReceivers) { return; }
    const int index = binding.trx;
    State::Channel& channel = m_state->channels[index];
    if (channel.binding.alive != binding.alive) { return; }

    // A removed/replaced receiver retains a bounded tombstone while its old
    // producer is live. Neither a new owner nor a competing source can seize it.
    for (auto it = m_state->pins.begin(); it != m_state->pins.end();) {
        if (!it->epoch.current()) { it = m_state->pins.erase(it); }
        else { ++it; }
    }
    const auto pin = m_state->pins.constFind(sliceId);
    if (pin != m_state->pins.cend()) {
        if (pin->epoch.stream() != input.stream() || pin->owner != binding.alive) { return; }
    } else if (m_state->pins.size() >= qsizetype(PcmFrameGate::kMaxStreams)) { return; }
    if (!m_state->gate.accept(input)) { return; }
    m_state->pins.insert(sliceId,{input.epochLease(),binding.alive});

    m_state->processing = true;
    QPointer<DaxReceiveModel> self(this);
    const auto processing = qScopeGuard([self] { if (self) { self->m_state->processing = false; } });
    if (channel.epoch.stream() != input.stream() || channel.nextSample != input.firstSample()
        || input.discontinuity() || !channel.converter) {
        channel.epoch = input.epochLease();
        channel.converter = std::make_shared<TciRxConverter>(input.stream().format,24000);
        emit channelReset(index+1);
        if (!self || !m_state->enabled || !binding.current()
            || channel.binding.alive != binding.alive || !input.current()) { return; }
    }
    channel.nextSample = input.firstSample() + input.frameCount();
    const std::shared_ptr<TciRxConverter> converter = channel.converter;
    if (!converter || !converter->valid()) { return; }
    const auto current = [self,index,binding,input,converter] {
        return self && self->m_state->enabled && input.current() && binding.current()
            && self->m_state->channels[index].binding.alive == binding.alive
            && self->m_state->channels[index].converter == converter;
    };
    const bool accepted = converter->process(std::span(input.samples().constData(), input.samples().size()),
        [self,index,current](QVector<float> stereo) {
            if (!current()) { return false; }
            const QByteArray pcm(reinterpret_cast<const char*>(stereo.constData()),
                                 stereo.size()*qsizetype(sizeof(float)));
            emit self->audioReady(index+1,pcm);
            return current();
        });
    if (!accepted && self && channel.converter == converter) {
        channel.converter.reset();
        emit channelReset(index+1);
    }
}
}
#endif
