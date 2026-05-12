#pragma once

#include <QWidget>
#include <QVector>

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
    explicit RxApplet(QWidget* parent = nullptr);

    // Attach to a slice; pass nullptr to detach.
    void setSlice(SliceModel* slice);
    void setAfGain(int pct);
    void syncStepFromSlice(int stepHz, const QVector<int>& stepList);
    void cycleStepUp();
    void cycleStepDown();

    // Step the active slice's RX passband through the per-mode filter preset
    // list. direction = +1 widens, -1 narrows. Routes through applyFilterPreset
    // so all modes (LSB/CWL/DIGL/RTTY/AM/CW/USB) get mode-correct edge geometry.
    void stepFilterWidth(int direction);

    // Connect to transmit model for QSK (break_in) indicator.
    void setTransmitModel(class TransmitModel* txModel);

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
    // Emitted when the user adjusts the AF gain slider (0–100).
    void afGainChanged(int value);
    // Emitted when the user changes the tuning step size (Hz).
    void stepSizeChanged(int hz);
    // Emitted when Auto SQL tracking is toggled.
    void sqlAutoChanged(bool on);
    // Emitted when the radio reports a squelch state change (for spectrum line).
    void squelchStateChanged(bool on, int level);

#ifdef HAVE_RADE
    // Emitted when user selects/deselects RADE digital voice mode
    void radeActivated(bool on, int sliceId);
#endif

public:
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

    SliceModel* m_slice{nullptr};
    TransmitModel* m_txModel{nullptr};
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
    enum class SqlMode : uint8_t { Off, Manual, Auto };
    QPushButton* m_sqlBtn{nullptr};
    QSlider*     m_sqlSlider{nullptr};
    SqlMode      m_sqlMode{SqlMode::Off};
    bool         m_savedSquelchOn{false};

    void applySqlModeVisuals();
    void cycleSqlMode();
    void setSqlMode(SqlMode m, bool propagateToRadio);


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
