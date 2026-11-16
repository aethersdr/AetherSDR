#include "AetherPDMeasurementExportDialog.h"

#include <QSaveFile>
#include <QStringList>
#include <utility>

namespace AetherSDR {
AetherPDMeasurementExportDialog::AetherPDMeasurementExportDialog(
    QByteArray json, QByteArray csv, QByteArray png, QWidget* parent)
    : QFileDialog(parent, tr("Export AetherPD measurement"))
{
    setObjectName(QStringLiteral("pdMeasureExportDialog"));
    setAcceptMode(QFileDialog::AcceptSave);
    const QString csvFilter = tr("CSV measurement (*.csv)");
    const QString pngFilter = tr("PNG spectrum (*.png)");
    QStringList filters{tr("JSON measurement (*.json)"), csvFilter};
    if (!png.isEmpty()) { filters.append(pngFilter); }
    setNameFilters(filters);
    setDefaultSuffix(QStringLiteral("json"));
    connect(this, &QFileDialog::filterSelected, this, [this, csvFilter, pngFilter](const QString& filter) {
        setDefaultSuffix(filter == pngFilter ? QStringLiteral("png")
            : filter == csvFilter ? QStringLiteral("csv") : QStringLiteral("json"));
    });
    connect(this, &QFileDialog::fileSelected, this,
        [this, json = std::move(json), csv = std::move(csv), png = std::move(png), csvFilter, pngFilter](const QString& path) {
            const QByteArray& selected = selectedNameFilter() == pngFilter ? png
                : selectedNameFilter() == csvFilter ? csv : json;
            QSaveFile file(path);
            const bool saved = !selected.isEmpty() && file.open(QIODevice::WriteOnly)
                && file.write(selected) == selected.size() && file.commit();
            emit exportCompleted(saved);
        });
}
} // namespace AetherSDR
