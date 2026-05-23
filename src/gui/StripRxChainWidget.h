#pragma once

#include "core/AudioEngine.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

namespace MasterSDR {

// Visual RX DSP signal chain — strip variant.  Paints a single
// horizontal row of nine boxes:
//
//   [RADIO] ─▶ [ADSP] ─▶ [AGC-T] ─▶ [EQ] ─▶ [AGC-C] ─▶ [DESS]
//                                                  ─▶ [TUBE] ─▶ [EVO] ─▶ [SPEAK]
//
// RADIO and SPEAK are status-only endpoints.  ADSP is a clickable
// launcher that opens / focuses the in-strip ADSP panel (the
// MasterDsp NR cluster).  The six middle stages
// (AGC-T / EQ / AGC-C / DESS / TUBE / EVO) are user-controllable;
// click toggles bypass, double-click opens the editor, drag
// reorders within the chain.
//
// Sibling of `StripChainWidget` (TX).  Layout / interaction
// semantics mirror the TX side; only the stage set, labels, and
// engine bindings differ.
class StripRxChainWidget : public QWidget {
    Q_OBJECT

public:
    explicit StripRxChainWidget(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);

    // Status-tile inputs.  Each setter is idempotent (no repaint
    // when unchanged) so it's safe to call from a signal handler at
    // any frequency.
    void setPcAudioEnabled(bool on);
    // ADSP tile state — `active` greens the tile.  `label` rotates
    // the active module's short name (NR2 / NR4 / DFNR / BNR);
    // empty falls back to the generic "ADSP" placeholder.
    void setClientDspActive(bool active, const QString& label = QString());
    void setOutputUnmuted(bool on);

signals:
    // Emitted on double-click of an implemented stage tile —
    // MasterialAudioStrip routes this to focus the corresponding
    // RX panel.
    void editRequested(AudioEngine::RxChainStage stage);

    // Emitted when the user clicks ADSP — MasterialAudioStrip
    // focuses the ADSP panel (or opens it if it isn't visible).
    void dspEditRequested();

    // Emitted after a successful drag-reorder.  Engine has the new
    // chain order by this point; the signal is informational so the
    // parent can refresh any dependent UI.
    void chainReordered();

    // Emitted when single-click toggles a stage's bypass.  Mirrors
    // the TX-side signal so MainWindow can keep the docked
    // ClientRxChainWidget repainted in lock-step.
    void stageEnabledChanged(AudioEngine::RxChainStage stage, bool enabled);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void contextMenuEvent(QContextMenuEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dragMoveEvent(QDragMoveEvent* ev) override;
    void dragLeaveEvent(QDragLeaveEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    QSize sizeHint() const override;

private:
    enum class TileKind : uint8_t {
        StatusRadio,   // leftmost; no chain stage
        StatusAdsp,    // clickable launcher; no chain stage
        StatusSpeak,   // rightmost; no chain stage
        Stage,         // user DSP stage (RxChainStage)
    };

    struct BoxRect {
        TileKind                  kind;
        AudioEngine::RxChainStage stage{AudioEngine::RxChainStage::None};
        QRectF                    rect;
    };
    QVector<BoxRect> m_boxes;

    void rebuildLayout();
    int  hitTest(const QPointF& pos) const;
    int  dropInsertIndex(const QPointF& pos) const;

    bool isStageImplemented(AudioEngine::RxChainStage s) const;
    bool isStageBypassed(AudioEngine::RxChainStage s) const;
    void toggleStageBypass(int boxIdx);

    // ADSP cluster (NR2/NR4/MNR/DFNR/RN2/BNR) is rendered as a
    // Stage-style tile.  Bypassed ⇔ no NR module enabled.  Toggle
    // snapshots the active set on bypass and restores it on un-bypass.
    bool isAdspBypassed() const;
    void toggleAdspBypass();

    AudioEngine* m_audio{nullptr};
    bool         m_pcAudioOn{false};
    bool         m_dspActive{false};
    QString      m_dspLabel;          // active NR module's short name; empty → "ADSP"
    QStringList  m_adspBypassSnapshot; // set of NR module names that were on at bypass-time
    bool         m_outputUnmuted{true};

    // Drag state.
    QPoint m_pressPos;
    int    m_pressIndex{-1};
    int    m_dropIndex{-1};

    // Deferred single-click → toggle-bypass timer.  Mirrors the TX
    // side: single-click fires after QApplication::doubleClickInterval()
    // so a follow-up double-click can cancel the bypass and open the
    // editor instead.
    class QTimer* m_clickTimer{nullptr};
    int           m_pendingClickIdx{-1};
};

} // namespace MasterSDR
