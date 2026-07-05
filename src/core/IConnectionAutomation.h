#pragma once

#include <QList>
#include <QString>

#include "core/RadioDiscovery.h"  // RadioInfo

class QObject;

namespace AetherSDR {

// Engine-side interface for the automation bridge's connect/disconnect verbs.
//
// AutomationServer lives in libaethercore (aetherd RFC step 1) and must not
// include any gui/ header, but the connect/disconnect/dialog verbs still need
// to drive the real connection UI so automation exercises the same
// MainWindow/RadioModel path a human does. The GUI's ConnectionPanel
// implements this interface; the engine holds only an IConnectionAutomation*.
//
// Lifetime: the implementor is a QObject (ConnectionPanel is a QWidget). Verbs
// that defer work onto the GUI event loop guard against destruction with
// QPointer<QObject>(asQObject()) — hence asQObject().
class IConnectionAutomation {
public:
    virtual ~IConnectionAutomation() = default;

    // Discovered local radios, for the `connect list`/`connect local` verbs.
    virtual QList<RadioInfo> automationLocalRadios() const = 0;

    // Drive the normal connect path. Return false + fill *error on failure.
    virtual bool automationConnectLocalSerial(const QString& serial,
                                              QString* error = nullptr) = 0;
    virtual bool automationConnectByIp(const QString& hostOrIp,
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
