#pragma once

#include <QWidget>
#include <QPointer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QVector>
#include <QStringList>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>

#include "core/KiwiSdrProtocol.h"

class QPushButton;
class ScrollableLabel;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QSlider;
class QComboBox;
class QCheckBox;
class QGraphicsOpacityEffect;
class QDoubleSpinBox;
class QGridLayout;
class QPainter;

namespace AetherSDR {

class SliceModel;
class TransmitModel;
class RadioModel;
class PhaseKnob;
class RxApplet;
class KiwiSdrManager;
class SmartMtrWidget;

// Floating VFO info panel attached to the VFO marker on the spectrum display.
// Shows slice info (antennas, frequency, signal level, filter width, TX/SPLIT)
// and tabbed sub-menus (Audio, DSP, Mode, X/RIT, DAX).
// Anchored to the left of the VFO marker; flips right when clipped.
class VfoWidget : public QWidget {
    Q_OBJECT

public:
    explicit VfoWidget(QWidget* parent = nullptr);
    ~VfoWidget() override;

    void setSlice(SliceModel* slice);
    void setAntennaList(const QStringList& ants);
    void setTransmitModel(TransmitModel* txModel);
    void setRadioModel(RadioModel* radioModel);
    void setKiwiSdrManager(KiwiSdrManager* manager);
    // Wire the SQL button + slider as a mirror of the RxApplet's 3-way
    // SQL UI (Off / Manual / Auto, manual-level cache, Auto margin).
    // Without this call, the SQL row still functions but in the old
    // 2-state on/off mode against the slice's squelchLevel only.
    void setRxApplet(RxApplet* rx);
    void setSignalLevel(float dbm);
    void setReceiveMeterReading(
        const AetherSDR::KiwiSdrProtocol::MeterReading& reading);
    // SmartMTR feeds: live mic level + separately-measured mic peak (both dBFS)
    // and global TX (MOX) state. The SmartMTR view shows mic level on this VFO's
    // TX slice while transmitting (peak marker driven by micPeak), and received
    // signal otherwise.
    void setMicLevel(float micDbfs, float micPeakDbfs);
    void setTransmitting(bool tx);

    // Split mode: call whenever TX assignment or active slice changes.
    //   isTxSlice  — this VFO's slice has tx=1
    //   splitActive — TX is assigned to a different slice than the active one
    void updateSplitBadge(bool isTxSlice, bool splitActive);

    // Flag direction hint for deconfliction.
    //   Auto/ForceLeft/ForceRight participate in the 20-px edge-clip flip:
    //   if the panel would overrun the spectrum edge, it flips to the other
    //   side so the panel stays visible.
    //   LockLeft/LockRight disable that flip and hold the requested side
    //   even if the panel overruns the edge. Used by split pairs so the
    //   RX/TX panels stay on their opposite sides instead of collapsing
    //   onto the same side when the pair is near a pan edge (#2663).
    enum FlagDir { Auto, ForceLeft, ForceRight, LockLeft, LockRight };

    // Reposition relative to VFO marker x coordinate.
    void updatePosition(int vfoX, int specTop, FlagDir dir = Auto);

    // Draw this flag's SmartMTR extremes value labels (min/max or current signal,
    // gated by the meter options) onto the spectrum painter, in the band just
    // below the flag. Called by SpectrumWidget's overlay pass so the labels land
    // on top of the slice markers. No-op unless the SmartMTR meter + value labels
    // are active and the flag is expanded. Coordinates are SpectrumWidget-local.
    void drawSmartMtrLabels(QPainter& p) const;

    // Client-side DSP buttons (NR2 / NR4 / MNR / BNR / DFNR / RN2) were
    // removed from the VFO DSP grid; that family lives in the spectrum
    // overlay menu and the AetherDSP applet only.
    void setAfGain(int pct);
    void setEscLevel(float dbm);
    void setEscControlsAvailable(bool available);
    void syncFromSlice();
    void setRecordOn(bool on);
    void setPlayOn(bool on);
    void setPlayEnabled(bool enabled);
    void beginDirectEntry(QString source = QStringLiteral("vfo-direct-entry"));
    QLabel* freqLabel() const { return m_freqLabel; }

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);

    // Spoken summary of this flag for AT tools (slice, frequency, TX state).
    // Consumed by VfoWidgetAccessible — the QAccessibleInterface implemented
    // for this widget in VfoWidget.cpp — so collapsed flags, whose slice/TX
    // badges are custom-painted with no child-widget equivalent, aren't opaque
    // to screen readers. (#3754)
    QString accessibleSummary() const;

