#include "DStarAccessibility.h"

#include <QAccessible>
#include <QCoreApplication>
#include <QLabel>

namespace AetherSDR {

void updateDStarSliceStateLabel(QLabel* label, const QString& sliceText)
{
    if (!label) {
        return;
    }

    const QString accessibleName = QCoreApplication::translate(
        "DStarModemPage", "D-STAR slice: %1").arg(sliceText);
    if (label->text() == sliceText
        && label->accessibleName() == accessibleName) {
        return;
    }

    label->setText(sliceText);
    label->setAccessibleName(accessibleName);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

} // namespace AetherSDR
