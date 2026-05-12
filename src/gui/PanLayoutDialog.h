#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QVBoxLayout;
class QWidget;

namespace AetherSDR {

// Layout ID encoding:
//   "1"    — single pan (full)
//   "2v"   — 2 pans vertical (A / B)
//   "2h"   — 2 pans horizontal (A | B)
//   "2h1"  — 2 top + 1 bottom (A|B / C)
//   "12h"  — 1 top + 2 bottom (A / B|C)
//   "2x2"  — 2×2 grid (A|B / C|D)
//   "3h2"  — 3 top + 2 bottom (A|B|C / D|E)
//   "2x3"  — 3 rows of 2 (A|B / C|D / E|F)
//   "4h3"  — 4 top + 3 bottom (A|B|C|D / E|F|G)
//   "2x4"  — 4 rows of 2 (A|B / C|D / E|F / G|H)

struct PanLayout {
    QString id;
    QString label;
    int panCount;
    // Rows: each row is a list of cell widths (1=full, 2=half)
    // e.g. {{2,2},{1}} = two half-width on top, one full on bottom
    QVector<QVector<int>> rows;
};

class PanLayoutDialog : public QDialog {
    Q_OBJECT

public:
    explicit PanLayoutDialog(int maxPans, const QString& currentLayout,
                             QWidget* parent = nullptr);

    QString selectedLayout() const { return m_selected; }
    void setFramelessMode(bool on);

private:
    void buildUI(int maxPans, const QString& currentLayout);
    QString m_selected;
    QWidget* m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
};

} // namespace AetherSDR