    // Lean render mode: drop WA_TranslucentBackground so the panel composites
    // as an opaque, cacheable layer instead of being alpha-blended over the
    // whole window every frame (the dominant idle CPU cost — see #3283).
    void setOpaqueMode(bool on);

    // Which side of the slice marker the flag panel is currently rendered on.
    // Tracked by updatePosition() via m_lastOnLeft.  Used by panFollowVfo()
    // to extend the pan-follow trigger to the flag's outer edge — single-side
    // for non-split slices, both-sides for split pairs (#2761).
    bool onLeft() const { return m_lastOnLeft; }

#ifdef HAVE_RADE
    void setRadeActive(bool on, const QString& label = QStringLiteral("RADE"));
    void setRadeSynced(bool synced);
    void setRadeSnr(float snrDb);
    void setRadeFreqOffset(float hz);
    void setRadeCallsign(const QString& callsign);
#endif

    // Reflect the real WFM demodulator state (owned by MainWindow) back onto
    // the WFM toggle button, WITHOUT re-emitting wfmActivated. Self-gated on
    // this widget's slice so a state change on another slice is ignored.
    // Keeps the VfoWidget and RxApplet WFM buttons from desyncing when the
    // demod is toggled from the other surface or torn down by a mode change.
    void setWfmActive(bool on, int sliceId);

Q_SIGNALS:
    void afGainChanged(int value);
    void audioMuteToggled(bool on);   // per-slice AF mute changed by user (#1560)
    void rxPanChanged(int value);     // pan slider moved; AudioEngine re-applies after NR (#1460)
    void closeSliceRequested();
    void lockToggled(bool locked);
    // Client-side DSP signals deleted with the buttons — overlay menu
    // and AetherDSP applet handle those toggles directly now.
#ifdef HAVE_RADE
    void radeActivated(bool on, int sliceId);
#endif
    void wfmActivated(bool on, int sliceId);
    void recordToggled(bool on);
    void playToggled(bool on);
    void aetherDspRequested();     // user clicked the ADSP button on the DSP tab
    void aetherVoiceRequested();   // user clicked the AetherVoice button on the DSP tab
    void splitToggled();
    void swapRequested();
    void autotuneRequested(bool intermittent);  // CW auto-tune: false=stop, true=loop
    void autotuneOnceRequested();               // CW auto-tune one-shot
    void zeroBeatRequested();                   // client-side CW zero-beat
    void addSpotRequested(double freqMhz);
    void sliceActivationRequested(int sliceId);
    void kiwiRxAntennaSelected(int sliceId, const QString& profileId);
    void flexRxAntennaSelected(int sliceId);
    void autoSqlMarginDbChanged(int dB);
    // Emitted when the wheel tunes by step so MainWindow can apply the shared
    // tuning/reveal policy.
    void stepTuneRequested(double mhz);
    void directEntryCommitted(double mhz, const QString& source);
    // Per-slice VFO marker style changed (#1526).  markerWidth: 0 = off
    // (no center line / no top triangle, passband only), 1 = 1 px line,
    // 3 = 3 px line.
    void markerStyleChanged(int markerWidth, bool filterEdgesHidden);
    // The SmartMTR value labels need a repaint (meter values/options changed).
    // SpectrumWidget connects this to markOverlayDirty() so the spectrum overlay
    // (which draws the labels) refreshes. Throttled at the source.
    void smartMtrLabelsChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void wheelEvent(QWheelEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    void updateSignalMeterTarget();
    void animateSignalMeter();
    // Build and push the current MeterInput (signal vs mic by TX state) to the
    // SmartMTR widget. Cheap; safe to call on every level/state update.
    void pushSmartMtrInput();
    // Read the global extremes options (MeterViewController) and push them to the
    // SmartMTR widget; show/hide + reposition the value-label overlay. Called on
    // construction, on extremesChanged() broadcast, and on meter-view switch.
    void pushSmartMtrOptions();
    // Throttled bridge from the meter's repaint to a spectrum-overlay refresh.
    void onSmartMtrRepainted();
    bool usesUnavailableSignalMeter() const;
    static float signalDbmToMeterFraction(float dbm);

    void buildUI();
    void buildTabContent();
    // Meter view (standard S-Meter vs SmartMTR component).  Driven globally by
    // MeterViewController; m_meterStack switches pages and meterBarRect() locates
    // the painted bar.  The inline selector row (m_meterMenuRow) is revealed by
    // clicking the meter strip; syncMeterMenuButtons() reflects the choice.
    void applyMeterView(bool smartMtr);
    void syncMeterMenuButtons();
    void setMeterMenuOpen(bool open);  // open/close the S-Meter/SmartMTR selector
    QRect meterBarRect() const;
    void updateTxBadgeStyle(bool isTx);
    void showTab(int index);
    void closeActiveTab();  // close any open DSP/Mode/... tab panel
    void updateFreqLabel();
    bool cancelDirectEntry();
    void updateFilterLabel();
    void updateModeTab();
    void rebuildFilterButtons();
    void updateFilterHighlight();
    void applyFilterPreset(int widthHz);
    void saveFilterPresets();
    void updateAgcSliderFromSlice();
    void updateAntennaButton(QPushButton* button, const QString& token, bool tx);
    void updateAntennaButtons();
    QStringList txAntennaOptions() const;
    QString antennaMenuLabel(const QString& token, const QStringList& options) const;
    static QString formatFilterLabel(int hz);

    SliceModel*    m_slice{nullptr};
    TransmitModel* m_txModel{nullptr};
    RadioModel*    m_radioModel{nullptr};
    KiwiSdrManager* m_kiwiSdrManager{nullptr};
    QStringList    m_antList;
    bool           m_updatingFromModel{false};
    bool           m_lastOnLeft{true};
    float          m_signalDbm{-130.0f};
    // Whether m_signalDbm is a real calibrated reading. FLEX always is; a
    // KiwiSDR slice without a calibrated meter is not, in which case the
    // SmartMTR needle must show no-data rather than peg the hardcoded S0.
    bool           m_signalHasDbm{true};
    KiwiSdrProtocol::MeterReading m_receiveMeterReading;
    bool           m_receiveMeterReadingActive{false};
    QTimer         m_signalMeterAnimation;
    QElapsedTimer  m_signalMeterElapsed;
    float          m_signalMeterFraction{0.0f};
    float          m_targetSignalMeterFraction{0.0f};
    bool           m_collapsed{false};
    bool           m_opaqueMode{false};  // lean mode: opaque (non-translucent) panel
    bool           m_collapseToggled{false};  // guard: absorb release after toggle
    int            m_scrollAccum{0};    // trackpad pixel scroll accumulator
    int            m_angleAccum{0};     // mouse wheel angle accumulator
    qint64         m_lastWheelMs{0};    // debounce: timestamp of last accepted wheel step
    QPointer<QLabel> m_collapsedFreqLabel;

    // Accessibility: debounced frequency announcement (300 ms settle before speaking)
    QTimer   m_accessibleFrequencyTimer;
    QString  m_pendingAccessibleFrequencyText;
    QString  m_lastAccessibleFrequencyText;
    void     scheduleFrequencyAnnouncement(const QString& text);
    QSet<QWidget*> m_hiddenBeforeCollapse;    // widgets already hidden before collapse

    // Header row
    QPushButton* m_rxAntBtn{nullptr};
    QPushButton* m_txAntBtn{nullptr};
    QLabel*      m_filterWidthLbl{nullptr};
    QPushButton* m_splitBadge{nullptr};
    QPushButton* m_txBadge{nullptr};
    QLabel*      m_sliceBadge{nullptr};
    QPointer<QPushButton> m_lockVfoBtn;
    QPointer<QPushButton> m_closeSliceBtn;
    QPointer<QPushButton> m_recordBtn;
    QPointer<QPushButton> m_playBtn;
    QTimer* m_recordPulse{nullptr};

    static constexpr int kSignalMeterAnimationIntervalMs = 8;
    static constexpr float kSignalMeterAttackTimeSeconds = 0.045f;
    static constexpr float kSignalMeterReleaseTimeSeconds = 0.180f;
    static constexpr float kSignalMeterSnapEpsilon = 0.001f;

    // Frequency / meter
    QLabel* m_freqLabel{nullptr};
    QLineEdit* m_freqEdit{nullptr};
    QStackedWidget* m_freqStack{nullptr};
    QLabel* m_dbmLabel{nullptr};
    // Meter strip: page 0 = standard S-meter (painted bar + dBm label),
    // page 1 = SmartMTR component.  m_smartMtr mirrors the current page.
    QStackedWidget* m_meterStack{nullptr};
    bool m_smartMtr{false};
    SmartMtrWidget* m_smartMtrWidget{nullptr};
    // The SmartMTR extremes value labels are drawn by SpectrumWidget's overlay
    // pass (so they sit on top of the slice). This clock throttles how often the
    // meter's repaint asks the spectrum overlay to refresh.
    QElapsedTimer m_labelDirtyClock;
    qint64 m_lastLabelDirtyMs{-1};
    float m_micDbfs{-40.0f}; // latest mic level (dBFS); SmartMTR TX scale
    float m_micPeakDbfs{-40.0f}; // latest mic peak (dBFS, radio MICPEAK stat)
    bool m_transmitting{false}; // global MOX state
    // Inline selector row revealed by clicking the meter strip (not a popup),
    // shown between the meter and the tab bar.
    QWidget* m_meterMenuRow{nullptr};
    // Explicit open-state for the selector. The paintEvent underline gates on
    // this rather than m_meterMenuRow->isVisible(): in GPU flag mode the flag is
    // hidden and rasterized into a sprite, where the child's isVisible() reads
    // false even with the selector open — which dropped the underline from the
    // sprite. The selector is one of the controls that should stay visible.
    bool m_meterMenuOpen{false};
    QPushButton* m_sMeterOptBtn{nullptr};
    QPushButton* m_smartMtrOptBtn{nullptr};
    // SmartMTR-only display options, shown vertically below the selector
    // buttons. Disabled while the standard S-meter is selected. "Extremes
    // speed" is further gated on "Show extremes" being checked.
    QCheckBox* m_showExtremesChk{nullptr};
    QComboBox* m_extremesSpeedCmb{nullptr};
    QComboBox* m_showValuesCmb{nullptr};
    // Which meter to show while transmitting: None (stay on RX signal) or Mic
    // Level. Disabled while the standard S-meter is selected.
    QComboBox* m_txMeterCmb{nullptr};
    // The three SmartMTR option rows (label + combo). Disabled as a unit when the
    // option doesn't apply; the label/combo dim via their :disabled stylesheet —
    // render()-compatible, so they stay dimmed (not blank) in GPU flag sprites.
    QWidget* m_speedRow{nullptr};
    QWidget* m_valuesRow{nullptr};
    QWidget* m_txMeterRow{nullptr};
    // Enable/disable the SmartMTR-only options per the current meter view and
    // the "Show extremes" checkbox state (see implementation for the rules).
    void syncSmartMtrSettingsState();
    // Re-seed this flag's option controls from the global MeterViewController
    // (used when another open flag changes a setting), then re-evaluate state.
    void syncSmartMtrSettingsControls();
    // Thin spacer between the meter and the tab bar, shown only while the meter
    // selector is open, to give the curved underline room below the indicator.
    QWidget* m_meterUnderlineRoom{nullptr};
    QString m_directEntrySource{"vfo-direct-entry"};

    // Sub-menu tabs
    QVector<QPushButton*> m_tabBtns;
    QStackedWidget* m_tabStack{nullptr};
    QWidget*        m_tabBar{nullptr};
    int m_activeTab{-1};

    // Tab content widgets
    // Audio tab
    QSlider* m_afGainSlider{nullptr};
    QSlider* m_panSlider{nullptr};
    QPushButton* m_muteBtn{nullptr};
    QPushButton* m_divBtn{nullptr};
    // ESC (Enhanced Signal Clarity) panel — shown when DIV is active (parent only)
    QWidget*     m_escPanel{nullptr};
    QPushButton* m_escBtn{nullptr};
    PhaseKnob*   m_phaseKnob{nullptr};
    QSlider*     m_escPhaseSlider{nullptr};
    QPushButton* m_escPlus180Btn{nullptr};
    QSlider*     m_escGainSlider{nullptr};
    QLabel*      m_escPhaseLbl{nullptr};
    QLabel*      m_escGainLbl{nullptr};
    QLabel*      m_escMeterLbl{nullptr};
    QLabel*      m_escDbmLbl{nullptr};
    QWidget*     m_escMeterBar{nullptr};
    float        m_escLevelDbm{-130.0f};
    bool         m_diversityAllowed{true};
    bool         m_escControlsAvailable{true};
    void syncEscPanelVisibility();
    void syncTabStackHeightToCurrentPage();
    void relayoutToCurrentContent();
    QPushButton* m_sqlBtn{nullptr};
    QPointer<RxApplet> m_rxApplet;       // mirrored only while this VFO's slice is active
    QLabel*      m_sqlValueLbl{nullptr}; // captured during buildUI() for syncSqlVisuals
    bool         m_savedSquelchOn{false};
    // Apply the current SqlMode from m_rxApplet to the VfoWidget's SQL
    // button label/style and slider range/value.  Called on rxApplet's
    // sqlModeChanged signal and once after setRxApplet().
    bool mirrorsRxAppletSql() const;
    enum class LocalSqlMode { Off, Manual, Auto };
    LocalSqlMode standaloneSqlMode() const;
    void cycleStandaloneSqlMode();
    int autoSqlMarginDb() const;
    void setAutoSqlMarginDb(int dB);
    int manualSqlMaximum() const;
    int clampManualSqlLevel(int level) const;
    int agcThresholdMinimum() const;
    int agcThresholdMaximum() const;
    void syncSqlVisuals();
public:
    void setDiversityAllowed(bool allowed);
    void setSmartSdrPlus(bool has);
    void setHasExtendedDsp(bool has);

    // Reflect whether any client-side AetherDSP NR module (NR2 / NR4 / MNR /
    // BNR / DFNR / RN2) is active by accenting the ADSP launcher, so the cue is
    // visible on the VFO grid without opening the applet. Driven by MainWindow
    // from the AudioEngine *EnabledChanged signals. (#3800)
    void setAetherDspActive(bool active);

    // Per-slice VFO marker display prefs, persisted by slice ID (#1526).
    // markerWidth: 0 = off, 1 = 1 px, 3 = 3 px.
    int  markerWidth() const { return m_markerWidth; }
    bool filterEdgesHidden() const { return m_filterEdgesHidden; }
    void setMarkerWidth(int widthPx);
    void setFilterEdgesHidden(bool hide);
private:
    int  m_markerWidth{1};
    bool m_filterEdgesHidden{false};
    // Marker: single button cycling Off → 1 px → 3 px on click.  Label
    // reflects the current state.
    class QPushButton* m_markerThicknessBtn{nullptr};
    // Filter edge lines: single checkable button — checked = edges shown,
    // unchecked = edges hidden.
    class QPushButton* m_edgesBtn{nullptr};
    void loadDisplayPrefs();
    void saveDisplayPrefs();

    QSlider* m_sqlSlider{nullptr};
    QComboBox* m_agcCmb{nullptr};
    QSlider* m_agcTSlider{nullptr};
    QLabel* m_agcValueLbl{nullptr};
    // DSP tab
    QPushButton* m_nbBtn{nullptr};
    QPushButton* m_nrBtn{nullptr};
    QPushButton* m_anfBtn{nullptr};
    QPushButton* m_nrlBtn{nullptr};
    QPushButton* m_nrsBtn{nullptr};
    QPushButton* m_rnnBtn{nullptr};
    QPushButton* m_nrfBtn{nullptr};
    QPushButton* m_anflBtn{nullptr};
    QPushButton* m_anftBtn{nullptr};
    QPushButton* m_apfBtn{nullptr};
    QPushButton* m_aetherDspBtn{nullptr};    // launches AetherDSP Settings dialog
    bool         m_aetherDspActive{false};   // any client NR module on (#3800)
    QPushButton* m_aetherVoiceBtn{nullptr};  // toggles Aetherial Audio Channel Strip

    // Shared DSP-level row at the bottom of the DSP grid: one slider whose
    // target switches based on which leveled DSP the user most recently
    // turned on.  RNN / ANFT / APF are toggle-only on this slider — they
    // either have no level (RNN, ANFT) or own a dedicated container (APF).
    enum DspLevelTarget { LvlNone = 0, LvlNR, LvlNB, LvlAnf, LvlNrl, LvlNrs, LvlNrf, LvlAnfl };
    QWidget* m_dspLevelRow{nullptr};
    QLabel*  m_dspLevelLabel{nullptr};
    QSlider* m_dspLevelSlider{nullptr};
    QLabel*  m_dspLevelValue{nullptr};
    DspLevelTarget m_dspLevelTarget{LvlNone};
    // Activation stack — most recent at the back.  Lets the slider fall
    // back to the previous still-on DSP when the active one is turned
    // off, instead of hiding the row entirely.
    QList<DspLevelTarget> m_dspLevelStack;
    void pushDspLevelTarget(DspLevelTarget t);
    void popDspLevelTarget(DspLevelTarget t);
    void setDspLevelTarget(DspLevelTarget t);
    // Pick a sensible initial target from the current slice's enable
    // flags; called when m_slice is set and on mode-driven re-visibility.
    void refreshDspLevelTarget();
    QWidget* m_apfContainer{nullptr};
    QSlider* m_apfSlider{nullptr};
    QLabel*  m_apfValueLbl{nullptr};
    // DSP grid re-layout
    QGridLayout* m_dspGrid{nullptr};
    void relayoutDspGrid();
    // RTTY Mark/Shift (shown only in RTTY mode)
    QWidget* m_rttyContainer{nullptr};
    // DIG offset (shown only in DIGL/DIGU mode)
    QWidget*        m_digContainer{nullptr};
    ScrollableLabel* m_digOffsetLabel{nullptr};   // read-only display, scroll-wheel steps
    QLineEdit*       m_digOffsetEdit{nullptr};     // inline direct-entry (double-click)
    QStackedWidget*  m_digOffsetStack{nullptr};    // switches between label and edit
    // FM OPT controls (shown only in FM/NFM mode)
    QWidget*       m_fmContainer{nullptr};
    QComboBox*     m_fmToneModeCmb{nullptr};
    QComboBox*     m_fmToneValueCmb{nullptr};
    QDoubleSpinBox* m_fmOffsetSpin{nullptr};
    QPushButton*   m_fmOffsetDown{nullptr};
    QPushButton*   m_fmSimplexBtn{nullptr};
    QPushButton*   m_fmOffsetUp{nullptr};
    QPushButton*   m_fmRevBtn{nullptr};
    ScrollableLabel* m_markLabel{nullptr};
    ScrollableLabel* m_shiftLabel{nullptr};
    // Mode tab
    QComboBox* m_modeCombo{nullptr};
    QPushButton* m_quickModeBtns[3]{};
    QString      m_quickModeAssign[3];  // e.g. "USB", "CW", "SSB", "DIG"
    QPushButton* m_wfmBtn{nullptr};
    void updateQuickModeButtons();
    QGridLayout* m_filterGrid{nullptr};
    QVector<QPushButton*> m_filterBtns;
    QVector<int> m_filterWidths;
    // Parallel to m_filterWidths.  When a slot has user-defined custom
    // edges (right-click → "Set Custom Edges..."), the lo/hi are stored
    // here and applied directly instead of going through applyFilterPreset's
    // mode-rule recompute.  INT_MIN sentinel means "no custom edges, use
    // mode rules" — preserves the legacy width-only behaviour. (#2259)
    QVector<int> m_filterCustomLo;
    QVector<int> m_filterCustomHi;
    // CW autotune row (only visible in CW mode). The container holds the
    // "Autotune:" label + buttons; deleting it on rebuild also removes the
    // label, which is not tracked as its own member.
    class QWidget* m_autotuneContainer{nullptr};
    QPushButton* m_autotuneOnceBtn{nullptr};
    QPushButton* m_autotuneLoopBtn{nullptr};
    QPushButton* m_zeroBeatBtn{nullptr};
    bool         m_hasSmartSdrPlus{false};
    bool         m_hasExtendedDsp{false};
    // RIT/XIT tab
    QPushButton* m_ritBtn{nullptr};
    QPushButton* m_xitBtn{nullptr};
    ScrollableLabel* m_ritLabel{nullptr};
    ScrollableLabel* m_xitLabel{nullptr};
    // DAX tab
    QComboBox* m_daxCmb{nullptr};

#ifdef HAVE_RADE
    QLabel*  m_radeStatusLabel{nullptr};   // freq row: "RADE ●" badge only
    QWidget* m_radeInfoRow{nullptr};        // info row: callsign + SNR + offset
    QLabel*  m_radeCallsignLabel{nullptr};  // hidden until EOO received
    QLabel*  m_radeSnrLabel{nullptr};       // "12dB" or "---"
    QLabel*  m_radeOffsetLabel{nullptr};    // "+125Hz" — hidden when no data
    bool     m_radeActive{false};
    QString  m_radeLabel{"RADE"};          // badge prefix: "RADE" or "FreeDV"
#endif

    static constexpr int WIDGET_W = 252;
    static constexpr int COLLAPSED_W = 34;
};

} // namespace AetherSDR
