#pragma once

#include "core/ClientEq.h"
#include <QWidget>

class QHBoxLayout;

namespace AetherSDR {

// Bottom-of-editor strip: one column per active band showing freq (Hz), gain
// (dB) and Q in the band's colour; the selected band's gain is boxed.
// Left-click selects; right-click offers numeric entry (#2655). Numeric writes go
// through ClientEq::setBand() (same path as canvas drags) and emit bandEdited()
// so the host can persist and redraw.
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

} // namespace AetherSDR
