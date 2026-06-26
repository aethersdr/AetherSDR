#pragma once

#include <QWidget>
#include <QVector>
#include <QTimer>

class ScrollableLabel;
namespace AetherSDR { class FilterPassbandWidget; }

class QButtonGroup;
class QHBoxLayout;
class QGridLayout;
class QPushButton;
class QSlider;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QComboBox;
class QDoubleSpinBox;
class QToolButton;

namespace AetherSDR {

class SliceModel;
class RadioModel;
class KiwiSdrManager;

// RX Applet — controls for a single receive slice.
//
// Layout (top to bottom):
//  • RX antenna selector (ANT1 / ANT2)
//  • Filter width presets (1.8 / 2.1 / 2.4 / 2.7 / 3.3 / 6.0 kHz)
//  • AGC mode (OFF / SLOW / MED / FAST)
//  • AF gain slider (audio output level)
//  • RF gain slider (IF gain)
//  • Squelch on/off + level slider
//  • DSP toggles: NB, NR, ANF
//  • RIT on/off + Hz offset with < > step buttons
//  • XIT on/off + Hz offset with < > step buttons
class RxApplet : public QWidget {
    Q_OBJECT

public:
    // 3-way squelch state.  Off → Manual → Auto cycle on SQL button click.
    // Mirrored by VfoWidget's SQL button via the sqlModeChanged signal so
    // both UI surfaces present the same state and value.
    enum class SqlMode : uint8_t { Off, Manual, Auto };

    explicit RxApplet(QWidget* parent = nullptr);

    // Attach to a slice; pass nullptr to detach.
    void setSlice(SliceModel* slice);
    void setAfGain(int pct);

    // Cross-widget access for the bidirectional SQL sync with VfoWidget.
    // VfoWidget mirrors mode + value via these methods; RxApplet stays the
    // source of truth for SqlMode, the current surface's manual level, and
    // the AutoSqlMarginDb persistence.
    SqlMode sqlMode() const { return m_sqlMode; }
    bool    isAttachedToSlice(const SliceModel* slice) const { return m_slice == slice; }
    int     sqlManualLevel() const;
    int     sqlManualMaximum() const;
    int     autoSqlMarginDb() const;
    // Externally cycle the mode (Off → Manual → Auto → Off) — same path the
    // RxApplet's own SQL button takes.  Emits sqlModeChanged.
    void    cycleSqlModeExternal();
    // Programmatic slider drag from another UI surface.  Branches by mode
    // exactly like the in-applet slider does: Manual writes the current
    // receive surface; Flex also persists m_sqlManualLevel, while Kiwi keeps
    // its replacement-source level independent. Auto writes AppSettings
    // AutoSqlMarginDb and emits autoSqlMarginDbChanged. Off is a no-op.
    void    setSqlSliderValueExternal(int v);
    void syncStepFromSlice(int stepHz, const QVector<int>& stepList);
    void cycleStepUp();
    void cycleStepDown();

    // Step the active slice's RX passband through the per-mode filter preset
    // list. direction = +1 widens, -1 narrows. Routes through applyFilterPreset
    // so all modes (LSB/CWL/DIGL/RTTY/AM/CW/USB) get mode-correct edge geometry.
    void stepFilterWidth(int direction);

    // Connect to transmit model for QSK (break_in) indicator.
    void setTransmitModel(class TransmitModel* txModel);
    void setRadioModel(class RadioModel* radioModel);
    void setKiwiSdrManager(KiwiSdrManager* manager);

    // Set the available antenna list (from ant_list in panadapter status).
    void setAntennaList(const QStringList& ants);

    // Slice tab toggle — create N buttons (A..H) capped at hardware max.
    // If maxSlices <= 1, the row is hidden.
    void setMaxSlices(int maxSlices);
    void clearSliceButtons();

