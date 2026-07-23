#pragma once

// AetherClock strip applet: alignment scope (dominant) + status row +
// collapsed settings drawer, visually modeled on WaveApplet. The applet
// reads/writes the AetherClock MODEL + engine ACTION surface only — no DSP,
// no radio access, no DAX registration of any kind; engine/model
// construction and the DAX-provider wiring live in
// MainWindow_AetherClock.cpp.
//
// Slice binding: the applet listens on AppletPanel::setSlice forwarding —
// the user picks the listening slice by strip selection; the applet never
// creates or grabs a slice.

#include <QDateTime>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;
class QTextEdit;
class QTimer;
class GuardedComboBox;

namespace AetherSDR {

class AetherClockEngine;
class AetherClockModel;
class ClockAlignmentWidget;
class SliceModel;
struct ClockDiagnostics;

class AetherClockApplet : public QWidget {
    Q_OBJECT

public:
    explicit AetherClockApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Wiring entry (called from MainWindow_AetherClock.cpp after engine +
    // model construction). Connects model change signals → status row,
    // engine alignmentFrame → scope, and enables the action controls.
    // Detaches any previously attached pair.
    void attach(AetherClockEngine* engine, AetherClockModel* model);

    ClockAlignmentWidget* alignmentWidget() const { return m_scope; }

public slots:
    // AppletPanel::setSlice forwarding (per-applet, explicit).
    void setSlice(SliceModel* slice);

private:
    void buildUi();
    void buildSettingsDrawer();
    void setSettingsExpanded(bool expanded);
    void updateStartStopUi();
    void applyPresetSelection();  // combo → engine applyStationPreset (selected slice)
    // Recompute the DAX chooser index, DAX display text+style, and the no-DAX
    // warning visibility from current (engine running, bound slice, selected
    // slice) state.
    void refreshDaxUi();

    // Trust/time surfaces. WWV lands one decode per 60 s frame, so the raw
    // decodedUtc reads up to a minute stale; refreshTrustAndTime is the 1 Hz
    // display-side extrapolator (UTC readout + trust line + time-dependent
    // tooltips) and never touches the engine/model.
    void updateDecodeAnchor();     // capture (anchorUtc, anchorHostMs) on a decode
    void refreshTrustAndTime();    // 1 Hz tick: UTC readout + trust line + dyn tips
    void updateTickTimer();        // run the tick only while running AND anchored
    void updateDynamicTooltips();  // LED + UTC tooltips (state / quality / age)
    void applyStaticTooltips();    // one-shot fixed tooltips for every widget

    // Bind m_boundSlice to the strip selection at engine start/switch and watch
    // its DAX channel + frequency for the run. Sole binder — Start button and
    // the running station-switch both call it (no duplicated bind block).
    void bindAndWatchBoundSlice();
    // Tuned-away banner: shown while running when the bound slice's dial has
    // drifted off the configured preset's expected dial (VFO spun away).
    void refreshTuneWarning();

    // WS-7 acquisition telemetry surfaces (PRD-C). The funnel row + verdict
    // line render the model's ClockDiagnostics snapshot for the whole run —
    // full green when Locked (Ozy field review 2026-07-22 superseded the
    // earlier collapse-on-lock); the debug pane is the read-only "raw nerd
    // info" feed in the settings drawer (AetherModem mechanics, SupportDialog
    // token treatment).
    void refreshFunnel();          // diagnosticsChanged → stages + verdict
    QString verdictText(const ClockDiagnostics& d) const;  // §6 heuristic — measured only
    void setDebugExpanded(bool expanded);
    void appendDebugLine(const QString& tag, const QString& msg);

    QPointer<AetherClockEngine> m_engine;
    QPointer<AetherClockModel> m_model;
    QPointer<SliceModel> m_slice;       // strip-selected listening slice
    QPointer<SliceModel> m_boundSlice;  // slice the running engine is bound to

    ClockAlignmentWidget* m_scope{nullptr};
    QLabel* m_utcValue{nullptr};      // decoded UTC, monospace
    QLabel* m_offsetValue{nullptr};   // signed offset, glanceable
    QLabel* m_lockLed{nullptr};       // state-colored LED dot
    QLabel* m_stationTag{nullptr};    // WWV / WWVH / WWVB / --
    QLabel* m_trustLine{nullptr};     // "q<quality> · <age>" decode-trust readout
    QLabel* m_boundSliceTag{nullptr}; // "▸<letter>" bound slice, running only
    QLabel* m_daxWarning{nullptr};    // amber no-DAX-channel warning, under status row
    QLabel* m_tuneWarning{nullptr};   // amber tuned-away warning, under status row
    QWidget* m_funnelRow{nullptr};    // WS-7 five-stage acquisition funnel (pre-lock)
    QLabel* m_stageCells[5]{};        // Car / Tick / Frm / Dec / Vote cells
    QLabel* m_verdictLine{nullptr};   // plain-English verdict, terse tech tooltip
    QPushButton* m_debugToggle{nullptr};  // "▸ Debug" toggle inside the drawer
    QTextEdit* m_debugLog{nullptr};   // read-only scrolling diagnostics pane
    QFrame* m_settingsDrawer{nullptr};
    QPushButton* m_drawerToggle{nullptr};
    GuardedComboBox* m_presetCombo{nullptr};
    GuardedComboBox* m_daxCombo{nullptr};  // DAX chooser for the selected slice
    QPushButton* m_tuneButton{nullptr};
    QPushButton* m_startStopButton{nullptr};
    QLabel* m_presetNote{nullptr};  // per-preset dial note (+ WWVB AGC note)

    bool m_updatingDaxFromModel{false};  // guard the DAX combo↔model echo loop
    QMetaObject::Connection m_sliceDaxConn;        // selected slice daxChannelChanged
    QMetaObject::Connection m_boundSliceDaxConn;   // bound slice daxChannelChanged (running)
    QMetaObject::Connection m_boundSliceFreqConn;  // bound slice frequencyChanged (running)

    QTimer* m_tickTimer{nullptr};  // 1 Hz UTC/trust extrapolation tick
    QDateTime m_anchorUtc;         // last decoded UTC (display anchor; invalid = none)
    qint64 m_anchorHostMs{0};      // host clock ms captured at the anchor decode

    // Preset the running decoder is actually on (set at every start/switch),
    // for the tuned-away dial math + banner text.
    double m_runningCarrierMHz{0.0};
    QString m_runningPresetLabel;

    // WS-7 funnel/verdict working state (display-side only).
    int m_lastSecondOfFrame{-1};      // newest alignmentFrame secondOfFrame
    qint64 m_stage1FailSinceMs{0};    // host ms since carrier stage started failing
    qint64 m_stage2FailSinceMs{0};    // host ms since timing stage started failing
    QVector<double> m_qualityTrend;   // last 3 voteQuality samples (verdict trend)
    int m_lastLoggedFrames{-1};       // VOTE debug-line change detection
    quint8 m_lastLoggedRefusal{0};
};

} // namespace AetherSDR
