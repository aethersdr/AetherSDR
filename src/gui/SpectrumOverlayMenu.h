#pragma once

#include "models/RadioModel.h"

#include <QWidget>
#include <QVector>
#include <climits>
#include <QStringList>
#include <QPoint>
#include <QPointer>
#include <QWheelEvent>
#include <QMouseEvent>

class QPushButton;
class QComboBox;
class QSlider;
class QLabel;

namespace AetherSDR {

class MemoryBrowsePanel;
class SliceModel;

// Floating overlay menu anchored to the top-left of the SpectrumWidget.
// Open by default; collapses to a single arrow button when closed.
// Buttons are placeholders — signals emitted for parent to wire.
class SpectrumOverlayMenu : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumOverlayMenu(QWidget* parent = nullptr);

    // Raise this widget and all floating panels above sibling widgets.
    void raiseAll();
    void setMemories(const QMap<int, MemoryEntry>& memories);

    // Set the antenna list (from RadioModel::antListChanged).
    void setAntennaList(const QStringList& ants);

    // Sync Display sub-panel controls with saved settings.  The Black slider
    // displays `black` while autoBlack is off and `autoBlackOffset` while it
    // is on; both values are stored internally so toggling the AUTO button
    // swaps the slider position without losing either preference.
    void syncDisplaySettings(int avg, int fps, int fillPct, bool weightedAvg,
                             const QColor& fillColor, int gain, int black,
                             bool autoBlack, int autoBlackOffset, int rate,
                             int floorPos = 75, bool floorEnable = false,
                             bool heatMap = true, int colorScheme = 0,
                             bool showGrid = true,
                             float lineWidth = 2.0f);
    // Sync blanker/cursor/opacity controls not covered by syncDisplaySettings.
    void syncExtraDisplaySettings(bool blankerOn, float blankerThresh,
                                  int bgOpacity,
                                  int freqGridSpacingKhz = 0);

    // Set the panadapter ID this overlay belongs to (for +RX routing).
    void setPanId(const QString& id) { m_panId = id; }
    QString panId() const { return m_panId; }

    // Connect/disconnect the ANT panel to a slice model.
    void setSlice(SliceModel* slice);
    void setWnbState(bool on, int level);
    void setRfGain(int gain);
    void setRfGainRange(int low, int high, int step);
    void setSwrSweepPowerWatts(int watts);

    // Populate XVTR band sub-panel
    struct XvtrBand { QString name; double rfFreqMhz; };
    void setXvtrBands(const QVector<XvtrBand>& bands);
    void syncDaxIqChannel(int channel);
    // DSP button accessors and the DSP sub-panel were removed — radio-
    // side DSP lives on VfoWidget only, client-side DSP lives on the
    // AetherDSP applet only.

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override { event->accept(); }
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); }

signals:
    void addRxClicked(const QString& panId);
    void addTnfClicked();
    void memoryActivated(int memoryIndex, const QString& panId);
    void quickAddMemoryRequested(const QString& panId);
    void daxIqChannelChanged(int channel);  // 0=Off, 1-4
    void addPanClicked();
    void daxClicked();
    // DSP-related signals (nr2Toggled / rn2Toggled / bnrToggled /
    // nr4Toggled / mnrToggled / dfnrToggled / bnrIntensityChanged /
    // *RightClicked) were removed with the overlay's DSP panel.
    // Display sub-panel signals
    void fftAverageChanged(int frames);
    void fftFpsChanged(int fps);
    void fftWeightedAverageChanged(bool on);
    void fftFillAlphaChanged(float alpha);
    void fftFillColorChanged(const QColor& color);
    void fftHeatMapChanged(bool on);
    void showGridChanged(bool on);
    void freqGridSpacingChanged(int khz);
    void fftLineWidthChanged(float width);
    void wfColorGainChanged(int gain);
    void wfBlackLevelChanged(int level);
    void wfAutoBlackChanged(bool on);
    // Auto-black target offset (0-100, 50 = at noise floor).  Emitted when
    // the Black slider moves while AUTO is engaged.
    void wfAutoBlackOffsetChanged(int offset);
    void wfLineDurationChanged(int ms);
    void wfColorSchemeChanged(int scheme);
    void noiseFloorPositionChanged(int pos);
    void noiseFloorEnableChanged(bool on);
    // Emitted when the auto-squelch margin slider changes (dBm above noise floor).
    void autoSqlMarginDbChanged(int dBm);
    // Emitted when user selects a band from the sub-panel.
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);
    // Emitted when user clicks XVTR button to open Radio Setup XVTR tab.
    void xvtrSetupRequested();
    // Emitted when WNB toggle changes.
    void wnbToggled(bool on);
    // Emitted when WNB level slider changes (0–100).
    void wnbLevelChanged(int level);
    // Emitted when RF gain slider changes (panadapter-level).
    void rfGainChanged(int gain);
    void swrSweepStartRequested(int sliceId, int sweepPowerWatts);
    void swrSweepPowerChanged(int watts);
    void swrSweepClearRequested();
    // NB Waterfall Blanker (#277)
    void wfBlankerEnabledChanged(bool on);
    void wfBlankerThresholdChanged(float threshold);
    void backgroundImageRequested();
    void backgroundImageCleared();
    void backgroundOpacityChanged(int pct);
    void displaySettingsReset();

