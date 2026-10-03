#pragma once

#include "core/ThemeManager.h"
#include "gui/PeripheralAuthStore.h"

#include <QLabel>
#include <QLineEdit>
#include <QCoreApplication>
#include <functional>

namespace AetherSDR {

// Keep the pending-code UI state aligned with a target change that may
// synchronously discard the previous attempt's code.
inline void connectPeripheralWithCode(QLineEdit* edit, QLabel* status,
                                      const QString& host, quint16 port,
                                      const std::function<void(const QString&, quint16)>& connectFn,
                                      const std::function<void(const QString&)>& setCodeFn)
{
    const QString newCode = edit ? edit->text() : QString();
    status->setProperty("discardedAuthCode", false);
    if (!newCode.isEmpty()) {
        if (!PeripheralAuthStore::validCode(newCode)) {
            status->setProperty("credentialError", true);
            status->setText(QCoreApplication::translate(
                "RadioSetupDialog", "Error: invalid authorization code"));
            ThemeManager::instance().applyStyleSheet(status,
                "QLabel { color: {{color.accent.danger}}; font-size: 11px; }");
            return;
        }
        status->setProperty("pendingAuthCode", false);
        status->setProperty("credentialError", false);
        status->setProperty("credentialNote", QString());
        edit->clear();
        connectFn(host, port);
        status->setProperty("discardedAuthCode", false);
        status->setProperty("pendingAuthCode", true);
        setCodeFn(newCode);
    } else {
        status->setProperty("pendingAuthCode", false);
        status->setProperty("credentialError", false);
        status->setProperty("credentialNote", QString());
        // The peer requests a saved code only when it challenges this socket.
        connectFn(host, port);
        setCodeFn(QString());
    }
}

} // namespace AetherSDR
