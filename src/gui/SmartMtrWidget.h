#pragma once

#include <QWidget>

class QLabel;

namespace AetherSDR {

// Placeholder alternative meter view shown in the VFO flag in place of the
// standard S-meter when the operator selects "SmartMTR" from the meter menu.
// No signal-processing logic yet — it simply displays the text "SmartMTR".
class SmartMtrWidget : public QWidget {
    Q_OBJECT
public:
    explicit SmartMtrWidget(QWidget* parent = nullptr);

    // The VfoWidget meter area is a size-to-current-page stack, so it adopts
    // whatever height this returns — S-Meter and SmartMTR can therefore differ
    // in height and the flag resizes when switching.  The real meter component
    // will set its own preferred height here.
    QSize sizeHint() const override;

private:
    QLabel* m_label{nullptr};
};

} // namespace AetherSDR
