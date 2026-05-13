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

class QPushButton;
class ScrollableLabel;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QSlider;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;

namespace AetherSDR {

class SliceModel;
class TransmitModel;
class RadioModel;
class PhaseKnob;

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
    void setSignalLevel(float dbm);

    // Split mode: call whenever TX assignment or active slice changes.
    //   isTxSlice  — this VFO's slice has tx=1
    //   splitActive — TX is assigned to a different slice than the active one
    void updateSplitBadge(bool isTxSlice, bool splitActive);

    // Flag direction hint for deconfliction.
    enum FlagDir { Auto, ForceLeft, ForceRight };

    // Reposition relative to VFO marker x coordinate.
    void updatePosition(int vfoX, int specTop, FlagDir dir = Auto);

    // Client-side DSP buttons (NR2 / NR4 / MNR / BNR / DFNR / RN2) were
    // removed from the VFO DSP grid; that family lives in the spectrum
    // overlay menu and the AetherDSP applet only.
    void setAfGain(int pct);
    void setEscLevel(float dbm);
    void syncFromSlice();
    void setRecordOn(bool on);
    void setPlayOn(bool on);
    void setPlayEnabled(bool enabled);
    void beginDirectEntry(QString source = QStringLiteral("vfo-direct-entry"));
    QLabel* freqLabel() const { return m_freqLabel; }

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);

#ifdef HAVE_RADE
    void setRadeActive(bool on);
    void setRadeSynced(bool synced);
    void setRadeSnr(float snrDb);
    void setRadeFreqOffset(float hz);
#endif

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
    // Emitted when the wheel tunes by step so MainWindow can apply the shared
    // tuning/reveal policy.
    void stepTuneRequested(double mhz);
    void directEntryCommitted(double mhz, const QString& source);
    // Per-slice VFO marker style changed (#1526).  markerWidth: 0 = off
    // (no center line / no top triangle, passband only), 1 = 1 px line,
    // 3 = 3 px line.
    void markerStyleChanged(int markerWidth, bool filterEdgesHidden);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void wheelEvent(QWheelEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    void updateSignalMeterTarget();
    void animateSignalMeter();
    static float signalDbmToMeterFraction(float dbm);

    void buildUI();
    void buildTabContent();
    void updateTxBadgeStyle(bool isTx);
    void showTab(int index);
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
    QStringList    m_antList;
    bool           m_updatingFromModel{false};
    bool           m_lastOnLeft{true};
    float          m_signalDbm{-130.0f};
    QTimer         m_signalMeterAnimation;
    QElapsedTimer  m_signalMeterElapsed;
    float          m_signalMeterFraction{0.0f};
    float          m_targetSignalMeterFraction{0.0f};
    bool           m_collapsed{false};
    bool           m_collapseToggled{false};  // guard: absorb release after toggle
    int            m_scrollAccum{0};    // trackpad pixel scroll accumulator
    int            m_angleAccum{0};     // mouse wheel angle accumulator
    qint64         m_lastWheelMs{0};    // debounce: timestamp of last accepted wheel step
    QPointer<QLabel> m_collapsedFreqLabel;
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
    QString m_directEntrySource{"vfo-direct-entry"};

    // Sub-menu tabs (QLabels with click via event filter)
    QVector<QLabel*> m_tabBtns;
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
    QSlider*     m_escGainSlider{nullptr};
    QLabel*      m_escPhaseLbl{nullptr};
    QLabel*      m_escGainLbl{nullptr};
    QLabel*      m_escMeterLbl{nullptr};
    QLabel*      m_escDbmLbl{nullptr};
    QWidget*     m_escMeterBar{nullptr};
    float        m_escLevelDbm{-130.0f};
    QPushButton* m_sqlBtn{nullptr};
    bool         m_savedSquelchOn{false};
public:
    void setDiversityAllowed(bool allowed);
    void setSmartSdrPlus(bool has);
    void setHasExtendedDsp(bool has);

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
    QLabel* m_radeStatusLabel{nullptr};
    bool    m_radeActive{false};
#endif

    static constexpr int WIDGET_W = 252;
    static constexpr int COLLAPSED_W = 34;
};

} // namespace AetherSDR
