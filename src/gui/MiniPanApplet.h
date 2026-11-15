#pragma once

// MiniPanApplet — K4-style narrow scope as an applet, because the menu bar is
// absent in Minimal Mode, whose UI is the applet panel (#4562). Float-out,
// always-on-top (#2430), geometry and close==hide come from the container
// framework. A VIEW: no pan or slice of its own; MainWindow re-slices the
// active pan's FFT to ±5/±10 kHz centred on the PASSBAND
// (MiniPan::passbandCenterOffsetHz) and drives it through the setters. Holds no
// radio references; reports feedWanted() and spanChanged(). The span setting
// lives in core/MiniPanSettings.h.

#include <QWidget>

namespace AetherSDR {

class MiniPanScope;

class MiniPanApplet : public QWidget {
    Q_OBJECT
public:
    explicit MiniPanApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Driven by MainWindow from the followed VFO slice.
    void setVfoMhz(double mhz);           // carrier: readout + hairline. 0 → placeholder
    void setSpanKHz(double kHz);
    void setPassbandHz(int lowHz, int highHz);   // Hz from the carrier; its centre
                                                 // is what the view centres on

    MiniPanScope* scope() const { return m_scope; }
    double spanKHz() const { return m_spanKHz; }
    double spanMhz() const { return m_spanKHz / 1000.0; }   // width of the re-sliced window

signals:
    // Shown or hidden — by the tray button, a float, a dock, or the container's
    // close button alike. MainWindow starts/stops consuming pan frames on this,
    // so a hidden applet costs nothing per frame.
    void feedWanted(bool wanted);
    void spanChanged(double kHz); // user picked ±5/±10 kHz — next frame re-slices

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;   // ±5/±10 kHz

private:
    void applySpanKHz(double kHz, bool persistAndEmit);

    MiniPanScope* m_scope{nullptr};

    double  m_vfoMhz{0.0};
    double  m_spanKHz{10.0};   // ±5 kHz default (10 kHz total span)
};

} // namespace AetherSDR
