#pragma once

// MiniPanWidget — the detachable K4-style mini-pan window (PR1: window shell).
//
// An INDEPENDENT top-level window (QWidget + Qt::Window, like FloatingContainerWindow)
// — NOT a QDialog/PersistentDialog — so it floats over third-party contest-logging
// software and survives AetherSDR being minimised. Single long-lived instance;
// close == hide (WA_DeleteOnClose stays false) so geometry/state persist cheaply and
// the View-menu toggle and the window's close button do the same thing.
//
// PR1 scope: frameless chrome + resize, geometry persistence, big frequency readout,
// and a MiniPanScope rendering an empty field (no radio feed yet). PR2 wires the
// dedicated narrow pan into MiniPanScope::updateSpectrum and drives the centre/span
// from the followed VFO. See docs/minipan-implementation.md.

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

    // Followed-VFO readout + view (PR2 drives these live).
    void setCenterMhz(double mhz);
    void setSpanKHz(double kHz);

    MiniPanScope* scope() const { return m_scope; }

    static constexpr auto kGeometryKey = "MiniPanGeometry";
    static constexpr auto kOpenKey     = "MiniPanOpen";

signals:
    void closedByUser();   // window hidden via its close button (menu should uncheck)

protected:
    void closeEvent(QCloseEvent* e) override;   // close == hide
    void showEvent(QShowEvent* e) override;      // restore geometry on first show
    void moveEvent(QMoveEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void saveGeometryToSettings() const;
    void refreshHeader();

    FramelessWindowTitleBar* m_titleBar{nullptr};
    QVBoxLayout*  m_layout{nullptr};
    QLabel*       m_freqLabel{nullptr};
    MiniPanScope* m_scope{nullptr};

    double  m_centerMhz{0.0};
    QTimer  m_saveTimer;
    bool    m_restoring{false};
    bool    m_geometryRestored{false};
    bool    m_alwaysOnTop{false};
};

} // namespace AetherSDR
