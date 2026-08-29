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
class QCheckBox;
class QDoubleSpinBox;
class QScrollArea;

namespace AetherSDR {

class MemoryBrowsePanel;
class KiwiSdrManager;
class SliceModel;
class SpectrumOverlayWheelGuard;

// Floating overlay menu anchored to the top-left of the SpectrumWidget.
// Open by default; collapses to a single arrow button when closed.
// Buttons are placeholders — signals emitted for parent to wire.
class SpectrumOverlayMenu : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumOverlayMenu(QWidget* parent = nullptr);

    // Raise this widget and all floating panels above sibling widgets.
    void raiseAll();
    void setMemories(const QMap<int, MemoryEntry>& memories, bool writable = true);

    // Set the antenna list (from RadioModel::antListChanged).
    void setAntennaList(const QStringList& ants);
    void setKiwiSdrManager(KiwiSdrManager* manager);
    void setRadioModel(RadioModel* model);

    // Sync the complete Display sub-panel. Radio-owned values are supplied by
    // live status; client-rendered values come from AppSettings. The Black
    // slider displays `black` while autoBlack is off and `autoBlackOffset`
    // while it is on.
    void syncDisplaySettings(int avg, int fps, int fillPct, bool weightedAvg,
                             const QColor& fillColor, int gain, int black,
                             bool autoBlack, int autoBlackOffset, int rate,
                             int floorPos = 75, bool floorEnable = false,
                             bool heatMap = true, int colorScheme = 0,
                             bool showGrid = true,
                             float lineWidth = 2.0f,
                             bool autoBlackRadioSide = false,
                             int renderMode = 0,
                             int dssFloorDepth = 6,
                             int dssGain = 70,
                             const QColor& lineColor = QColor(0x00, 0xe5, 0xff),
                             int dssRowSpan = 100);
    // Update only the radio-owned pan processing controls from live status.
    // Signal blockers keep status echoes from generating commands back to the
    // radio.
    // Grey the 3D Span row out when the GPU mesh path is unavailable. The CPU
    // image fallback ignores rowSpanFactor entirely, so the control would move,
    // label, and persist while nothing on screen changed.
    void setDssRowSpanSupported(bool supported);

    void syncPanProcessingSettings(int avg, int fps, bool weightedAvg);
    void syncWfLineDuration(int rate);
    void syncKiwiWaterfallSettings(int minDbm, int maxDbm, bool autoScale,
                                   int rate);
    // Sync blanker/cursor/opacity controls not covered by syncDisplaySettings.
    void syncExtraDisplaySettings(bool blankerOn, float blankerThresh,
                                  int bgOpacity,
                                  int freqGridSpacingKhz = 0,
                                  const QColor& bgFillColor = QColor(),
                                  int freqScaleFontPt = 8);

    // Set the panadapter ID this overlay belongs to (for +RX routing).
    void setPanId(const QString& id);
    QString panId() const { return m_panId; }

    // Set the panadapter's stable client-side slot index (SpectrumWidget::panIndex()
    // — 0, 1, 2, 3 by layout position, distinct from the radio-assigned m_panId
    // string above). Used to key the persisted collapsed/expanded state of this
    // menu so each panadapter slot remembers its own preference across restarts
    // (client-side UI preference, not radio-authoritative — see AGENTS.md
    // "Settings Authority Policy"). Restores the saved state on first call.
    void setPanSlotIndex(int idx);

