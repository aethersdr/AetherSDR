#pragma once

#include "PersistentDialog.h"

#include <QString>
#include <QVector>

class QWidget;

namespace AetherSDR {

// Layout IDs: "1" single; "2v" A/B; "2h" A|B; "2h1" A|B / C; "12h" A / B|C;
// "2x2" A|B / C|D; "3h2" A|B|C / D|E; "2x3" A|B / C|D / E|F;
// "4h3" A|B|C|D / E|F|G; "2x4" A|B / C|D / E|F / G|H.

struct PanLayout {
    QString id;
    QString label;
    int panCount;
    // Rows: each row is a list of cell widths (1=full, 2=half)
    // e.g. {{2,2},{1}} = two half-width on top, one full on bottom
    QVector<QVector<int>> rows;
};

class PanLayoutDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit PanLayoutDialog(int maxPans, const QString& currentLayout,
                             QWidget* parent = nullptr);

    QString selectedLayout() const { return m_selected; }

private:
    void buildUI(int maxPans, const QString& currentLayout);
    QString m_selected;
};

} // namespace AetherSDR