    // Enable/disable buttons based on which slices are open, and check the
    // button for the currently active slice.
    void updateSliceButtons(const QList<SliceModel*>& slices, int activeSliceId);

signals:
    // Emitted when the user clicks a slice tab button.
    void sliceActivationRequested(int sliceId);
    // Emitted when the user requests the AGC-T noise calibration panel
    // (right-click on the AGC-T slider). Carries the active slice id.
    void calibrateAgcTRequested(int sliceId);
    // Emitted when the user adjusts the AF gain slider (0–100).
    void afGainChanged(int value);
    // Emitted whenever the tuning step size changes (Hz), from ANY source —
    // including radio-driven syncs via syncStepFromSlice() (memory recall,
    // band crossing). Connect source-agnostic UI sync here (e.g. SpectrumWidget
    // scroll-to-tune step), but NOT command echoes or user-facing toasts.
    void stepSizeChanged(int hz);
    // Emitted only when the operator deliberately changes the step (STEP
    // buttons/scroll, cycle shortcuts, encoder push). Use this for actions
    // that should not fire on radio-driven syncs: pushing to the radio,
    // persisting, and the "Step: …" status-bar toast.
    void stepSizeChangedByUser(int hz);
    void kiwiRxAntennaSelected(int sliceId, const QString& profileId);
    void flexRxAntennaSelected(int sliceId);
    // Emitted when Auto SQL tracking is toggled.
    void sqlAutoChanged(bool on);
    // Emitted on every SQL mode transition (Off / Manual / Auto), so any
    // mirroring UI (e.g. VfoWidget's SQL button + slider) can refresh its
    // label, color, and slider role.  Carries the new SqlMode value as an
    // int so the header doesn't need to leak the enum to listeners that
    // don't care about the symbolic names.
    void sqlModeChanged(int mode);
    // Emitted when the user adjusts the SQL slider while SQL mode is Auto.
    // Carries the new dB margin above the measured noise floor.  Routes to
    // every SpectrumWidget's setAutoSqlMarginDb().  Replaces the standalone
    // "Auto SQL ∆" slider that used to live in the Display overlay menu.
    void autoSqlMarginDbChanged(int dB);
    // Emitted when the radio reports a squelch state change (for spectrum line).
    void squelchStateChanged(bool on, int level);
    void directEntryCommitted(double mhz, const QString& source);
    // Emitted when the user presses the Mute All button.
    // Logic lives in MainWindow::onMuteAllSlicesToggle() which has RADE context.
    void muteAllToggled();

#ifdef HAVE_RADE
    // Emitted when user selects/deselects RADE digital voice mode
    void radeActivated(bool on, int sliceId);
#endif
    void wfmActivated(bool on, int sliceId);

public:
    // Reflect the real WFM demodulator state (owned by MainWindow) back onto
    // the WFM toggle button, WITHOUT re-emitting wfmActivated. Self-gated on
    // this applet's slice so a state change on another slice is ignored.
    // Keeps the RxApplet and VfoWidget WFM buttons from desyncing when the
    // demod is toggled from the other surface or torn down by a mode change.
    void setWfmActive(bool on, int sliceId);

    void setInitialStepSize(int hz);

    // Mode-aware filter width formatter, shared with VfoWidget so the two
    // filter readouts stay in sync (#794, #1225, #2197).
    static QString formatFilterWidth(int lo, int hi, const QString& mode = QString());

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUI();
    void connectSlice(SliceModel* s);
    void disconnectSlice(SliceModel* s);
    void updateAntennaButton(QPushButton* button, const QString& token, bool tx);
    void updateAntennaButtons();
    void updateFreqLabel();
    // Shrink the frequency digits to fit the RX Controls box so the leading
    // digits never clip in the fixed-width applet column (#3463/#3515).
    void applyFreqFit();
    QStringList txAntennaOptions() const;
    QString antennaMenuLabel(const QString& token, const QStringList& options) const;

    void applyFilterPreset(int widthHz);
    void updateFilterButtons();
    void updateModeSettings(const QString& mode);
    void rebuildFilterButtons();
    void saveFilterPresets();
    void rebuildStepSizes();
    void updateAgcCombo();
    void updateOffsetDirButtons();
    void applyOffsetDir(const QString& dir);
    static QString formatHz(int hz);
    static QString formatStepLabel(int hz);

    // Keeps slice-tab styling on the normal slice identity palette. The
    // speaker button owns mute feedback; slice letters must not grey out and
    // read as disabled after mute-all or Kiwi receive routing changes.
    void refreshAllMutedDim();
    void setSliceButtonsDimmed(bool dim);

    SliceModel* m_slice{nullptr};
    TransmitModel* m_txModel{nullptr};
    RadioModel* m_radioModel{nullptr};
    KiwiSdrManager* m_kiwiSdrManager{nullptr};
    QStringList m_antList{"ANT1", "ANT2"};   // populated from ant_list key

