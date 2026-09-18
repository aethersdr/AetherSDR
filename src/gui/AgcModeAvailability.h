#pragma once

#include <QComboBox>
#include <QStandardItemModel>
#include <QStringList>

namespace AetherSDR {

inline void setAgcModeAvailability(QComboBox* combo, const QStringList& modes)
{
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    if (!model) {
        return;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (QStandardItem* item = model->item(i)) {
            const bool available = modes.contains(combo->itemText(i).toLower());
            item->setEnabled(available);
            item->setToolTip(available ? QString()
                : QObject::tr("This AGC mode is unavailable through the current radio backend"));
        }
    }
}

inline bool currentAgcModeAvailable(const QComboBox* combo)
{
    const QModelIndex index = combo->model()->index(combo->currentIndex(), combo->modelColumn());
    return index.isValid() && (combo->model()->flags(index) & Qt::ItemIsEnabled);
}

} // namespace AetherSDR
