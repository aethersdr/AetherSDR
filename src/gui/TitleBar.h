#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;
class QFrame;
class QMenuBar;
class QHBoxLayout;
class QTimer;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

namespace AetherSDR {

class PersistentDialog;

class TitleBar : public QWidget {
    Q_OBJECT

public:
    explicit TitleBar(QWidget* parent = nullptr);

    // Embed the menu bar into the left side of the title bar
    void setMenuBar(QMenuBar* mb);

    void setPcAudioEnabled(bool on);
    void setPcAudioDevices(const QString& inputDevice, const QString& outputDevice);
    void setLineoutMuted(bool muted);
    void setMasterVolume(int pct);
    void setHeadphoneVolume(int pct);
    void setOtherClientTx(bool transmitting, const QString& station);

    // Status-bar transmit timer (left of the PC Audio button). Driven by
    // RadioModel::operatorTransmitChanged — runs only for operator MOX/PTT/VOX
    // transmits, never TCI/DAX. Hidden when idle; on unkey it holds the final
    // elapsed time for 15 s then fades out.
    void setOperatorTransmitting(bool active);
    // Introspection for the automation bridge (`txtimer` verb): visible /
    // running / holding / fading / elapsedMs / text / opacity.
    QVariantMap txTimerState() const;
    void setMultiFlexStatus(int clientCount, const QStringList& names);
    void onHeartbeat();       // Call when a discovery packet arrives
    void onHeartbeatLost();   // Call when radio lost from discovery
    void setDiscovering(bool active); // Solid amber while discovering / not yet connected
    void setMinimalMode(bool on);
    void setBlinkEnabled(bool enabled); // Toggle heartbeat animation on/off
    // Set the flash color used while adaptive throttle is active (empty = restore green).
    // Ignored while the disconnected-alarm blink is running.
    void setThrottleFlashColor(const QString& hexColor);

    // Reflect applet-panel state on the dock-side icons:
    //  - visible=false: both icons dim (no active side).
    //  - visible=true:  the icon matching `left` is highlighted.
    void setAppletDockState(bool visible, bool left);

    // Highlight the pop-out icon when the applet panel is floating in its
    // own Qt::Window.
    void setAppletFloating(bool floating);

    // Windows native hit-testing uses this to expose custom title-bar gaps
    // as caption drag zones while keeping controls interactive.
    bool isSystemMoveAreaAt(const QPoint& globalPos) const;

signals:
    void pcAudioToggled(bool on);
    void masterVolumeChanged(int pct);
    void headphoneVolumeChanged(int pct);
    void lineoutMuteChanged(bool muted);
    void headphoneMuteChanged(bool muted);
    void minimalModeRequested();
    void minimalModeWindowedExitRequested();
    void multiFlexClicked();
    // Emitted when the blink setting changes (e.g. via right-click) so the
    // View menu checkbox can stay in sync.
    void blinkEnabledChanged(bool enabled);
    // Dock-side selectors — applet panel left vs right of the panadapter.
    void dockAppletLeftRequested();
    void dockAppletRightRequested();
    // Toggle applet panel between docked and floating-window mode.
    void popOutAppletRequested();

public:
    // Open the feature-request dialog.  Wired from Help → Submit your idea…
    void showFeatureRequestDialog();
    void setChildDialogsFramelessMode(bool on);

private:
    void markDragHandle(QWidget* widget);
    bool isDragHandle(QObject* obj) const;
    bool startWindowMove(QMouseEvent* ev, bool useSystemMove = true);
    bool continueWindowMove(QMouseEvent* ev);
    bool finishWindowMove(QMouseEvent* ev);
    void handleTitleDoubleClick(QMouseEvent* ev);
    void showFeatureRequestDialogImpl();
    void updatePcAudioToolTip();
    QHBoxLayout* m_hbox{nullptr};
    QMenuBar*    m_menuBar{nullptr};
    QLabel*      m_appNameLabel{nullptr};
    QLabel*      m_otherTxLabel{nullptr};
    QPushButton* m_mfBtn{nullptr};
    QPushButton* m_pcBtn{nullptr};
    QPushButton* m_speakerBtn{nullptr};
    QPushButton* m_headphoneBtn{nullptr};
    QSlider*     m_masterSlider{nullptr};
    QSlider*     m_hpSlider{nullptr};
    QLabel*      m_masterLabel{nullptr};
    QLabel*      m_hpLabel{nullptr};

    // Window-control trio (frameless mode): minimize, maximize/restore, close.
    // QLabels (not buttons) for a flat look; click is wired via eventFilter.
    QLabel*      m_minimizeLbl{nullptr};
    QLabel*      m_maximizeLbl{nullptr};
    QLabel*      m_closeLbl{nullptr};
    QLabel*      m_dockLeftLbl{nullptr};
    QLabel*      m_dockRightLbl{nullptr};
    QLabel*      m_popOutLbl{nullptr};
    QPointer<PersistentDialog> m_issueReporterDialog;
    QFrame*      m_dockSep{nullptr};
    bool         m_minimalMode{false};
    bool         m_windowMoveActive{false};
    bool         m_windowMoveUsesSystem{false};
    QPoint       m_windowMovePressGlobal;
    QPoint       m_windowMoveStartPos;

    // Heartbeat indicator
    QLabel*      m_heartbeat{nullptr};
    QTimer*      m_heartbeatOffTimer{nullptr};   // 100ms green→grey
    QTimer*      m_heartbeatAlarmTimer{nullptr}; // 500ms red/grey blink
    int          m_missedBeats{0};
    bool         m_alarmRed{false};
    bool         m_blinkEnabled{true};  // persisted via AppSettings "HeartbeatBlinkEnabled"
    bool         m_discovering{false};  // solid amber while waiting for connection
    QString      m_throttleFlashColor; // empty = default green; set while adaptive throttle is active
    QString      m_pcAudioInputDevice;
    QString      m_pcAudioOutputDevice;

    // ── Transmit timer ──────────────────────────────────────────────────────
    void         updateTxTimerText();          // repaint MM:SS / H:MM:SS from elapsed
    QString      formatTxElapsed(qint64 ms) const;
    QLabel*      m_txTimerLabel{nullptr};
    QTimer*      m_txTimerTick{nullptr};        // 5 Hz cadence while running
    QTimer*      m_txTimerHoldTimer{nullptr};   // 15s post-unkey hold, then fade
    QGraphicsOpacityEffect* m_txTimerOpacity{nullptr};
    QPropertyAnimation*     m_txTimerFade{nullptr};
    QElapsedTimer m_txElapsed;                  // wall-clock since this key-up
    qint64        m_txFrozenMs{0};              // elapsed frozen at unkey (hold/fade)
    bool          m_txTimerRunning{false};      // true between key-up and unkey

protected:
    // Drag-to-move + double-click-to-maximize for frameless main window.
    // Click hits a child first (menu bar item, button, slider) and only
    // bubbles to TitleBar when the press lands on bare background.
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void showEvent(QShowEvent* ev) override;

private:
    void updateMaximizeIcon();
    QString currentBeatColor() const;  // #20c060 or throttle color
};

} // namespace AetherSDR
