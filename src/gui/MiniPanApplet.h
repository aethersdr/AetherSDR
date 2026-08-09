#pragma once

// MiniPanApplet — the K4-style narrow scope, as an applet.
//
// It is an APPLET rather than a View-menu window for one decisive reason: the
// menu bar does not exist in Minimal Mode (MainWindow::toggleMinimalMode strips
// the title bar to heartbeat + logo + restore/feature buttons), and Minimal Mode
// is the feature's headline use case. The applet panel IS the Minimal Mode UI —
// it is reparented into the central layout and shown — so the tray button is the
// only entry point reachable when the operator actually wants this. (#4562)
//
// Everything the standalone window used to hand-roll now comes from the
// container framework, which is why this file is so much smaller than the
// MiniPanWidget it replaced: float-out into a top-level window (so it still
// floats over contest-logging software), always-on-top (#2430), geometry
// persistence, and close==hide are all ContainerWidget/FloatingContainerWindow
// behaviour, driven from the standard ContainerTitleBar.
//
// The mini-pan is a VIEW, not a radio object: it creates no panadapter and no
// slice. MainWindow re-slices the FFT frames the active slice's pan is already
// streaming down to this window's +/-5 or +/-10 kHz, and drives centre/passband
// from the followed VFO through the setters below. So the applet costs a radio
// nothing to open — no pan slot, nothing to leak, no phantom slice.
//
// This widget holds NO radio/slice references (so it links into a light
// offscreen test). It reports two intents back up: feedWanted() (shown/hidden)
// and spanChanged().
//
// The only genuinely feature-owned setting left is the ±5/±10 kHz span, in
// core/MiniPanSettings.h (Constitution Principle V).

#include <QWidget>

namespace AetherSDR {

class MiniPanScope;

class MiniPanApplet : public QWidget {
    Q_OBJECT
public:
    explicit MiniPanApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Driven by MainWindow from the followed VFO slice.
    void setCenterMhz(double mhz);        // 0 → placeholder readout
    void setSpanKHz(double kHz);
    void setPassbandHz(int lowHz, int highHz);

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

    double  m_centerMhz{0.0};
    double  m_spanKHz{10.0};   // ±5 kHz default (10 kHz total span)
};

} // namespace AetherSDR
