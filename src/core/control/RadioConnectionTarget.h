#pragma once

#include "core/discovery/RadioDiscoverySource.h"

#include <QObject>

#include <memory>

namespace AetherSDR {
class RadioModel;

namespace control {

// Trusted engine seam. No wire parsing, raw commands, credentials, or TX verbs.
// Implementations reserve the connection synchronously before starting work and
// remain non-idle until cancellation/teardown has actually completed.
class RadioConnectionTarget : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Connecting, Connected, Disconnecting };
    using QObject::QObject;
    ~RadioConnectionTarget() override = default;
    [[nodiscard]] virtual State state() const = 0;
    [[nodiscard]] virtual QString errorCode() const = 0;
    [[nodiscard]] virtual bool supports(const DiscoveredRadio& radio) const = 0;
    virtual void connectRadio(const DiscoveredRadio& radio) = 0;
    virtual void disconnectRadio() = 0;
    // Local startup intent only. This is deliberately absent from radio.connect
    // and the public catalogue; account identity comes from trusted host config.
    virtual bool connectConfiguredRemote(const QString&, const QString&) { return false; }

signals:
    void stateChanged();
};

// Owns only connection lifecycle, not the model. The model must outlive it.
// Native RadioInfo translation stays below the vendor seam.
[[nodiscard]] std::unique_ptr<RadioConnectionTarget> makeModelRadioConnectionTarget(
    RadioModel* radio);

} // namespace control
} // namespace AetherSDR
