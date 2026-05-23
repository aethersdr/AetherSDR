#pragma once

#include "core/ClientEq.h"
#include <QWidget>

class QHBoxLayout;

namespace MasterSDR {

// Bottom-of-editor strip: one column per active band, stacking frequency
// (Hz), gain (dB), and Q as text labels in the band's palette colour.
// Selected band gets a boxed outline around the gain value — the
// Logic-Pro-style "this is what you're tweaking" affordance.
//
// Left-click selects the band.  Right-click on a column opens a context
// menu offering numeric entry for Frequency / Gain / Q (issue #2655).
// Numeric writes go straight through ClientEq::setBand() — the same
// path canvas drags use — and emit bandEdited() so the host panel can
// persist + redraw.
class ClientEqParamRow : public QWidget {
    Q_OBJECT

public:
    explicit ClientEqParamRow(QWidget* parent = nullptr);

    void setEq(ClientEq* eq);

signals:
    void bandSelected(int idx);
    // Fired when the user commits a numeric value via the column's
    // right-click context menu.  Host wiring connects this to
    // saveClientEqSettings() + canvas update().
    void bandEdited(int idx);

public slots:
    void refresh();           // rebuild columns to match current band count
    void refreshValues();     // update text without re-laying-out (drag path)
    void setSelectedBand(int idx);

private:
    class Column;  // implemented in the .cpp

    void rebuild();

    ClientEq*    m_eq{nullptr};
    QHBoxLayout* m_layout{nullptr};
    int          m_selectedBand{-1};
};

} // namespace MasterSDR
