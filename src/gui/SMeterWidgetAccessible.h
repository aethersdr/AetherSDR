#pragma once

#include <QAccessibleWidget>

namespace AetherSDR {

// Accessibility adapter for the custom-painted S-meter. Keeping the adapter
// separate makes the non-visual value contract explicit and discoverable by
// the GUI accessibility check.
class SMeterWidgetAccessible : public QAccessibleWidget {
public:
    explicit SMeterWidgetAccessible(QWidget* widget);
    QString text(QAccessible::Text textType) const override;
};

QAccessibleInterface* sMeterAccessibleFactory(const QString& key, QObject* object);

} // namespace AetherSDR
