#pragma once

#include "core/TxGrantManager.h"

#include <QObject>
#include <memory>

namespace AetherSDR {
class RadioModel;
namespace control {

// Trusted, socket-free seam. IDs and credentials never reach this interface.
class TransmitControlTarget : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual TxGrantManager* grants() const = 0;
    virtual bool ready() const = 0;
    virtual bool recovering() const = 0;
    virtual bool start(const TxGrantManager::Admission& admission) = 0;
    virtual void emergencyStop() = 0;
signals:
    void radioInvalidated();
};

[[nodiscard]] std::unique_ptr<TransmitControlTarget> makeModelTransmitControlTarget(RadioModel* radio);

} // namespace control
} // namespace AetherSDR