    // Connect/disconnect the ANT panel to a slice model.
    void setSlice(SliceModel* slice);
    void setWnbState(bool on, int level);
    // Show/hide the whole WNB row (button + level slider + readout) based on
    // whether the radio runs its own DSP (RadioCapabilities::hasRadioSideDsp).
    void setRadioSideDspAvailable(bool available);
    // Whether this radio has DAX audio/IQ channels at all
    // (RadioCapabilities::hasDaxStreams). Hides the per-pan DAX button and its
    // panel: the channel selectors reach a radio-side routing feature that a
    // backend without DAX simply does not have, so on an HL2 they were live
    // controls wired to nothing.
    void setDaxStreamsAvailable(bool available);
    // Whether the connected radio can hold manual notches at all
    // (RadioCapabilities::maxNotchFilters). False HIDES the +TNF button rather
    // than disabling it: the control shipped live on every backend while the
    // commands behind it only meant anything to a Flex, so on any other radio
    // it was a button that did nothing.
    void setNotchesSupported(bool supported);
    // Whether the RADIO computes a per-tile waterfall black level
    // (RadioCapabilities::hasRadioSideWaterfallAutoBlack). False removes HW from
    // the Black Level button's cycle and moves off it if it was selected —
    // the SW estimate is untouched and stays available on every family.
    void setRadioSideAutoBlackAvailable(bool available);
    void syncWnbState(bool on, int level, bool updating);
    void setRfGain(int gain);
    void setRfGainRange(int low, int high, int step,
                       const QString& unitSuffix = QStringLiteral(" dB"));
    // Discrete receive front-end stages. An EMPTY label list hides the
    // control — a radio with no preamp or no attenuator shows neither an
    // empty button nor a disabled one.
    void setPreampLabels(const QStringList& labels);
    void setPreampStep(int step);
    void setAttenuatorLabels(const QStringList& labels);
    void setAttenuatorStep(int step);
    void setLoopState(bool loopA, bool loopB);
    void syncNoiseFloorPosition(int pos);
    void syncDssFloorDepth(int dB);

    // Populate XVTR band sub-panel
    struct XvtrBand { QString name; double rfFreqMhz; QString stackKey; };
    void setXvtrBands(const QVector<XvtrBand>& bands);

    // Surface the connected radio's built-in transverter bands (4m on
    // FLEX-6500 Region 1, 4m + 2m on FLEX-6700) in the band menu.  Pass
    // a default-constructed value when disconnected — the conditional
    // VHF row will disappear.  Triggers a band-panel rebuild. (#695)
    void setRadioCapabilities(ModelCapabilities caps);

    // The tuning range the connected backend reports (MHz). Band buttons whose
    // target frequency falls outside it are disabled and say why, so a
    // direct-sampling HF receiver stops offering 6 m as though it were a band
    // it could reach. Pass (0, 0) for "not reported" — every button is enabled,
    // which is the pre-existing behaviour and what a Flex gets.
    void setTuningRangeMhz(double minMhz, double maxMhz);

