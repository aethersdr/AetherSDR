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
// This widget holds NO radio/slice references (so it links into a light
// offscreen test). MainWindow owns the data glue: it creates the dedicated
// narrow pan (RadioModel::createMiniPan), feeds MiniPanScope via
// panFeedSpectrumReady, and drives centre/passband from the followed VFO by
// calling the setters below. The applet reports three intents back up:
// feedWanted() (shown/hidden — the radio-side pan follows it), scopeResized()
// (debounced — MainWindow re-pushes the pan's xpixels) and spanChanged().
//
// The only genuinely feature-owned setting left is the ±5/±10 kHz span, in
// core/MiniPanSettings.h (Constitution Principle V).

#include <QWidget>
#include <QTimer>

class QLabel;

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
    double spanMhz() const { return m_spanKHz / 1000.0; }   // for the radio pan bandwidth

signals:
    // Shown or hidden — by the tray button, a float, a dock, or the container's
    // close button alike. MainWindow creates/frees the radio-side pan on this,
    // so a hidden applet never holds a pan slot.
    void feedWanted(bool wanted);
    void scopeResized();          // debounced — MainWindow re-pushes xpixels
    void spanChanged(double kHz); // user picked ±5/±10 kHz — MainWindow re-pushes bandwidth

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;   // ±5/±10 kHz

private:
    void refreshHeader();
    void applySpanKHz(double kHz, bool persistAndEmit);

    QLabel*       m_freqLabel{nullptr};
    MiniPanScope* m_scope{nullptr};

    double  m_centerMhz{0.0};
    double  m_spanKHz{10.0};   // ±5 kHz default (10 kHz total span)
    QTimer  m_xpixTimer;
};

} // namespace AetherSDR
