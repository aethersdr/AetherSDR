#pragma once

#include <QByteArray>
#include <QFileDialog>

namespace AetherSDR {
// Owns immutable export bytes so a new run cannot replace an open save operation.
class AetherPDMeasurementExportDialog final : public QFileDialog {
    Q_OBJECT
public:
    AetherPDMeasurementExportDialog(QByteArray json, QByteArray csv, QByteArray png,
                                    QWidget* parent = nullptr);
signals:
    void exportCompleted(bool saved);
};
} // namespace AetherSDR