    // Bands the radio itself declared (optional "bands=" discovery/status
    // key, names from BandDefs).  Non-empty: the band grid is built from
    // this list instead of the HF layout + model capability flags, so a
    // gateway presenting non-Flex hardware offers its true band set (e.g.
    // an IC-9700's 2m/440/23cm).  Empty (all real Flex radios): the grid
    // is unchanged.  Triggers a band-panel rebuild on change.
    void setDeclaredBands(const QStringList& bands,
                          const QVector<DeclaredBandRange>& ranges = {});
    void syncDaxIqChannel(int channel);
    // Reflect the real WFM demodulator state onto the DAX-panel WFM toggle
    // WITHOUT re-emitting wfmToggleRequested. Self-gated on this menu's slice,
    // so a state change on another slice is ignored. (#3853)
    void setWfmActive(bool on, int sliceId);
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
    // WFM software-demod toggle in the DAX panel. Acts on this menu's slice;
    // WFM is mode-independent (raw IQ from this pan's DAX stream). (#3853)
    void wfmToggleRequested(bool on, int sliceId);
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
    void fftLineColorChanged(const QColor& color);
    void fftHeatMapChanged(bool on);
    void showGridChanged(bool on);
    void freqGridSpacingChanged(int khz);
    void freqScaleFontPtChanged(int pt);
    void fftLineWidthChanged(float width);
    void wfColorGainChanged(int gain);
    void wfBlackLevelChanged(int level);
    void wfAutoBlackChanged(bool on);
    // Auto-black target offset (0-100, 50 = at noise floor).  Emitted when
    // the Black slider moves while AUTO is engaged.
    void wfAutoBlackOffsetChanged(int offset);
    // Auto-black source: false = client-side estimate (default), true = radio.
    void wfAutoBlackSourceChanged(bool radioSide);
    void wfLineDurationChanged(int ms);
    void kiwiWaterfallMaxChanged(int maxDbm);
    void kiwiWaterfallMinChanged(int minDbm);
    void kiwiWaterfallAutoRequested();
    void kiwiWaterfallRateChanged(int rate);
    void wfColorSchemeChanged(int scheme);
    void spectrumRenderModeChanged(int mode);
    void dssFloorDepthChanged(int dB);
    void dssGainChanged(int pct);
    void dssRowSpanChanged(int pct);
    void noiseFloorPositionChanged(int pos);
    void noiseFloorEnableChanged(bool on);
    // Emitted when user selects a band from the sub-panel.  stackKeyHint is
    // populated only when the clicked control already knows the exact Flex
    // band-stack key, such as a configured XVTR button.
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode,
                      const QString& stackKeyHint = {});
    // Emitted when user clicks XVTR button to open Radio Setup XVTR tab.
    void xvtrSetupRequested();
    // Emitted when WNB toggle changes.
    void wnbToggled(bool on);
    void loopAToggled(bool on);
    void loopBToggled(bool on);
    // Emitted when WNB level slider changes (0–100).
    void wnbLevelChanged(int level);
    // Emitted when RF gain slider changes (panadapter-level).
    void rfGainChanged(int gain);
    // Step index into the label list this menu was given, never a dB value.
    void preampStepChanged(int step);
    void attenuatorStepChanged(int step);
    // customLowMhz/customHighMhz bound the sweep when the operator has ticked
    // "Limit range" (else both 0 = sweep the full band). The values are clamped
    // to the in-region band edges receiver-side, so they can only ever narrow
    // the sweep, never widen it past what the band plan already permits.
    void swrSweepStartRequested(int sliceId, int sweepPowerWatts,
                                double customLowMhz, double customHighMhz);
    void swrSweepClearRequested();
    void swrSweepSaveCsvRequested();
    void kiwiRxAntennaSelected(int sliceId, const QString& profileId);
    void flexRxAntennaSelected(int sliceId);
    // NB Waterfall Blanker (#277)
    void wfBlankerEnabledChanged(bool on);
    void wfBlankerThresholdChanged(float threshold);
    void backgroundImageRequested();
    void backgroundImageCleared();
    // Right-click "Clear": turn the background off entirely (no image, just the
    // fill colour) and persist it.
    void backgroundImageDisabled();
    void backgroundOpacityChanged(int pct);
    void backgroundFillColorChanged(const QColor& color);
    void displaySettingsReset();
    // "Clone to all Pans": push every Display-panel setting on THIS pan onto
    // every other open panadapter, so a tuned-in look is set once instead of
    // per pan. MainWindow owns the fan-out (it is the only object that can see
    // the other pans and the radio).
    void displaySettingsCloneRequested();