private:
    QString m_panId;
    void toggle();
    void updateLayout();
    void toggleBandPanel();
    void buildBandPanel();
    void toggleAntPanel();
    void buildAntPanel();
    void toggleDaxPanel();
    void buildDaxPanel();
    void syncDaxPanel();
    void toggleDisplayPanel();
    void buildDisplayPanel();
    void toggleMemoryPanel();
    void buildMemoryPanel();
    void hideAllSubPanels();
    void showBandPanelAt(const QPoint& pos);
    void syncAntPanel();

    static constexpr int kBtnAddRx = 0;
    static constexpr int kBtnAddTnf = 1;
    static constexpr int kBtnBand = 2;
    static constexpr int kBtnAnt = 3;
    static constexpr int kBtnDisplay = 4;
    static constexpr int kBtnMemoryBrowse = 5;
    static constexpr int kBtnDax = 6;

    QPushButton* m_toggleBtn{nullptr};
    QVector<QPushButton*> m_menuBtns;
    bool m_expanded{true};

    // Band sub-panel (shown to the right of the menu)
    QWidget* m_bandPanel{nullptr};
    bool m_bandPanelVisible{false};
    QWidget* m_xvtrPanel{nullptr};
    bool m_xvtrPanelVisible{false};
    QVector<QPushButton*> m_xvtrBandBtns;

    // ANT sub-panel
    QWidget*     m_antPanel{nullptr};
    bool         m_antPanelVisible{false};
    QComboBox*   m_rxAntCmb{nullptr};
    QSlider*     m_rfGainSlider{nullptr};
    QLabel*      m_rfGainLabel{nullptr};
    QPushButton* m_wnbBtn{nullptr};
    QSlider*     m_wnbSlider{nullptr};
    QLabel*      m_wnbLabel{nullptr};
    QPushButton* m_swrStartBtn{nullptr};
    QPushButton* m_swrClearBtn{nullptr};
    QSlider*     m_swrPowerSlider{nullptr};
    QLabel*      m_swrPowerLabel{nullptr};

    // DAX sub-panel
    QWidget*   m_daxPanel{nullptr};
    bool       m_daxPanelVisible{false};
    QComboBox* m_daxIqCmb{nullptr};

    // Memory browse sub-panel
    MemoryBrowsePanel* m_memoryPanel{nullptr};
    bool               m_memoryPanelVisible{false};

    // Display sub-panel
    QWidget*     m_displayPanel{nullptr};
    bool         m_displayPanelVisible{false};
    QSlider*     m_avgSlider{nullptr};
    QLabel*      m_avgLabel{nullptr};
    QSlider*     m_fpsSlider{nullptr};
    QLabel*      m_fpsLabel{nullptr};
    QSlider*     m_fillSlider{nullptr};
    QLabel*      m_fillLabel{nullptr};
    QPushButton* m_fillColorBtn{nullptr};
    QColor       m_fillColor{0x00, 0xe5, 0xff};  // default cyan
    QPushButton* m_heatMapBtn{nullptr};
    QPushButton* m_showGridBtn{nullptr};
    QSlider*     m_lineWidthSlider{nullptr};
    QLabel*      m_lineWidthLabel{nullptr};
    QPushButton* m_weightedAvgBtn{nullptr};
    QSlider*     m_gainSlider{nullptr};
    QLabel*      m_gainLabel{nullptr};
    QSlider*     m_blackSlider{nullptr};
    QLabel*      m_blackLabel{nullptr};
    QPushButton* m_autoBlackBtn{nullptr};
    // Two values backing the single Black slider; the slider shows whichever
    // matches the current AUTO state.  Toggling AUTO swaps the displayed
    // value, edits route to the matching member + matching signal.
    int          m_blackManualValue{15};
    int          m_blackAutoOffsetValue{50};
    QComboBox*   m_colorSchemeCmb{nullptr};
    QSlider*     m_rateSlider{nullptr};
    QLabel*      m_rateLabel{nullptr};
    // NB Waterfall Blanker (#277)
    QPushButton* m_wfBlankerBtn{nullptr};
    QSlider*     m_wfBlankerThreshSlider{nullptr};
    QLabel*      m_wfBlankerThreshLabel{nullptr};
    QSlider*     m_floorSlider{nullptr};
    QLabel*      m_floorLabel{nullptr};
    QPushButton* m_floorEnableBtn{nullptr};

    QComboBox*   m_freqGridSpacingCmb{nullptr};
    QSlider*     m_bgOpacitySlider{nullptr};
    QLabel*      m_bgOpacityLabel{nullptr};
    QSlider*     m_autoSqlMarginSlider{nullptr};
    QLabel*      m_autoSqlMarginLabel{nullptr};

    QStringList  m_antList;
    QPointer<SliceModel> m_slice;
    bool         m_updatingFromModel{false};
    int          m_lastEmittedRfGain{INT_MIN};  // dedupe rfgain emits across drag snap ticks (#1498)
};

} // namespace AetherSDR
