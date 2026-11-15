#pragma once

#include "core/ThemeManager.h"
#include "core/AppSettings.h"
#include "gui/PeripheralAuthStore.h"
#include "gui/PeripheralConnectionSource.h"

#include <QLineEdit>
#include <QCoreApplication>
#include <functional>

namespace AetherSDR {

// Retry an unchanged discovered endpoint without turning it into a permanent
// manual override. Editing the address or port is explicit manual intent.
inline void connectPeripheralTarget(QLineEdit* address, const QString& ipKey,
                                    const QString& portKey, quint16 port,
                                    const std::function<void(const QString&, quint16)>& connectFn)
{
    const QString host = address->text().trimmed();
    AppSettings& settings = AppSettings::instance();
    const bool discoveryRetry = settings.value(ipKey, QString()).toString().trimmed().isEmpty()
        && host == address->property("peripheralDiscoveredHost").toString()
        && port == address->property("peripheralDiscoveredPort").toUInt();
    if (!discoveryRetry) {
        settings.setValue(ipKey, host);
        settings.setValue(portKey, QString::number(port));
        settings.save();
    }
    connectFn(host, port);
}

// Keep the pending-code UI state aligned with a target change that may
// synchronously discard the previous attempt's code. The caller refreshes what
// Setup shows afterwards.
inline void connectPeripheralWithCode(QLineEdit* edit, PeripheralDeviceStatus& status,
                                      const QString& host, quint16 port,
                                      const std::function<void(const QString&, quint16)>& connectFn,
                                      const std::function<void(const QString&)>& setCodeFn)
{
    // A revealed vault value is display-only. Reconnect still asks the actual
    // peer for its endpoint-bound credential after AUTH; never replay this text.
    const bool savedCodeShown = edit && edit->property("peripheralSavedCodeRevealed").toBool();
    const QString newCode = edit && !savedCodeShown ? edit->text() : QString();
    if (savedCodeShown) {
        edit->setProperty("peripheralSavedCodeRevealed", false);
        edit->clear();
        edit->setEchoMode(QLineEdit::Password);
    }
    status.discardedAuthCode = false;
    if (!newCode.isEmpty()) {
        if (!PeripheralAuthStore::validCode(newCode)) {
            status.attention = PeripheralDeviceStatus::Attention::InvalidCode;
            status.message = QCoreApplication::translate(
                "RadioSetupDialog", "Invalid authorization code");
            return;
        }
        status.pendingAuthCode = false;
        status.attention = PeripheralDeviceStatus::Attention::None;
        status.message.clear();
        status.note.clear();
        edit->clear();
        connectFn(host, port);
        status.discardedAuthCode = false;
        status.pendingAuthCode = true;
        setCodeFn(newCode);
    } else {
        status.pendingAuthCode = false;
        status.attention = PeripheralDeviceStatus::Attention::None;
        status.message.clear();
        status.note.clear();
        // The peer requests a saved code only when it challenges this socket.
        connectFn(host, port);
        setCodeFn(QString());
    }
}

} // namespace AetherSDR