private:
    QString m_panId;
    int m_panSlotIndex{-1};
    QPointer<PanadapterModel> m_panadapter;
    QMetaObject::Connection m_panRxAntennaConnection;
    QMetaObject::Connection m_panLoopConnection;
    SpectrumOverlayWheelGuard* m_wheelGuard{nullptr};
    void setKiwiWaterfallControlMode(bool kiwiMode);
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
    void layoutDisplayPanel();
    void buildDisplayPanel();
    void toggleMemoryPanel();
    void buildMemoryPanel();
    void hideAllSubPanels();
    void showBandPanelAt(const QPoint& pos);
    void syncAntPanel();
    void wirePanadapterRxAntenna();
    void refreshAntennaCombo();
    void setRxAntennaComboToken(const QString& token);
    QString currentRxAntennaToken() const;
    QString antennaComboLabel(const QString& token, const QStringList& options) const;
    void updateLoopButtonVisibility();

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

    // Every band button in the main band panel paired with the frequency it
    // tunes to, so the tuning-range gate can be re-applied after any rebuild
    // without the two builders each having to know about it.
    //
    // QPointer, not a raw pointer: the band panel is destroyed with
    // deleteLater() on every rebuild, so entries can outlive their buttons by a
    // full event-loop turn if a range update lands in that window.
    QVector<QPair<QPointer<QPushButton>, double>> m_bandBtnFreqs;
    double m_tuningMinMhz{0.0};
    double m_tuningMaxMhz{0.0};
    // True until a connected backend says otherwise, so a disconnected session
    // keeps the button rather than having it appear on connect.
    bool m_notchesSupported{true};
    void applyTuningRangeToBandButtons();

    // Cached state for band-panel rebuilds — setXvtrBands() and
    // setRadioCapabilities() each store their argument and trigger
    // a rebuild so either input changing produces a correct panel
    // without the caller needing to re-supply the other half. (#695)
    QVector<XvtrBand>  m_lastXvtrBands;
    ModelCapabilities  m_radioCapabilities;
    QStringList        m_declaredBands;   // radio-declared band set (see setDeclaredBands)
    QVector<DeclaredBandRange> m_declaredBandRanges;

    // ANT sub-panel
    QWidget*     m_antPanel{nullptr};
    bool         m_antPanelVisible{false};
    QComboBox*   m_rxAntCmb{nullptr};
    QWidget*     m_loopRow{nullptr};
    QPushButton* m_loopABtn{nullptr};
    QPushButton* m_loopBBtn{nullptr};
    QSlider*     m_rfGainSlider{nullptr};
    QLabel*      m_rfGainLabel{nullptr};
    // What the RF-gain readout appends. " dB" on a radio with a real gain
    // register, "%" on one whose gain is an opaque scale.
    QString      m_rfGainUnitSuffix{QStringLiteral(" dB")};
    void refreshFrontEndButtons();
    // ONE ROW EACH, and each hides on its own. They were a single "Front end:"
    // row with both buttons side by side, which does not fit: the ANT panel is
    // a fixed 180 px with a 48 px label column, so "Front end:" was clipped and
    // "PRE: P.AMP2" in the 56 px that left was unreadable. Two rows in the same
    // label+control shape as RX ANT and RF Gain above them cost one row of
    // height and make both legible.
    QWidget*     m_preampRow{nullptr};
    QWidget*     m_attenuatorRow{nullptr};
    QPushButton* m_preampBtn{nullptr};
    QPushButton* m_attenuatorBtn{nullptr};
    QStringList  m_preampLabels;
    QStringList  m_attenuatorLabels;
    int          m_preampStep{0};
    int          m_attenuatorStep{0};
    QWidget*     m_wnbRow{nullptr};   // container for the whole WNB row
    QPushButton* m_wnbBtn{nullptr};
    QSlider*     m_wnbSlider{nullptr};
    QLabel*      m_wnbLabel{nullptr};
    QPushButton* m_swrStartBtn{nullptr};
    QPushButton* m_swrClearBtn{nullptr};
    QPushButton* m_swrSaveBtn{nullptr};
    QCheckBox* m_swrRangeCheck{nullptr};
    QDoubleSpinBox* m_swrLowSpin{nullptr};
    QDoubleSpinBox* m_swrHighSpin{nullptr};

    // DAX sub-panel
    QWidget*     m_daxPanel{nullptr};
    bool         m_daxPanelVisible{false};
    QComboBox*   m_daxIqCmb{nullptr};
    QPushButton* m_wfmBtn{nullptr};   // WFM software-demod toggle (#3853)

    // Memory browse sub-panel
    MemoryBrowsePanel* m_memoryPanel{nullptr};
    bool               m_memoryPanelVisible{false};

    // Display sub-panel
    QWidget*     m_displayPanel{nullptr};
    QScrollArea* m_displayScroll{nullptr};
    bool         m_displayPanelVisible{false};
    QSlider*     m_avgSlider{nullptr};
    QLabel*      m_avgLabel{nullptr};
    QSlider*     m_fpsSlider{nullptr};
    QLabel*      m_fpsLabel{nullptr};
    QSlider*     m_fillSlider{nullptr};
    QLabel*      m_fillLabel{nullptr};
    QPushButton* m_fillColorBtn{nullptr};
    QColor       m_fillColor{0x00, 0xe5, 0xff};  // default cyan
    QPushButton* m_lineColorBtn{nullptr};
    QColor       m_lineColor{0x00, 0xe5, 0xff};  // default cyan (#4239)
    QPushButton* m_heatMapBtn{nullptr};
    QPushButton* m_showGridBtn{nullptr};
    QSlider*     m_lineWidthSlider{nullptr};
    QLabel*      m_lineWidthLabel{nullptr};
    QPushButton* m_weightedAvgBtn{nullptr};
    QSlider*     m_gainSlider{nullptr};
    QLabel*      m_gainTitleLabel{nullptr};
    QLabel*      m_gainLabel{nullptr};
    QSlider*     m_blackSlider{nullptr};
    QLabel*      m_blackTitleLabel{nullptr};
    QLabel*      m_blackLabel{nullptr};
    QPushButton* m_autoBlackBtn{nullptr};
    // Auto-black is a 3-way cycle on one button: 0 = Off, 1 = Auto-C (client
    // noise-floor estimate), 2 = Auto-R (radio per-tile level).
    // The operator's stored INTENT (0 Off / 1 SW / 2 HW). Keeps HW across a
    // session on a radio that cannot serve it — see effectiveAutoBlackMode.
    int m_autoBlackMode{1};
    // Permissive default, matching every other capability gate: with no
    // radio attached there is nothing to be honest about.
    bool m_radioSideAutoBlackAvailable{true};
    void applyAutoBlackMode(int mode, bool emitSignals);
    // m_autoBlackMode masked by the capability. The stored field is the
    // operator's intent and may hold HW on a radio that has none; this is what
    // the button shows and what the app acts on. (#4606)
    int  effectiveAutoBlackMode() const;
    void clearKiwiWaterfallAutoButtonState();
    // Two values backing the single Black slider; the slider shows whichever
    // matches the current AUTO state.  Toggling AUTO swaps the displayed
    // value, edits route to the matching member + matching signal.
    int          m_blackManualValue{15};
    int          m_blackAutoOffsetValue{50};
    QComboBox*   m_colorSchemeCmb{nullptr};
    QComboBox*   m_renderModeCmb{nullptr};
    QSlider*     m_dssFloorSlider{nullptr};  // 3DSS floor depth (dB below floor)
    QLabel*      m_dssFloorLabel{nullptr};
    QSlider*     m_dssGainSlider{nullptr};  // 3DSS colour floor (0-100)
    QLabel*      m_dssGainLabel{nullptr};
    QSlider*     m_dssRowSpanSlider{nullptr};  // 3DSS wedge close-in (0-100)
    QLabel*      m_dssRowSpanLabel{nullptr};
    QLabel*      m_dssRowSpanTitle{nullptr};
    bool         m_dssRowSpanSupported{true};
    QComboBox*   m_gpuCombo{nullptr};   // render-GPU selector (multi-GPU only)
    QSlider*     m_rateSlider{nullptr};
    QLabel*      m_rateLabel{nullptr};
    bool         m_kiwiWaterfallControlMode{false};
    // NB Waterfall Blanker (#277)
    QPushButton* m_wfBlankerBtn{nullptr};
    QSlider*     m_wfBlankerThreshSlider{nullptr};
    QLabel*      m_wfBlankerThreshLabel{nullptr};
    QSlider*     m_floorSlider{nullptr};
    QLabel*      m_floorLabel{nullptr};
    QPushButton* m_floorEnableBtn{nullptr};
    QPushButton* m_cloneToAllPansBtn{nullptr};

    QComboBox*   m_freqGridSpacingCmb{nullptr};
    QComboBox*   m_freqScaleFontCmb{nullptr};
    QSlider*     m_bgOpacitySlider{nullptr};
    QLabel*      m_bgOpacityLabel{nullptr};
    QPushButton* m_bgFillColorBtn{nullptr};  // colour swatch below the bg image

    QStringList  m_antList;
    RadioModel*  m_radioModel{nullptr};
    QPointer<KiwiSdrManager> m_kiwiSdrManager;
    QPointer<SliceModel> m_slice;
    bool         m_updatingFromModel{false};
    int          m_lastEmittedRfGain{INT_MIN};  // dedupe rfgain emits across drag snap ticks (#1498)
};

} // namespace AetherSDR
