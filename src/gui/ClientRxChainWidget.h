#pragma once

#include "core/AudioEngine.h"

#include <QVector>
#include <QWidget>

namespace MasterSDR {

// Visual RX DSP signal chain.  Paints a horizontal strip:
//
//   [RADIO]→[DSP]→[RX EQ]→[GATE]→[COMP]→[TUBE]→[PUDU]→[SPEAK]
//
// Three of the eight tiles are status-only (non-interactive):
//   - RADIO  — green when PC Audio (standard SSB stream) is enabled
//   - DSP    — green when any client-side NR (NR4 / DFNR / BNR) is on
//   - SPEAK  — green when MasterSDR's audio output is unmuted
//
// The remaining five (RX EQ / GATE / COMP / TUBE / PUDU) are user-
// controllable DSP stages.  Phase 0 ships them as greyed "coming
// soon" placeholders with no interactivity; later phases enable
// click-to-bypass, double-click-to-edit, and drag-to-reorder.
class ClientRxChainWidget : public QWidget {
    Q_OBJECT

public:
    explicit ClientRxChainWidget(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);

    // Status-tile inputs.  Each setter is idempotent (no repaint when
    // unchanged) so it's safe to call from a signal handler at any
    // frequency.
    void setPcAudioEnabled(bool on);
    // DSP tile state — the label rotates to show the active module's
    // short name (e.g. "NR4", "DFNR", "BNR").  Empty label falls back
    // to the generic "DSP" placeholder.  Tile greens whenever active is
    // true regardless of label.
    void setClientDspActive(bool on, const QString& label = QString());
    void setOutputUnmuted(bool on);

signals:
    // Emitted when the user double-clicks an implemented stage tile —
    // MainWindow maps this to opening the right editor in RX mode.
    void editRequested(AudioEngine::RxChainStage stage);
    // Emitted when the user double-clicks the DSP status tile —
    // MainWindow opens the full MasterDsp Settings dialog.
    void dspEditRequested();
    // Emitted when single-click re-enables NR2 from the AppSettings-
    // persisted LastClientNr — MainWindow runs FFTW wisdom prep before
    // flipping the engine on (#2275).
    void nr2EnableWithWisdomRequested();
    // Emitted when the user single-clicks an implemented stage tile —
    // toggles the stage's enabled state.  ClientChainApplet listens so
    // it can refresh BYPASS button visuals.
    void stageEnabledChanged(AudioEngine::RxChainStage stage, bool enabled);
    // Emitted after a successful drag-reorder.  AudioEngine has the
    // new order by this point — the signal informs MainWindow so it
    // can mirror the order onto the applet tile stack.
    void chainReordered();

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dragMoveEvent(QDragMoveEvent* ev) override;
    void dragLeaveEvent(QDragLeaveEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    QSize sizeHint() const override;

private:
    enum class TileKind : uint8_t {
        StatusRadio,
        StatusDsp,
        StatusSpeak,
        Stage,            // user DSP stage (RxChainStage)
    };

    struct BoxRect {
        TileKind                  kind;
        AudioEngine::RxChainStage stage{AudioEngine::RxChainStage::None};
        QRectF                    rect;
    };
    QVector<BoxRect> m_boxes;

    void rebuildLayout();
    int  hitTest(const QPointF& pos) const;        // index into m_boxes, or -1
    int  dropInsertIndex(const QPointF& pos) const; // index into the user-stage list
    bool isStageImplemented(AudioEngine::RxChainStage s) const;
    bool isStageBypassed(AudioEngine::RxChainStage s) const;
    void toggleStageBypass(int boxIdx);

    AudioEngine* m_audio{nullptr};
    // Deferred single-click → toggle-bypass timer.  Single click fires
    // after QApplication::doubleClickInterval() so a genuine double
    // click (editor-open) can cancel it before it fires.  Same UX as
    // ClientChainWidget on the TX side.
    class QTimer* m_clickTimer{nullptr};
    int           m_pendingClickIdx{-1};
    // Drag state — tracks the press point and the drag target once
    // the mouse has moved far enough to distinguish click from drag.
    QPoint m_pressPos;
    int    m_pressIndex{-1};
    int    m_dropIndex{-1};      // where the drag is currently hovering
    bool         m_pcAudioOn{false};
    bool         m_dspActive{false};
    QString      m_dspLabel;          // active module's short name; empty → "DSP"
    bool         m_outputUnmuted{true};
};

} // namespace MasterSDR
