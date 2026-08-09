#pragma once

// MiniPanWidget — the detachable K4-style mini-pan window (presentation only).
//
// An INDEPENDENT top-level window (QWidget + Qt::Window, like FloatingContainerWindow)
// — NOT a QDialog/PersistentDialog — so it floats over third-party contest-logging
// software and survives AetherSDR being minimised. Single long-lived instance;
// close == hide (WA_DeleteOnClose stays false) so geometry/state persist cheaply and
// the View-menu toggle and the window's close button do the same thing.
//
// This widget holds NO radio/slice references (so it links into a light offscreen
// test). MainWindow owns the data glue: it creates the dedicated narrow pan
// (RadioModel::createMiniPan), feeds MiniPanScope via panFeedSpectrumReady, and drives
// centre/passband from the followed VFO by calling the setters below. The window
// reports its own two intents back up: closedByUser() (X button) and scopeResized()
// (debounced — MainWindow re-pushes the pan's xpixels from scope()->width()).
//
// Persistence lives in core/MiniPanSettings.h — ONE nested "MiniPan" object,
// never flat AppSettings keys (Constitution Principle V).

#include <QWidget>
#include <QString>
#include <QTimer>

class QLabel;
class QVBoxLayout;

namespace AetherSDR {

class MiniPanScope;
class FramelessWindowTitleBar;

class MiniPanWidget : public QWidget {
    Q_OBJECT
public:
    explicit MiniPanWidget(QWidget* parent = nullptr);

    // Runtime frameless-chrome toggle propagation (driven by MainWindow).
    void setFramelessMode(bool on);
    void setAlwaysOnTop(bool on);

    // Driven by MainWindow from the followed VFO slice.
    void setCenterMhz(double mhz);        // 0 → placeholder readout
    void setSpanKHz(double kHz);
    void setPassbandHz(int lowHz, int highHz);

    MiniPanScope* scope() const { return m_scope; }
    double spanKHz() const { return m_spanKHz; }
    double spanMhz() const { return m_spanKHz / 1000.0; }   // for the radio pan bandwidth

signals:
    void closedByUser();          // window hidden via its close button (menu should uncheck)
    void scopeResized();          // debounced — MainWindow re-pushes xpixels
    void spanChanged(double kHz); // user picked ±5/±10 kHz — MainWindow re-pushes bandwidth

protected:
    void closeEvent(QCloseEvent* e) override;   // close == hide
    void showEvent(QShowEvent* e) override;      // restore geometry on first show
    void moveEvent(QMoveEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;   // span / always-on-top

private:
    void saveGeometryToSettings() const;
    void refreshHeader();
    void applySpanKHz(double kHz, bool persistAndEmit);

    FramelessWindowTitleBar* m_titleBar{nullptr};
    QVBoxLayout*  m_layout{nullptr};
    QLabel*       m_freqLabel{nullptr};
    MiniPanScope* m_scope{nullptr};

    double  m_centerMhz{0.0};
    double  m_spanKHz{10.0};   // ±5 kHz default (10 kHz total span)
    QTimer  m_saveTimer;
    QTimer  m_xpixTimer;
    bool    m_restoring{false};
    bool    m_geometryRestored{false};
    bool    m_alwaysOnTop{false};
};

} // namespace AetherSDR