    // Step sizes (Hz) — per-mode, swapped on mode change
    QVector<int> m_stepSizes{10, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
    int          m_stepIdx{2};          // index into m_stepSizes, default 100 Hz
    QPushButton* m_stepDown{nullptr};   // "<" button
    ScrollableLabel* m_stepLabel{nullptr};  // current step value display
    QPushButton* m_stepUp{nullptr};     // ">" button

    // ── Slice tab toggle row ─────────────────────────────────────────────
    QWidget*                m_sliceTabRow{nullptr};
    QButtonGroup*           m_sliceGroup{nullptr};
    QVector<QToolButton*>   m_sliceBtns;
    bool                    m_sliceButtonClicksConnected{false};

    // Mute button click handling — single click toggles this slice (timer-
    // deferred by the platform double-click interval, typically 400 ms),
    // double click toggles all owned slices (handled in eventFilter, which
    // cancels the timer and emits muteAllToggled).
    QTimer*                 m_muteClickTimer{nullptr};

    // ── Header row ────────────────────────────────────────────────────────
    QLabel*      m_sliceBadge{nullptr};   // "A" / "B" / "C" / "D"
    QHBoxLayout* m_headerRow{nullptr};
    QPushButton* m_lockBtn{nullptr};      // tune-lock toggle
    QPushButton* m_rxAntBtn{nullptr};     // RX antenna dropdown (blue)
    QPushButton* m_txAntBtn{nullptr};     // TX antenna dropdown (red)
    QLabel*      m_filterWidthLbl{nullptr}; // current filter width e.g. "2.7K"
    QPushButton* m_qskBtn{nullptr};       // QSK toggle
    QHBoxLayout* m_freqRow{nullptr};       // frequency display row
    QPushButton* m_txBadge{nullptr};       // TX slice indicator (click to set as TX slice)
    QComboBox*   m_modeCombo{nullptr};     // mode selector (USB, LSB, CW, etc.)
    QPushButton* m_wfmButton{nullptr};     // WFM software demodulator toggle
    QLabel*      m_freqLabel{nullptr};     // frequency readout e.g. "14.289.510"
    QLineEdit*   m_freqEdit{nullptr};
    QStackedWidget* m_freqStack{nullptr};

    // Filter presets (Hz widths) — per-mode, swapped on mode change
    QVector<int>            m_filterWidths{1800, 2100, 2400, 2700, 3300, 6000};
    // Parallel "custom edges" — INT_MIN sentinel = use mode rules. (#2259)
    QVector<int>            m_filterCustomLo;
    QVector<int>            m_filterCustomHi;
    QVector<QPushButton*>   m_filterBtns;
    QGridLayout*            m_filterGrid{nullptr};
    QWidget*                m_filterContainer{nullptr};
    AetherSDR::FilterPassbandWidget* m_filterPassband{nullptr};

    // FM duplex/repeater controls (shown only in FM/NFM/DFM modes)
    QWidget*        m_fmContainer{nullptr};
    QComboBox*      m_toneModeCmb{nullptr};
    QComboBox*      m_toneValueCmb{nullptr};
    QDoubleSpinBox* m_offsetSpin{nullptr};
    QPushButton*    m_offsetDown{nullptr};
    QPushButton*    m_simplexBtn{nullptr};
    QPushButton*    m_offsetUp{nullptr};
    QPushButton*    m_revBtn{nullptr};

    // Containers for show/hide on mode change
    QWidget*     m_agcContainer{nullptr};
    QWidget*     m_ritContainer{nullptr};
    QWidget*     m_xitContainer{nullptr};

    // AGC
    static constexpr const char* AGC_MODES[4] = {"off", "slow", "med", "fast"};
    QComboBox*   m_agcCombo{nullptr};
    QSlider*     m_agcTSlider{nullptr};

    // AF gain + audio pan
    QPushButton* m_muteBtn{nullptr};
    QSlider*     m_afSlider{nullptr};
    QSlider*     m_panSlider{nullptr};

    // Squelch — 3-way cycle: Off → Manual → Auto → Off.
    // Click m_sqlBtn cycles through the modes; the button label and style
    // change with the mode ("SQL"/"AUTO"; base/green/amber).  Auto mode
    // emits sqlAutoChanged(true) so MainWindow's spectrum-side algorithm
    // takes over driving the squelch level; Manual mode uses the slider.
    // SqlMode is declared in the public section above so VfoWidget can mirror.
    QPushButton* m_sqlBtn{nullptr};
    QSlider*     m_sqlSlider{nullptr};
    SqlMode      m_sqlMode{SqlMode::Off};
    SqlMode      m_flexSqlMode{SqlMode::Off};
    bool         m_savedSquelchOn{false};
    // Last user-chosen Manual squelch level (0–100).  Auto mode overwrites
    // the slice's squelchLevel with algorithm-suggested values every FFT
    // tick, so we cache the manual value separately and restore it when
    // the user comes back to Manual.  Seeded from the slice on first attach.
    int          m_sqlManualLevel{20};

    void applySqlModeVisuals();
    void cycleSqlMode();
    void setSqlMode(SqlMode m, bool propagateToRadio);
    bool usingExternalReceiveSquelch() const;
    int clampManualSqlLevelForCurrentSurface(int level) const;
    void setManualSqlLevelForCurrentSurface(int level);
    int agcThresholdMinimum() const;
    int agcThresholdMaximum() const;
    void syncAgcSliderFromSlice();


    // RIT
    QPushButton* m_ritOnBtn{nullptr};
    QPushButton* m_ritZero{nullptr};
    QPushButton* m_ritMinus{nullptr};
    ScrollableLabel* m_ritLabel{nullptr};
    QPushButton* m_ritPlus{nullptr};

    // XIT
    QPushButton* m_xitOnBtn{nullptr};
    QPushButton* m_xitZero{nullptr};
    QPushButton* m_xitMinus{nullptr};
    ScrollableLabel* m_xitLabel{nullptr};
    QPushButton* m_xitPlus{nullptr};

    static constexpr int RIT_STEP_HZ = 10;
};

} // namespace AetherSDR
