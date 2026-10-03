#pragma once

#include <QList>
#include <QString>

#include "core/RadioDiscovery.h"  // RadioInfo

class QObject;

namespace AetherSDR {

// Engine-side interface for the bridge's connect/disconnect/dialog verbs.
// AutomationServer (libaethercore) can't include gui/ headers but must drive the
// real connection UI; ConnectionPanel implements this. The implementor is a
// QObject: deferred verbs guard with QPointer<QObject>(asQObject()).
class IConnectionAutomation {
public:
    virtual ~IConnectionAutomation() = default;

    // Discovered local radios, for the `connect list`/`connect local` verbs.
    virtual QList<RadioInfo> automationLocalRadios() const = 0;

    // Drive the normal connect path. Return false + fill *error on failure.
    virtual bool automationConnectLocalSerial(const QString& serial,
                                              QString* error = nullptr) = 0;
    // family selects the wire protocol to probe: "flex" (TCP/4992) or "hl2"
    // (HPSDR Protocol 1 discovery on UDP/1024). Empty keeps whatever the
    // connect dialog's radio-type selector is currently set to.
    virtual bool automationConnectByIp(const QString& hostOrIp,
                                       const QString& family = QString(),
                                       QString* error = nullptr) = 0;
    virtual bool automationDisconnect(QString* error = nullptr) = 0;

    // Connect-dialog visibility (fallback when no invokable dialog host is set).
    virtual bool automationDialogVisible() const = 0;
    virtual void automationSetDialogVisible(bool visible) = 0;

    // The implementing object, for QPointer lifetime guarding across the
    // deferred (QTimer::singleShot) calls the verbs schedule.
    virtual QObject* asQObject() = 0;
};

}  // namespace AetherSDR
