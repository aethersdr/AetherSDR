#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include <QList>
#include <QVariant>
#include <functional>
#include <memory>

// Existing protocol assertions observe the production Flex encoder now, not
// raw text emitted by a model. No connection, socket peer, or radio is started.
inline std::unique_ptr<AetherSDR::FlexBackend> modelControlEncoder(
    AetherSDR::TransmitModel* model, std::function<void(const QString&)> sink)
{
    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    backend->setCommandSink(std::move(sink));
    QObject::connect(model, &AetherSDR::TransmitModel::controlRequested, backend.get(),
                     [target = backend.get()](const AetherSDR::TransmitControlRequest& request) {
        target->requestTransmitControl(request);
    });
    return backend;
}

inline std::unique_ptr<AetherSDR::FlexBackend> modelControlEncoder(
    AetherSDR::SliceModel* model, std::function<void(const QString&)> sink)
{
    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    backend->setSliceCommandSink(std::move(sink));
    QObject::connect(model, &AetherSDR::SliceModel::controlRequested, backend.get(),
                     [target = backend.get(), id = model->sliceId()](const AetherSDR::SliceControlRequest& request) {
        target->requestSliceControl(id, request);
    });
    return backend;
}

class ModelControlWireSpy : public QList<QList<QVariant>> {
public:
    template<class Model> explicit ModelControlWireSpy(Model* model)
        : m_encoder(modelControlEncoder(model, [this](const QString& command) {
            append(QList<QVariant>{command});
        })) {}
private:
    std::unique_ptr<AetherSDR::FlexBackend> m_encoder;
};
