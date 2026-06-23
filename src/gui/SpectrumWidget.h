#pragma once

#include <algorithm>
#include <QHash>
#include <QWidget>
#include <QPushButton>
#include <QVector>
#include <QMap>
#include <QImage>
#include <QColor>
#include <QPointer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QLabel>

class QVariantAnimation;
class QSoundEffect;

#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#include <rhi/qrhi.h>
#define SPECTRUM_BASE_CLASS QRhiWidget
#else
#define SPECTRUM_BASE_CLASS QWidget
#endif

namespace AetherSDR {

class SpectrumOverlayMenu;
class VfoWidget;

// Shared timeout for the dBm-range echo handshake between MainWindow's
// request-side tracker (wirePanadapter / PendingDbmRange) and SpectrumWidget's
// echo-side tracker (m_pendingDbmRangeEcho).  Both ends must expire on the
// same interval — if the request side stays patient longer than the echo
// side, the spectrum can drop the echo while MainWindow is still waiting
// for a match (and vice versa).  Keep them tied to this one constant.
inline constexpr qint64 kDbmRangeHandshakeTimeoutMs = 2000;

// Waterfall color scheme presets.
enum class WfColorScheme : int {
    Default = 0,   // black → dark blue → blue → cyan → green → yellow → red
    Grayscale,     // black → white
    BlueGreen,     // black → blue → teal → green → white
    Fire,          // black → red → orange → yellow → white
    Plasma,        // black → purple → magenta → orange → yellow
    Purple,        // SmartSDR "Add Purple": black→blue→green→yellow→red→purple→white
    Count          // sentinel — number of schemes
};

// Gradient stop used by waterfall color mapping.
struct WfGradientStop { float pos; int r, g, b; };

// Returns the gradient stops for a given color scheme.
const WfGradientStop* wfSchemeStops(WfColorScheme scheme, int& count);

// Returns the display name for a color scheme.
const char* wfSchemeName(WfColorScheme scheme);

// Panadapter / spectrum display widget.
//
// Layout (top to bottom):
//   ~40% — spectrum line plot (current FFT frame, smoothed)
//   ~60% — waterfall (scrolling heat-map history)
//   20px — absolute frequency scale bar
//
// Overlays (drawn on top of spectrum + waterfall):
//   - Filter passband: semi-transparent band from filterLow to filterHigh Hz
//   - VFO marker: vertical orange line at the tuned VFO frequency
//
// Click anywhere in the spectrum/waterfall area to emit frequencyClicked().
// When AETHER_GPU_SPECTRUM is enabled, inherits QRhiWidget for GPU-accelerated
// waterfall rendering. Otherwise falls back to QPainter (QWidget).
class SpectrumWidget : public SPECTRUM_BASE_CLASS {
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget* parent = nullptr);
    ~SpectrumWidget() override;

    // Per-pan settings persistence
    void setPanIndex(int idx) { m_panIndex = idx; }
    int panIndex() const { return m_panIndex; }
    QString settingsKey(const QString& base) const;
    void loadSettings();

    QSize sizeHint() const override { return {800, 300}; }
    int spectrumPixelHeight() const;

    // Set the frequency range covered by this panadapter.
    void setFrequencyRange(double centerMhz, double bandwidthMhz);
    void clearDisplay();  // blank spectrum and waterfall on disconnect
    void resetGpuResources();  // tear down GPU pipelines for reparenting (#1240)
    void prepareForTopLevelChange(); // unregister QRhiWidget from the current backing-store QRhi
    void prepareForShutdown(); // tear down QRhi/native resources before QWidget backing store destruction
    QString rendererDescription() const;
    void setConnectionAnimationVisible(bool on, const QString& label = {});
    void setKiwiSdrConnectionOverlay(bool visible,
                                     const QString& detail = {},
                                     const QString& title = {});
    void showInterlockNotification(const QString& message, int durationMs = 5000);

    // Feed a new FFT frame. bins are scaled dBm values.
    void updateSpectrum(const QVector<float>& binsDbm);

    // Feed a single waterfall row from a VITA-49 waterfall tile.
    // lowFreqMhz/highFreqMhz describe the tile's frequency span.
    // When waterfall tile data is available, this is used instead of
    // the FFT-derived waterfall rows from updateSpectrum().
    void updateWaterfallRow(const QVector<float>& binsDbm,
                            double lowFreqMhz, double highFreqMhz,
                            quint32 timecode = 0);
    void setKiwiSdrWaterfallAvailable(bool available);
    void setKiwiSdrWaterfallActive(bool active);
    bool kiwiSdrWaterfallActive() const { return m_kiwiSdrWaterfallActive; }
    void setKiwiSdrWaterfallProfile(const QString& profileId);
    void clearKiwiSdrWaterfallRows();
    void clearKiwiSdrWaterfallRowsForProfile(const QString& profileId);
    void setKiwiSdrWaterfallAdjustments(int cellDb, int floorDb);
    void updateKiwiSdrWaterfallRow(const QVector<float>& binsDbm,
                                   double lowFreqMhz, double highFreqMhz,
                                   quint32 timecode = 0);

    // Update the dBm range used for the waterfall colour map and spectrum Y axis.
    void setDbmRange(float minDbm, float maxDbm);

    // Noise floor auto-adjust: position (1=top, 99=bottom), enable on/off.
    // Both setters persist to AppSettings (per-pan keys DisplayNoiseFloor*)
    // so the state and value survive launch.
    void setNoiseFloorPosition(int pos);
    void setNoiseFloorEnable(bool on);
    void prepareForFftScaleChange();
    void suspendNoiseFloorAutoAdjustUntil(qint64 untilMs);
    void resumeNoiseFloorAutoAdjust();
    void reacquireNoiseFloorLock();

    // Two-pass trimmed-mean noise floor from live FFT bins (dBm), EMA-smoothed.
    // Pass 1 computes the overall mean; pass 2 averages only bins ≤ mean so
    // signal peaks exclude themselves, leaving the flat noise baseline.
    // Reflects the current band, antenna and preamp — no hardcoded dBm value.
    float noiseFloorDbm() const { return m_measuredNoiseFloorDbm; }

    // Flex squelch threshold overlay line. level is the radio squelch_level
    // (0-100), mapped to absolute dBm via the radio's fixed scale:
    // dBm = -160 + level. (Empirically verified on FLEX-8600 fw 4.1.5.)
    void setSquelchLine(bool visible, int level);
    // KiwiSDR SQL is a dB margin above Kiwi's median noise-floor estimate.
    // marginDb is the server margin, not the UI slider value.
    void setKiwiSdrSquelchLine(bool visible, int marginDb, bool floorRelative);
    void setKiwiSdrSquelchMeterDbm(float dbm, bool squelched);
    void clearKiwiSdrSquelchLine();

    // When enabled, measures the noise floor on every FFT frame using a
    // two-pass trimmed mean (pass 1: overall mean; pass 2: mean of bins
    // at or below pass-1 mean to exclude signal peaks).  An EMA (α=0.1)
    // smooths frame-to-frame variation.  Emits autoSquelchLevelSuggested()
    // with a squelch level just above the smoothed floor.
    void setAutoSquelchEnable(bool on);

    // Margin above the EMA-smoothed noise floor for auto-squelch suggestion
    // (5-20 dB, default 10).  User-tunable via Display > SQL Margin.
    void setAutoSqlMarginDb(int dB);

    // (getters for display settings are below with their members)

    // Set the VFO frequency (draws the orange VFO marker).
    void setVfoFrequency(double freqMhz);

    // Set the filter edges (Hz offsets from VFO frequency).
    void setVfoFilter(int lowHz, int highHz);

    // Getters for band settings capture.
    float spectrumFrac()  const { return m_spectrumFrac; }
    float refLevel()      const { return m_refLevel; }
    float dynamicRange()  const { return m_dynamicRange; }
    bool isDraggingDbmScale() const { return m_draggingDbm || m_draggingDbmRange; }
    bool pendingAutoNoiseFloorDbmRange() const {
        return m_pendingDbmRangeEcho && m_pendingDbmRangeEchoFromAutoFloor;
    }
    bool noiseFloorAutoAdjustEnabled() const { return m_noiseFloorEnable; }
    double centerMhz()    const { return m_centerMhz; }
    double bandwidthMhz() const { return m_bandwidthMhz; }

    // Set the FFT/waterfall split ratio programmatically.
    void setSpectrumFrac(float f);

    // Get/set the click/scroll tuning step size in Hz (default 100).
    int stepSize() const { return m_stepHz; }
    void setStepSize(int hz) { m_stepHz = hz; }

    // Set panadapter bandwidth zoom limits (MHz). Called per-radio model.
    void setBandwidthLimits(double minMhz, double maxMhz) { m_minBwMhz = minMhz; m_maxBwMhz = maxMhz; }

    // Set the per-mode filter limits (Hz). Called when mode changes.
    void setFilterLimits(int minHz, int maxHz) { m_filterMinHz = minHz; m_filterMaxHz = maxHz; }

    // Set the current demod mode (for zoom centering behavior).
    void setMode(const QString& mode) { m_mode = mode; }

    // Lean render mode (#3283): skip the wallpaper layer (render an opaque
    // single layer), drop the translucent FFT fill, and cap the spectrum and
    // waterfall data-driven repaints to ~30 Hz (kLeanFrameMs = 33). The cap is
    // applied via leanCappedUpdate() only on the two high-frequency entry
    // points (updateSpectrum, updateWaterfallRow); interactive paths (cursor
    // moves, marker drags, etc.) keep their immediate update() calls so input
    // feels snappy. Reversible — no persisted state is destroyed.
    void setLeanMode(bool on);
    bool leanMode() const { return m_leanMode; }


    // Access the floating overlay menu (for wiring signals).
    SpectrumOverlayMenu* overlayMenu() const { return m_overlayMenu; }

    // Access VFO info widgets (one per slice).
    VfoWidget* vfoWidget() const { return m_vfoWidget; }  // active slice (compat)
    VfoWidget* vfoWidget(int sliceId) const;
    VfoWidget* addVfoWidget(int sliceId);
    void       removeVfoWidget(int sliceId);
    void       setActiveVfoWidget(int sliceId);
    // True if the slice has a split partner whose own VFO flag is rendered on
    // the opposite side via LockLeft / LockRight.  panFollowVfo() uses this
    // to extend the pan-follow trigger on both sides so neither flag clips
    // the pan edge (#2761).
    bool sliceHasSplitPartner(int sliceId) const;

    // WNB and RF gain state for on-screen indicators.
    bool wnbActive()   const { return m_wnbActive; }
    bool wnbUpdating() const { return m_wnbUpdating; }
    int  rfGainValue() const { return m_rfGainValue; }
    bool wideActive()  const { return m_wideActive; }
    void setWnbActive(bool on) { syncWnbState(on, 0, false); }
    void syncWnbState(bool on, int level, bool updating) {
        Q_UNUSED(level);
        if (m_wnbActive != on || m_wnbUpdating != updating) {
            m_wnbActive = on;
            m_wnbUpdating = updating;
            markOverlayDirty();
        }
    }
    void setRfGain(int gain) {
        if (m_rfGainValue != gain) {
            m_rfGainValue = gain;
            reacquireNoiseFloorLock();
        }
        markOverlayDirty();
    }
    void setWideActive(bool on) {
        if (m_wideActive != on) {
            m_wideActive = on;
            markOverlayDirty();
        }
    }

    // HF propagation forecast overlay (K-index, A-index, and Solar Flux Index).
    // Values of -1 mean not yet fetched; visible only when enabled.
    void setPropForecastVisible(bool on) { m_propForecastVisible = on; markOverlayDirty(); }
    void setPropForecast(double kIndex, int aIndex, int sfi) {
        m_propKIndex = kIndex;
        m_propAIndex = aIndex;
        m_propSfi = sfi;
        markOverlayDirty();
    }
    bool propForecastVisible() const { return m_propForecastVisible; }

    // MQTT device status overlay (#699)
    void setMqttDisplayValue(const QString& key, const QString& value) {
        m_mqttDisplayValues[key] = value; markOverlayDirty();
    }
    void clearMqttDisplay() { m_mqttDisplayValues.clear(); markOverlayDirty(); }

    // NB Waterfall Blanker (#277) — client-side impulse suppression
    void setWfBlankerEnabled(bool on);
    void setWfBlankerThreshold(float t);
    void setWfBlankerMode(int mode);  // 0=Fill, 1=Interpolate
    bool  wfBlankerEnabled()   const { return m_wfBlankerEnabled; }
    float wfBlankerThreshold() const { return m_wfBlankerThreshold; }
    int   wfBlankerMode()      const { return m_wfBlankerMode; }
    void setShowBandPlan(bool on) { m_bandPlanFontSize = on ? 6 : 0; update(); }
    void setBandPlanFontSize(int pt) { m_bandPlanFontSize = pt; update(); }
    void setBandPlanShowSpots(bool on) { m_bandPlanShowSpots = on; update(); }
    bool bandPlanShowSpots() const { return m_bandPlanShowSpots; }
    void setBandPlanManager(class BandPlanManager* mgr);
    void setSingleClickTune(bool on) { m_singleClickTune = on; }
    void setShowCursorFreq(bool on) { m_showCursorFreq = on; update(); }
    bool showCursorFreq() const { return m_showCursorFreq; }
    void setShowFpsMeters(bool on);
    bool showFpsMeters() const { return m_showFpsMeters; }
    void setShowTuneGuides(bool on);
    bool showTuneGuides() const { return m_showTuneGuides; }
    void setExtendedFrequencyLine(bool on);
    bool extendedFrequencyLine() const { return m_extendedFrequencyLine; }
    void setFloating(bool on) { m_isFloating = on; }
    void setBackgroundImage(const QString& path);
    QString backgroundImagePath() const { return m_bgImagePath; }
    void setBackgroundOpacity(int pct) { m_bgOpacity = qBound(0, pct, 100); markOverlayDirty(); }
    int backgroundOpacity() const { return m_bgOpacity; }
    void setBackgroundFillColor(const QColor& c);
    QColor backgroundFillColor() const { return m_bgFillColor; }
    bool showBandPlan() const { return m_bandPlanFontSize > 0; }
    int  bandPlanFontSize() const { return m_bandPlanFontSize; }

    // ── Display control setters ───────────────────────────────────────────
    // FFT controls (save to AppSettings on each change)
    void setFftAverage(int frames);
    void setFftWeightedAvg(bool on);
    void setFftFps(int fps);
    void setFftFillAlpha(float a);
    void setFftFillColor(const QColor& c);
    void setFftHeatMap(bool on);
    void setShowGrid(bool on);
    void setFreqGridSpacing(int khz);
    void setFreqScaleFontPt(int pt);
    void setFftLineWidth(float w);
    float fftFillAlpha() const         { return m_fftFillAlpha; }
    QColor fftFillColor() const        { return m_fftFillColor; }
    bool fftHeatMap() const            { return m_fftHeatMap; }
    bool showGrid() const              { return m_showGrid; }
    int  freqGridSpacing() const       { return m_freqGridSpacingKhz; }
    int  freqScaleFontPt() const       { return m_freqScaleFontPt; }
    float fftLineWidth() const         { return m_fftLineWidth; }
    int   fftAverage() const           { return m_fftAverage; }
    int   fftFps() const               { return m_fftFps; }
    bool  fftWeightedAvg() const       { return m_fftWeightedAvg; }

    // Waterfall controls (save to AppSettings on each change)
    void setWfColorGain(int gain);
    void setWfBlackLevel(int level);
    void setWfAutoBlack(bool on);
    // Auto-black offset (0-100, 50 = noise floor, <50 darker, >50 lighter).
    // Only consulted while m_wfAutoBlack is on; lets users bias the noise-
    // floor target without leaving auto-black.
    void setWfAutoBlackOffset(int level);
    // Radio-computed auto-black level from the latest waterfall tile (raw uint16
    // domain, radio-authoritative). 0 = not yet received → the client falls back
    // to its own noise-floor estimate.
    void setRadioAutoBlackLevel(quint32 rawLevel);
    // Auto-black source: false = client-side noise-floor estimate (default,
    // legacy look); true = the radio's per-tile auto-black level. Only consulted
    // while m_wfAutoBlack is on.
    void setWfAutoBlackRadioSide(bool radioSide);
    void setWfLineDuration(int ms);
    void setWfColorScheme(int scheme);
    void resetWfTimeScale();
    int   wfColorGain() const          { return m_wfColorGain; }
    int   wfBlackLevel() const         { return m_wfBlackLevel; }
    bool  wfAutoBlack() const          { return m_wfAutoBlack; }
    int   wfAutoBlackOffset() const    { return m_wfAutoBlackOffset; }
    bool  wfAutoBlackRadioSide() const { return m_wfAutoBlackRadioSide; }
    int   wfLineDuration() const       { return m_wfLineDuration; }
    int   wfColorScheme() const        { return static_cast<int>(m_wfColorScheme); }
    int   noiseFloorPosition() const   { return m_noiseFloorPosition; }
    bool  noiseFloorEnabled() const    { return m_noiseFloorEnable; }

    // Set slice info for the off-screen VFO indicator (legacy single-slice).
    void setSliceInfo(int sliceId, bool isTxSlice);

    // ── Multi-slice overlay API ───────────────────────────────────────────
    struct SliceOverlay {
        int    sliceId{0};
        double freqMhz{0};
        int    filterLowHz{0};
        int    filterHighHz{0};
        bool   isTxSlice{false};
        bool   isActive{false};
        int    splitPartnerId{-1};  // slice ID of split partner, -1 if not in split
        QString mode;               // "RTTY", "USB", etc.
        int    rttyMark{2125};      // RTTY mark audio offset (Hz)
        int    rttyShift{170};      // RTTY shift (Hz)
        bool   ritOn{false};
        int    ritFreq{0};          // Hz offset
        bool   xitOn{false};
        int    xitFreq{0};          // Hz offset
        // Per-slice VFO marker display preferences (#1526).
        // markerWidth: 0 = off (no center line / triangle, passband only),
        // 1 = 1 px, 3 = 3 px.
        int    markerWidth{1};
        bool   filterEdgesHidden{false};  // skip drawing filter-edge vertical lines
        QString perClientLetter;   // radio-provided index_letter (Multi-Flex)
    };

    // Add or update a slice overlay (called per-slice on any state change).
    bool isDraggingFilter() const { return m_draggingFilter != FilterEdge::None; }
    void setSliceOverlay(int sliceId, double freq, int fLow, int fHigh,
                         bool tx, bool active, const QString& mode = {},
                         int rttyMark = 2125, int rttyShift = 170,
                         bool ritOn = false, int ritFreq = 0,
                         bool xitOn = false, int xitFreq = 0);
    // Update just the frequency on an existing overlay (for optimistic scroll-to-tune)
    void setSliceOverlayFreq(int sliceId, double freqMhz);
    // Update the per-client letter on an existing overlay; safe to call
    // before/after setSliceOverlay.  Used by the Multi-Flex display mode
    // so the slice marker / passband colour can follow the radio's
    // index_letter assignment (#2606).
    void setSliceOverlayLetter(int sliceId, const QString& letter);
    // Update per-slice marker display style (#1526)
    void setSliceOverlayMarkerStyle(int sliceId, int markerWidth, bool filterEdgesHidden);
    // Remove a slice overlay.
    void removeSliceOverlay(int sliceId);

    // Mark two slices as a split pair (RX + TX). Pass -1 to clear.
    void setSplitPair(int rxSliceId, int txSliceId);

    // ── TNF overlay ─────────────────────────────────────────────────────
    struct TnfMarker {
        int    id;
        double freqMhz;
        int    widthHz;
        int    depthDb;
        bool   permanent;
    };
    void setTnfMarkers(const QVector<TnfMarker>& markers);
    void setTnfGlobalEnabled(bool on);

    struct SpotMarker {
        int    index;
        QString callsign;
        double freqMhz;
        QString color;       // #AARRGGBB or empty for default
        QString mode;
        QColor  dxccColor;   // DXCC-aware color from DxccColorProvider (#330)
        QString source;
        QString spotterCallsign;
        QString comment;
        qint64  timestampMs{0};
        // Protocol-supplied pill color (#AARRGGBB). Honored only when
        // Override Background is off — see drawSpotMarkers().
        QString backgroundColor;
    };
    void setSpotMarkers(const QVector<SpotMarker>& markers);

    struct SpotCluster {
        QRect rect;
        QVector<SpotMarker> spots;
    };

    struct SwrSweepPoint {
        double freqMhz{0.0};
        float swr{1.0f};
    };
    void setSwrSweepPoints(const QVector<SwrSweepPoint>& points,
                           bool running = false,
                           double currentFreqMhz = -1.0,
                           const QString& sourceLabel = {});
    void clearSwrSweepPoints();

    void setShowSpots(bool on) { m_showSpots = on; m_hoveredSpotKey.clear(); update(); }
    bool showSpots() const { return m_showSpots; }
    void setShowSHistory(bool on)    { m_showSHistory = on;    update(); }
    bool showSHistory() const         { return m_showSHistory; }
    void setShowSHistoryQrm(bool on) { m_showSHistoryQrm = on; update(); }
    bool showSHistoryQrm() const      { return m_showSHistoryQrm; }
    // Smart Spot Filtering: dim SSB/voice spots whose frequency is not within
    // ±1 kHz of a live S-History detection.  Once matched, a spot stays at full
    // opacity for 2 minutes after its last confirmation.  CW/digital spots are
    // always shown at full opacity regardless of this setting.
    void setSmartSpotFilter(bool on, qint64 enabledMs = 0) {
        if (on && !m_smartSpotFilter)
            m_smartSpotFilterEnabledMs = (enabledMs > 0) ? enabledMs
                                                         : QDateTime::currentMSecsSinceEpoch();
        m_smartSpotFilter = on;
        update();
    }
    bool smartSpotFilter() const     { return m_smartSpotFilter; }
    void setSmartSpotFilterOpacity(int pct) { m_smartSpotFilterOpacity = std::clamp(pct, 0, 100); update(); }
    void setSmartSpotFilterDelayS(int s)    { m_smartSpotFilterDelayS  = std::max(0, s); }
    // Match window between a DX-cluster spot and an S-History voice
    // detection (Hz, clamped to 100–5000).  ±this many Hz around each
    // S-History center counts as a match.  Tight = fewer false confirms
    // on crowded phone bands; loose = better tolerance for cluster
    // operators who spot the QRG they tuned through rather than the
    // exact carrier.  (#2609)
    void setSmartSpotFilterMatchHz(int hz)  { m_smartSpotFilterMatchHz = std::clamp(hz, 100, 5000); }
    // When on, click-to-tune on a SHistory/QRM marker rounds the target to
    // the nearest multiple of stepSize().  Compensates for the inherent
    // detector edge-bin imprecision (typically 100–300 Hz off carrier).
    void setSHistorySnapToStep(bool on) { m_sHistorySnapToStep = on; }
    bool sHistorySnapToStep() const     { return m_sHistorySnapToStep; }
    void setSHistoryMarkers(const QVector<SpotMarker>& markers);
    void setSpotFontSize(int px) { m_spotFontSize = px; markOverlayDirty(); }
    void setSpotMaxLevels(int n) { m_spotMaxLevels = n; markOverlayDirty(); }
    void setSpotStartPct(int pct) { m_spotStartPct = pct; markOverlayDirty(); }
    void setSpotOverrideColors(bool on) { m_spotOverrideColors = on; markOverlayDirty(); }
    void setSpotOverrideBg(bool on) { m_spotOverrideBg = on; markOverlayDirty(); }
    void setSpotShowLines(bool on) { m_spotShowLines = on; markOverlayDirty(); }
    bool spotShowLines() const { return m_spotShowLines; }
    void setSpotColor(const QColor& c) { m_spotColor = c; markOverlayDirty(); }
    void setSpotBgColor(const QColor& c) { m_spotBgColor = c; markOverlayDirty(); }
    void setSpotBgOpacity(int pct) { m_spotBgOpacity = pct; markOverlayDirty(); }
    void setTransmitting(bool tx);
    void setShowTxInWaterfall(bool on) { m_showTxInWaterfall = on; }
    void setHasTxSlice(bool has) { m_hasTxSlice = has; }
    void setTxWaterfallSlice(double freqMhz, int filterLowHz, int filterHighHz,
                             bool xitOn, int xitFreq);
    void clearTxWaterfallSlice();

signals:
    // Emitted when auto-squelch computes a new suggested level (0-100 radio units).
    // Connect to SliceModel::setSquelch and setSquelchLine to apply.
    void autoSquelchLevelSuggested(int level);

    // Emitted when user clicks on an inactive slice marker.
    void sliceClicked(int sliceId);
    // Emitted when the user requests an absolute jump in the panadapter area.
    void frequencyClicked(double mhz);
    // Emitted when the user makes an incremental tuning gesture such as
    // wheel tuning or VFO drag.
    void incrementalTuneRequested(double mhz);
    // Edge auto-pan step: pan the view to newCenterMhz AND tune the slice to
    // sliceFreqMhz in one shot, WITHOUT triggering pan-follow/reveal (which is
    // already accounted for by the explicit center).  Keeps the dragged slice
    // pinned under the cursor while the band scrolls.  (user-reported)
    void edgePanTuneRequested(double newCenterMhz, double sliceFreqMhz);
    void spotTriggered(int spotIndex);
    // Emitted when the user changes both center and bandwidth as one explicit
    // pan/zoom operation and the radio should apply them coherently. Splitting
    // those into separate commands was a known source of waterfall edge loss
    // and zoom drift during bandwidth drag / keyboard zoom.
    void frequencyRangeChangeRequested(double newCenterMhz, double newBandwidthMhz);
    void frequencyRangeChanged(double centerMhz, double bandwidthMhz);
    // Emitted when the user drags the frequency scale bar to change bandwidth.
    void bandwidthChangeRequested(double newBandwidthMhz);
    // Band/segment zoom: radio handles center/bandwidth (SmartSDR pcap: "band_zoom=1" / "segment_zoom=1")
    void bandZoomRequested();
    void segmentZoomRequested();
    // Emitted when the user drags the waterfall to pan the center frequency.
    void centerChangeRequested(double newCenterMhz);
    // Emitted when the user drags a filter edge to resize the passband.
    void filterChangeRequested(int lowHz, int highHz);
    // Emitted when the user adjusts the dBm scale (drag or arrows).
    void dbmRangeChangeRequested(float minDbm, float maxDbm);
    void dbmRangeDragFinished(float minDbm, float maxDbm);
    void noiseFloorPositionResolved(int pos);
    void waterfallLineDurationChangeRequested(int ms);
    // TNF signals
    void tnfCreateRequested(double freqMhz);
    void tnfMoveRequested(int id, double newFreqMhz);
    void tnfRemoveRequested(int id);
    void tnfWidthRequested(int id, int widthHz);
    void tnfDepthRequested(int id, int depthDb);
    void tnfPermanentRequested(int id, bool permanent);
    void sliceCreateRequested(double freqMhz);
    void sliceCloseRequested(int sliceId);
    void propForecastClicked();  // click on K/A/SFI overlay text
    void sliceTuneRequested(int sliceId, double freqMhz);
    void popOutRequested(bool popOut);  // true=float, false=dock
    void sliceTxRequested(int sliceId);
    // Emitted when FFT bin-mapping dimensions change so MainWindow can re-push
    // xpixels/ypixels to the radio (#1511).
    void dimensionsChanged(int w, int h);
    // Spot signals
    void spotAddRequested(double freqMhz, const QString& callsign,
                          const QString& comment, int lifetimeSec,
                          bool forwardToCluster);
    void spotRemoveRequested(int spotIndex);

protected:
#ifdef AETHER_GPU_SPECTRUM
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
#else
    void paintEvent(QPaintEvent* event) override;
#endif
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void leaveEvent(QEvent* event) override;

public:
    void showAddSpotDialog(double freqMhz);

    // Starstruck easter egg: Ctrl+Shift+A toggles the panadapter pan-drag sound.
    static void toggleStarstruckMode();

private:
    double effectiveGridStepMhz(int widgetWidth) const;
    void drawGrid(QPainter& p, const QRect& r);
    void drawSpectrum(QPainter& p, const QRect& r);
    void drawSliceMarkers(QPainter& p, const QRect& specRect, const QRect& wfRect);
    // Draw each flag's SmartMTR extremes value labels on top of the slice markers.
    void drawSmartMtrValueLabels(QPainter& p);
    void drawOffScreenSlices(QPainter& p, const QRect& specRect);
    void drawBandPlan(QPainter& p, const QRect& specRect);
    void drawTnfMarkers(QPainter& p, const QRect& specRect);
    void drawSpotMarkers(QPainter& p, const QRect& specRect);
    void drawSwrSweep(QPainter& p, const QRect& specRect);
    void drawAutoSqlFloor(QPainter& p, const QRect& specRect);
    void drawSquelchLine(QPainter& p, const QRect& specRect);
    void updateAutoSquelchFromBins(const QVector<float>& binsDbm);
    QRect leftOccludedRect() const;
    void showSpotClusterPopup(const SpotCluster& cluster, const QPoint& globalPos);
    const TnfMarker* tnfMarkerById(int id) const;
    QColor tnfColor(const TnfMarker& tnf) const;
    QColor tnfFillColor(const TnfMarker& tnf) const;
    QColor tnfLineColor(const TnfMarker& tnf) const;
    int  tnfAtPixel(int x, int preferredId = -1) const;
    bool sliceCursorShapeAt(const QPoint& localPos, Qt::CursorShape& shape) const;
    bool spectrumDefaultsToCrosshairAt(const QPoint& localPos) const;
    void installVfoCursorEventFilter(VfoWidget* widget);
    void setVfoCursorOverride(Qt::CursorShape shape);
    void clearVfoCursorOverride();
#ifdef AETHER_GPU_SPECTRUM
    void repositionVfoFlags(const QRect& specRect);  // #3617 — runs before flag render
    void updateLiveFlag();   // #3617 — pick which flag is live (hover/focus/popup)
    void setLiveFlag(int sliceId);   // #3617 — show one flag live, snapshot the rest
    void updateFlagRefreshTimer();        // #3746 — run the re-raster timer only while presented
    void attachFlagTimerWindowWatcher();  // #3746 — watch the top-level for minimize/restore
#endif
    void setSpectrumCursor(Qt::CursorShape shape);
    void updateTrackedCursorState(const QPoint& localPos, bool insideWidget);
    void updateTnfHoverPopup();
    void drawWaterfall(QPainter& p, const QRect& r);
    void createFpsMeterLabels();
    void updateFpsMeterLabels();
    void positionFpsMeterLabels();
    void positionZoomButtons();
    void drawFreqScale(QPainter& p, const QRect& r);
    void drawDbmScale(QPainter& p, const QRect& specRect);
    void drawTimeScale(QPainter& p, const QRect& wfRect);
    void drawConnectionAnimation(QPainter& p, const QRect& contentRect);
    void drawKiwiSdrConnectionOverlay(QPainter& p, const QRect& contentRect);
    void positionInterlockNotification();
    int waterfallStripWidth() const;
    QRect waterfallLiveButtonRect(const QRect& wfRect) const;
    QRect waterfallTimeScaleRect(const QRect& wfRect) const;
    void ensureWaterfallHistory();
    void rebuildWaterfallViewport();
    void rebuildWaterfallViewportForFrame(double centerMhz, double bandwidthMhz);
    void setWaterfallLive(bool live);
    void handleWaterfallFrequencyFrameChange(double oldCenterMhz,
                                             double oldBandwidthMhz,
                                             double newCenterMhz,
                                             double newBandwidthMhz);
    struct WaterfallStreamState {
        QImage waterfall;
        int wfWriteRow{0};
        QImage waterfallHistory;
        QVector<qint64> historyTimestamps;
        int historyWriteRow{0};
        int historyRowCount{0};
        int historyOffsetRows{0};
        QVector<double> historyRowCenterMhz;
        QVector<double> historyRowBwMhz;
        bool live{true};
        int rowsSinceRateChange{0};
        QVector<QRgb> prevTileScanline;
        QVector<float> kiwiFftTrace;
        QVector<float> kiwiLastWaterfallBins;
        double kiwiLastWaterfallCenterMhz{0.0};
        double kiwiLastWaterfallBandwidthMhz{0.0};
        bool kiwiLastWaterfallFrameValid{false};
        float kiwiAutoFloorDbm{-130.0f};
        float kiwiAutoCeilDbm{-50.0f};
        bool kiwiAutoRangeValid{false};
        bool valid{false};
    };
    void clearCurrentWaterfallRows();
    void saveCurrentWaterfallStreamState();
    void restoreCurrentWaterfallStreamState();
    WaterfallStreamState& activeKiwiWaterfallState();
    bool beginWaterfallStreamWrite(bool kiwiStream);
    void endWaterfallStreamWrite(bool kiwiStream, bool visibleStream);
    void appendHistoryRow(const QRgb* rowData, qint64 timestampMs,
                          double frameCenterMhz = -1.0,
                          double frameBandwidthMhz = -1.0);
    void appendVisibleRow(const QRgb* rowData);
    int waterfallHistoryCapacityRows() const;
    int maxWaterfallHistoryOffsetRows() const;
    int historyRowIndexForAge(int ageRows) const;
    QString pausedTimeLabelForAge(int ageRows) const;
    void updateWaterfallMsPerRowFromHistory();
    int waterfallFallbackIntervalMs() const;
    int waterfallFallbackTimeoutMs() const;
    int nativeWaterfallFallbackHoldMs(int intervalMs) const;
    void updateNativeWaterfallFallbackState(qint64 nowMs);
    bool pushRxWaterfallFallbackIfDue(const QVector<float>& bins, qint64 nowMs);
    void applyFpsMeterVisibility(bool on);
    void resetFpsMeterWindow();
    void updateFpsMeterValues();
    void recordPanadapterFrame();
    void recordWaterfallFrame(int rows = 1);
    void recordKiwiSdrWaterfallFrame(int rows = 1);
    bool anyDragActive() const;
    void publishPerfDragState() const;

    // Two-pass trimmed-mean noise-floor estimator: pass 1 takes the
    // overall mean across the bins (stride-sampled for speed), pass 2
    // averages only bins ≤ that mean.  Signal peaks inflate the pass-1
    // mean and therefore exclude themselves from pass 2, leaving the
    // flat noise baseline that a human eye reads as the "noise floor"
    // on the scope.
    float estimateNoiseFloorDbm(const QVector<float>& bins) const;

    // Update the smoothed-baseline tracker for the noise-floor auto-
    // adjust path.  Per-frame, asymmetric smoothing (drops follow
    // quickly, rises slowly), with a candidate-state transient filter
    // so brief upward spikes (lightning crashes) don't pull the lock.
    bool updateNoiseFloorBaseline(const QVector<float>& bins, bool forceBaseline);
    float estimateKiwiSdrVisualNoiseFloorDbm(const QVector<float>& bins) const;
    void updateKiwiSdrSquelchVisualFloor(float floorDbm);
    void pinKiwiSdrManualSquelchLine();
    // Adjust m_refLevel toward the target so the smoothed noise floor
    // sits at m_noiseFloorPosition.  Pans the dB range (keeps span
    // fixed) rather than zooming it (existing zoom-when-floor-moves
    // semantic was jarring — span changes shifted signal visual heights
    // every time the floor drifted).
    void applyNoiseFloorAutoAdjust(qint64 nowMs);
    bool noiseFloorAutoAdjustHeld(qint64 nowMs);
    void armNoiseFloorFastLock(int freshFrames, int snapFrames);
    void moveRefLevelToward(float targetRef, qint64 nowMs);
    void sendNoiseFloorRangeCommand(qint64 nowMs, bool force);
    void clearDbmReleaseRebase();
    // Reset the baseline tracker — called on any input change (zoom,
    // band switch, manual dBm drag) so the next frame re-acquires
    // rather than smooths from a stale value.
    void resetNoiseFloorBaseline();
    void reacquireNoiseFloorLockFromVisibleSource();
    // Re-capture the target frac. Explicit user changes (slider/right dBm bar)
    // can persist; startup/enable/layout refreshes only rebuild transient state.
    void refreshNoiseFloorTarget(bool captureCurrentScale = false, bool persistCapture = false);
    bool captureNoiseFloorTargetFromCurrentScale(bool notify, bool persist);

    // Helper: find overlay index for a sliceId, or -1.
    int overlayIndex(int sliceId) const;
    // Helper: find active overlay (or nullptr).
    const SliceOverlay* activeOverlay() const;
    // Helper: find TX overlay (or nullptr).
    const SliceOverlay* txOverlay() const;
    bool txWaterfallMaskRange(double& lowMhz, double& highMhz) const;
    bool txWaterfallAffectsThisPan() const;
    void beginTxDbmRangeFreeze();
    void endTxDbmRangeFreeze();
    void resetTxDbmRangeFreeze();
    void deferTxDbmRange(float minDbm, float maxDbm);
    void applyDbmRangeImmediate(float minDbm, float maxDbm);
    void reprojectBinsToFrozenTxDbmRange(QVector<float>& bins) const;
    void clearWaterfallRows();
    QVector<float> smoothKiwiSdrWaterfallBins(const QVector<float>& bins);
    void updateKiwiSdrAutoColorRange(const QVector<float>& bins);
    const QVector<float>& displaySpectrumBins() const;
    const QVector<float>& noiseFloorAutoLevelBins() const;

    void pushWaterfallRow(const QVector<float>& bins, int destWidth,
                          double tileLowMhz = -1, double tileHighMhz = -1);
    void pushKiwiSdrWaterfallRow(const QVector<float>& bins, int destWidth,
                                 double rowCenterMhz, double rowBandwidthMhz);
    QRgb dbmToRgb(float dbm) const;
    QRgb kiwiSdrLevelToRgb(float level) const;
    QRgb intensityToRgb(float intensity) const;  // for native waterfall tiles

    // Pixel x coordinate for a given frequency in MHz (0 = left edge).
    int mhzToX(double mhz) const;
    // Convert pixel x back to MHz.
    double xToMhz(int x) const;

    QVector<float> m_bins;       // raw FFT frame (dBm)
    QVector<float> m_smoothed;   // exponential-smoothed for visual stability
    QVector<float> m_kiwiSdrFftTrace;  // Kiwi-derived FFT trace, kept separate from Flex FFT
    bool m_shutdownPrepared{false};
    bool m_kiwiSdrWaterfallAvailable{false};
    bool m_kiwiSdrWaterfallActive{false};
    bool m_kiwiSdrConnectionOverlayVisible{false};
    QString m_kiwiSdrConnectionOverlayTitle;
    QString m_kiwiSdrConnectionOverlayDetail;
    WaterfallStreamState m_nativeWaterfallState;
    WaterfallStreamState m_kiwiWaterfallState;
    QHash<QString, WaterfallStreamState> m_kiwiProfileWaterfallStates;
    QString m_kiwiSdrWaterfallProfileId;
    QVector<float> m_kiwiSdrLastWaterfallBins;
    double m_kiwiSdrLastWaterfallCenterMhz{0.0};
    double m_kiwiSdrLastWaterfallBandwidthMhz{0.0};
    bool m_kiwiSdrLastWaterfallFrameValid{false};
    float m_kiwiSdrAutoFloorDbm{-130.0f};
    float m_kiwiSdrAutoCeilDbm{-50.0f};
    bool m_kiwiSdrAutoRangeValid{false};
    int m_kiwiSdrWaterfallCellDb{0};
    int m_kiwiSdrWaterfallFloorDb{0};

    double m_centerMhz{14.225};
    double m_bandwidthMhz{0.200};
    // Pan-follow smooth animation (#989): animates m_centerMhz toward the target
    // for small nudges so the VFO widget glides instead of snapping.
    QVariantAnimation* m_panCenterAnim{nullptr};
    double             m_panCenterTarget{14.225};
    double             m_panCenterStart{14.225}; // m_centerMhz at animation start (stale-echo guard)

    // Multi-slice overlays (replaces single m_vfoFreqMhz / m_filterLowHz / etc.)
    QVector<SliceOverlay> m_sliceOverlays;

    int    m_filterMinHz{-12000};  // per-mode lower bound (active slice)
    int    m_filterMaxHz{12000};   // per-mode upper bound (active slice)
    QString m_mode{"USB"};         // current demod mode (active slice)

    float m_refLevel{-50.0f};       // top of display (dBm)
    float m_dynamicRange{100.0f};   // dB range shown in spectrum (-50 to -150)
    bool  m_resetFftSmoothingOnNextFrame{false};
    bool  m_pendingDbmRangeEcho{false};
    bool  m_pendingDbmRangeEchoFromAutoFloor{false};
    qint64 m_pendingDbmRangeEchoStartMs{0};
    int   m_holdFftUpdatesAfterDbmRelease{0};
    float m_dbmReleasePreviewOldMinDbm{0.0f};
    float m_dbmReleasePreviewOldMaxDbm{0.0f};
    float m_dbmReleasePreviewNewMinDbm{0.0f};
    float m_dbmReleasePreviewNewMaxDbm{0.0f};
    float m_pendingMinDbm{0.0f};
    float m_pendingMaxDbm{0.0f};

    // Two-pass trimmed-mean noise floor (dBm), EMA-smoothed across ~20 frames.
    // -1000 = cold start (not yet measured).
    float m_measuredNoiseFloorDbm{-1000.0f};

    // Noise floor auto-adjust
    bool  m_noiseFloorEnable{false};
    int   m_noiseFloorPosition{75};  // 1=top, 99=bottom
    int   m_noiseFloorFrameCount{0};
    // Noise-floor auto-adjust state machine (per-frame baseline tracker
    // with asymmetric smoothing + transient rejection — keeps the floor
    // visually pinned at m_noiseFloorPosition without chasing lightning
    // crashes).
    bool   m_noiseFloorBaselineValid{false};
    bool   m_noiseFloorTargetValid{false};
    float  m_noiseFloorBaselineDbm{-1000.0f};
    float  m_noiseFloorTargetFrac{0.75f};
    qint64 m_noiseFloorLastSampleMs{0};
    qint64 m_noiseFloorLastMotionMs{0};
    qint64 m_noiseFloorLastCommandMs{0};
    qint64 m_noiseFloorScaleSettlingUntilMs{0};
    qint64 m_noiseFloorAutoAdjustHoldUntilMs{0};
    float  m_noiseFloorLastCommandRef{-1000.0f};
    bool   m_noiseFloorCandidateValid{false};
    float  m_noiseFloorCandidateDbm{-1000.0f};
    qint64 m_noiseFloorCandidateStartMs{0};
    int    m_noiseFloorCandidateFrames{0};
    int    m_noiseFloorFreshFrameCount{0};
    int    m_noiseFloorFastLockFrames{0};

    // Percentile EWMA used for the amber floor overlay line and auto-squelch.
    // Tracked separately from m_measuredNoiseFloorDbm (two-pass trimmed mean)
    // so the auto-adjust display feature and auto-squelch are independent.
    // Squelch threshold overlay lines. Flex and KiwiSDR keep independent
    // state because they can be controlled from different receive surfaces.
    bool  m_flexSquelchLineVisible{false};
    int   m_flexSquelchLevel{0};
    bool  m_kiwiSdrSquelchLineVisible{false};
    int   m_kiwiSdrSquelchLevel{0};
    bool  m_kiwiSdrSquelchLineFloorRelative{false};
    float m_kiwiSdrSquelchLiveFloorDbm{-999.0f};
    float m_kiwiSdrSquelchPinnedFloorDbm{-999.0f};
    bool  m_kiwiSdrSquelchPinnedFloorValid{false};
    float m_kiwiSdrSquelchPinnedThresholdDbm{-999.0f};
    bool  m_kiwiSdrSquelchPinnedThresholdValid{false};
    float m_kiwiSdrSquelchPinnedDisplayNorm{-1.0f};
    bool  m_kiwiSdrSquelchPinnedDisplayNormValid{false};
    bool  m_kiwiSdrSquelchMeterFloorValid{false};
    QVector<float> m_kiwiSdrSquelchMeterSamples;
    QTimer* m_squelchLineHideTimer{nullptr}; // auto-hides yellow line 3 s after enable/adjust (manual SQL only)
    bool  m_autoSquelchEnabled{false};
    float m_sqlNoiseFloorDbm{-999.0f};  // auto-squelch own two-pass trimmed-mean EWMA
    // dBm above noise floor for auto-squelch suggestion (5-20, default 10)
    int   m_autoSqlMarginDb{10};
    int   m_lastAutoSquelchLevel{-1};    // dedup — only emit when level changes

    // Tuning step size for click-snap and wheel scroll (Hz)
    int m_stepHz{100};
    int m_scrollAccum{0};   // trackpad pixel scroll accumulator (macOS)
    int m_angleAccum{0};    // mouse wheel angle accumulator (#390)
    qint64 m_lastWheelMs{0}; // debounce: timestamp of last accepted wheel step

    // Starstruck easter egg (Ctrl+Shift+A) — shared across all instances
    static bool s_starstruckMode;
    static QSoundEffect* s_starstruckSound;
    static void ensureStarstruckSoundLoaded();

    // Panadapter bandwidth zoom limits (MHz), set per-radio model
    double m_minBwMhz{0.010};   // 10 kHz default
    double m_maxBwMhz{5.400};   // safe default for unknown radios

    // ── FFT display controls (radio-side via "display pan set") ──────────
    int   m_panIndex{0};             // per-pan settings index (0, 1, 2, 3)
    int   m_fftAverage{0};           // 0=off, 1-10 frames
    bool  m_fftWeightedAvg{false};
    int   m_fftFps{25};
    float m_fftFillAlpha{0.70f};     // client-side fill opacity (0-1)
    QColor m_fftFillColor{0x00, 0xe5, 0xff};  // client-side fill color (default cyan)
    bool m_fftHeatMap{true};        // true = intensity heat map, false = solid color
    bool m_showGrid{true};          // false = hide grid lines
    int  m_freqGridSpacingKhz{0};   // 0=Auto, or 1/2/5/10/25/50/100 kHz (#1390)
    int  m_freqScaleFontPt{8};      // freq-scale label size, 8..14 pt (#3501)
    float m_fftLineWidth{2.0f};     // spectrum trace width in pixels

    // ── Waterfall display controls (radio-side via "display panafall set") ─
    int   m_wfColorGain{50};         // 0-100, maps intensity to color range
    int   m_wfBlackLevel{15};        // 0-125, intensity floor (below = black)
    bool  m_wfAutoBlack{true};
    // Auto-black offset (0-100). 50 → no offset (today's behaviour); <50
    // pushes the threshold above the noise floor (darker waterfall); >50
    // pulls it below (lighter).  Stored separately from m_wfBlackLevel so
    // toggling AUTO swaps between the two without losing either value.
    int   m_wfAutoBlackOffset{50};
    // Auto-black source: false = client-side noise-floor estimate (default,
    // legacy look); true = the radio's per-tile auto-black level.
    bool  m_wfAutoBlackRadioSide{false};
    WfColorScheme m_wfColorScheme{WfColorScheme::Default};
    float m_autoBlackThresh{145.0f}; // client-side auto-black: tracked noise floor
    // Radio's per-tile auto-black level (raw uint16). Preferred over the client
    // estimate when non-zero; matches FlexLib's auto-level pipeline.
    float m_radioAutoBlackRaw{0.0f};
    int   m_wfLineDuration{100};     // ms per waterfall row

    // Waterfall colour range for FFT-derived fallback (dBm).
    float m_wfMinDbm{-130.0f};
    float m_wfMaxDbm{-50.0f};

    // Scrolling waterfall image (Format_RGB32)
    QImage m_waterfall;
    int    m_wfWriteRow{0};  // ring buffer: next row to write (newest at top)
    QImage m_waterfallHistory;
    QVector<qint64> m_wfHistoryTimestamps;
    int    m_wfHistoryWriteRow{0};
    int    m_wfHistoryRowCount{0};
    int    m_wfHistoryOffsetRows{0};
    // Per-row frequency frame: each history row records the center/bandwidth it
    // was captured at (parallel to m_wfHistoryTimestamps). The full history image
    // (up to ~24k rows, ~0.5 GB at ultrawide widths) is therefore never globally
    // reprojected on a pan — instead rebuildWaterfallViewport remaps only the
    // ~700 visible rows from their own frame to the requested viewport on live
    // pan/zoom changes and time-scrollback.
    QVector<double> m_wfHistoryRowCenterMhz;
    QVector<double> m_wfHistoryRowBwMhz;
    bool   m_wfLive{true};
    bool   m_draggingTimeScale{false};
    bool   m_draggingTimeScaleRate{false};
    int    m_timeScaleDragStartY{0};
    int    m_timeScaleDragStartOffsetRows{0};
    int    m_timeScaleDragStartRatePercent{1};
    static constexpr qint64 kWaterfallHistoryMs = 20LL * 60LL * 1000LL;

    // True while native waterfall tile data (PCC 0x8004) is arriving on the
    // expected cadence.  RX uses paced FFT-derived rows only as a stale-native
    // fallback so slow requested waterfall rates do not look falsely timed out.
    bool m_hasNativeWaterfall{false};
    qint64 m_lastNativeTileMs{0};    // timestamp of last native tile (for fallback)
    bool m_waterfallFallbackActive{false};
    qint64 m_nextFallbackWaterfallRowMs{0};
    // Before the first native tile, or after a rate change, hold fallback
    // briefly so fast previews do not flash FFT rows while native data catches up.
    qint64 m_nativeWaterfallFallbackHoldUntilMs{0};
    QVector<QRgb> m_prevTileScanline;  // previous tile row for interpolation

    static constexpr float SMOOTH_ALPHA    = 0.35f;
    // Fraction of the panadapter area (above freq scale) used for spectrum
    float m_spectrumFrac{0.40f};
    // Height of the frequency scale bar at the default 8 pt label size.
    // Use freqScaleH() in layout code — it grows with the user's label
    // font so larger text never clips (#3501).
    static constexpr int   FREQ_SCALE_H    = 20;
    int freqScaleH() const;
    // Height of the draggable divider between FFT and freq scale
    static constexpr int   DIVIDER_H       = 4;
    // Divider drag state
    bool m_draggingDivider{false};
    // Bandwidth drag state (freq scale bar)
    bool m_draggingBandwidth{false};
    int  m_bwDragStartX{0};
    double m_bwDragStartBw{0.0};
    double m_bwDragAnchorMhz{0.0};
    // Waterfall pan drag state
    bool m_draggingPan{false};
    int  m_panDragStartX{0};
    double m_panDragStartCenter{0.0};
    // Filter edge drag state
    enum class FilterEdge { None, Low, High };
    FilterEdge m_draggingFilter{FilterEdge::None};
    int m_filterDragStartX{0};      // pixel X at grab time (#764)
    int m_filterDragStartHz{0};     // filter edge Hz at grab time (#764)
    // Lean render mode state (#3283).
    bool m_leanMode{false};
    QElapsedTimer m_leanRepaintClock;       // repaint cap in lean mode
    // Each panadapter present forces a full-window backing-store→GPU texture
    // re-upload (the dominant pooled cost on large/5K windows — #3283), so the
    // present rate ~= the flush rate. 33 ms (~30 Hz) roughly halves that upload
    // load vs 60 Hz while staying visually smooth for a low-overhead mode.
    static constexpr int kLeanFrameMs = 33;
    void leanCappedUpdate();                // update(), throttled when lean
    // VFO passband drag state (#404)
    bool m_draggingVfo{false};
    int  m_vfoDragOffsetHz{0};  // Hz offset from VFO at grab point (#1120)
    // Continuous edge auto-pan during VFO drag (user-reported).  The edge-follow
    // pan (revealFrequencyIfNeeded) is a *position* controller — it nudges the
    // slice a little past the trigger margin — not a *velocity* controller, so
    // holding the cursor at the border produced a tiny, self-limiting creep
    // (~0.1×span/s, the "rubber band" feel): the overshoot can't grow because
    // the cursor can't move past the physical border.  Instead, while the
    // cursor sits in the edge zone a timer drives a real pan *velocity* that
    // scales with edge depth and ramps up with hold time, panning the view and
    // keeping the slice pinned under the cursor via edgePanTuneRequested (a
    // pan-without-reveal path, so it doesn't fight the follow logic).
    QTimer* m_vfoDragEdgePanTimer{nullptr};
    int  m_vfoDragLastX{0};                 // last cursor X during VFO drag (px)
    int  m_vfoDragEdgeHoldTicks{0};         // ticks held in edge zone (ramp)
    bool m_vfoDragEdgePanDisabled{false};   // AETHER_NO_DRAG_EDGEPAN=1 escape hatch
    // Velocity knobs, env-tunable so the feel can be swept WITHOUT rebuilding:
    //   AETHER_DRAG_EDGEPAN_VMAX     — top speed, % of span width per second
    //   AETHER_DRAG_EDGEPAN_RAMP     — ms held to ramp from 0 → top speed
    //   AETHER_DRAG_EDGEPAN_INTERVAL — timer interval ms (~30 Hz default)
    int  m_edgePanVmaxPctBw{120};
    int  m_edgePanRampMs{600};
    int  m_edgePanIntervalMs{33};
    // Edge zone width as a fraction of widget width (matches the incremental
    // pan-follow trigger margin kIncrementalTriggerEdgeMarginFrac=0.05).
    static constexpr double kVfoDragEdgeZoneFrac = 0.05;
    void driveVfoDragTune(int mx, const char* phase);  // normal in-window tune
    bool updateVfoDragEdgePan(int mx);                 // → true if in edge zone
    void edgePanVelocityStep();                        // timer tick: velocity pan
    // dBm scale strip drag state
    static constexpr int DBM_STRIP_W = 36;  // width of the dBm scale strip
    static constexpr int DBM_ARROW_H = 14;  // height of each arrow button
    bool  m_draggingDbm{false};
    bool  m_draggingDbmRange{false};
    int   m_dbmDragStartY{0};
    float m_dbmDragStartRef{0.0f};
    float m_dbmDragStartRange{0.0f};
    float m_dbmDragStartBottom{0.0f};
    // Off-screen slice indicator hit rects (parallel to m_sliceOverlays)
    QVector<QRect> m_offScreenRects;
    int  m_hoveringOffScreenIdx{-1};

    // On-screen indicators (WNB, RF Gain)
    bool m_wnbActive{false};
    bool m_wnbUpdating{false};
    int  m_rfGainValue{0};
    bool m_wideActive{false};

    // HF propagation forecast overlay
    bool m_propForecastVisible{false};
    double m_propKIndex{-1.0};
    QRect  m_propClickRect;  // bounding rect of rendered prop text for click detection
    QRect  m_indicatorStripRect;  // bounding rect of full top-right indicator strip
                                  // (prop + WNB + RF Gain + WIDE) — single-click tune
                                  // is suppressed inside this rect (#1564)
    int  m_propAIndex{-1};
    int  m_propSfi{-1};

    // MQTT device status overlay
    QMap<QString, QString> m_mqttDisplayValues;

    // Background image
    QImage  m_bgImage;
    QImage  m_bgScaled;     // cached at current specRect size
    QString m_bgImagePath;
    QSize   m_bgScaledSize;
    int     m_bgOpacity{80};  // 0=full image, 100=solid dark (default 80%)
    // Solid fill colour painted BENEATH the bg image (#1741).  Default
    // matches the pre-feature compositing colour so visual is unchanged
    // until the operator picks something else via the spectrum overlay
    // menu's "Background:" colour swatch.
    QColor  m_bgFillColor{QColor(0x0a, 0x0a, 0x14)};

    // Cursor frequency label
    bool   m_showCursorFreq{false};
    QPoint m_cursorPos{-1, -1};

    // Tune guide overlay (vertical line + freq label, auto-hides after 4s)
    bool    m_showTuneGuides{false};
    bool    m_extendedFrequencyLine{false};
    bool    m_isFloating{false};
    bool    m_tuneGuideVisible{false};
    QTimer* m_tuneGuideTimer{nullptr};
    bool    m_connectionAnimationVisible{false};
    QString m_connectionAnimationLabel;
    QTimer* m_connectionAnimationTimer{nullptr};
    QElapsedTimer m_connectionAnimationClock;
    QLabel* m_interlockNotificationLabel{nullptr};
    QTimer* m_interlockNotificationTimer{nullptr};

    // State change detector cache (per-instance, NOT static — multiple
    // panadapters have different values and static vars cause an infinite
    // render loop that starves the event loop)
    double m_lastDetectCenter{0};
    double m_lastDetectBw{0};
    float  m_lastDetectRef{0};
    float  m_lastDetectDyn{0};
    float  m_lastDetectFrac{0};
    bool   m_lastDetectWnb{false};
    bool   m_lastDetectWnbUpdating{false};
    int    m_lastDetectRfGain{0};
    bool   m_lastDetectWide{false};

    // NB Waterfall Blanker (#277)
    bool  m_wfBlankerEnabled{false};
    int   m_wfBlankerMode{0};            // 0=Fill, 1=Interpolate
    float m_wfBlankerThreshold{1.15f};   // impulse multiplier vs rolling baseline
    static constexpr int WF_BLANKER_N = 32;
    float m_wfBlankerRing[WF_BLANKER_N]{};
    int   m_wfBlankerRingIdx{0};
    int   m_wfBlankerRingCount{0};
    QVector<QRgb> m_wfLastGoodRow;
    int  m_bandPlanFontSize{6};  // 0 = off
    bool m_bandPlanShowSpots{true};
    BandPlanManager* m_bandPlanMgr{nullptr};
    bool m_singleClickTune{false};
    QPoint m_clickPressPos;        // for single-click-to-tune drag threshold
    bool   m_spotClickConsumed{false}; // suppress release-to-tune after spot click (#530)
    bool m_showTxInWaterfall{false};  // default matches radio default (off)
    bool m_hasTxSlice{false};  // true if this pan contains the TX slice
    bool m_txWaterfallSliceValid{false};
    double m_txWaterfallFreqMhz{0.0};
    int m_txWaterfallFilterLowHz{0};
    int m_txWaterfallFilterHighHz{0};
    bool m_txWaterfallXitOn{false};
    int m_txWaterfallXitFreq{0};

    bool m_txDbmRangeFrozen{false};
    float m_txFrozenMinDbm{0.0f};
    float m_txFrozenMaxDbm{0.0f};
    float m_txSourceMinDbm{0.0f};
    float m_txSourceMaxDbm{0.0f};
    bool m_txDeferredDbmRangeValid{false};
    float m_txDeferredMinDbm{0.0f};
    float m_txDeferredMaxDbm{0.0f};

    bool     m_transmitting{false};
    float    m_preTxAutoBlack{145.0f}; // auto-black threshold saved before TX
    qint64   m_txEndMs{0};             // post-TX blanking: timestamp of TX→RX transition (#2117)

    // Waterfall time scale: ms-per-row is seeded from the requested rate and
    // corrected from real appended-row timestamps once the current rate has
    // enough samples.  Per-rate measurements are cached so later drags can use
    // the observed cadence immediately without re-jittering the visible scale.
    float    m_wfMsPerRow{100.0f};
    quint32  m_wfPrevTimecode{0};      // previous tile timecode (frame counter)
    qint64   m_wfPrevTimecodeMs{0};    // wall-clock time of previous timecode
    int      m_wfCalibrationCount{0};  // tiles measured so far
    bool     m_wfTimeScaleLocked{false};
    bool     m_wfHasMeasuredMsPerRow{false};
    int      m_wfLastMeasuredLineDurationMs{100};
    float    m_wfLastMeasuredMsPerRow{100.0f};
    qint64   m_wfCalibrationResumeMs{0};
    int      m_wfRowsSinceRateChange{0};
    QHash<int, float> m_wfMeasuredMsPerRowByLineDuration;
    QHash<int, int>   m_wfMeasuredSampleCountByLineDuration;


    // Lightweight diagnostics overlay toggled from View -> FPS Meters.
    bool m_showFpsMeters{false};
    QTimer* m_fpsMeterTimer{nullptr};
    QElapsedTimer m_fpsMeterWindow;
    int m_panadapterFrameCount{0};
    int m_waterfallFrameCount{0};
    int m_kiwiSdrWaterfallFrameCount{0};
    double m_panadapterFps{0.0};
    double m_waterfallFps{0.0};
    double m_kiwiSdrWaterfallFps{0.0};
    QLabel* m_panFpsMeterLabel{nullptr};
    QLabel* m_wfFpsMeterLabel{nullptr};
    qint64 m_lastMouseMoveNs{0};

    // ── TNF markers ────────────────────────────────────────────────────
    QVector<TnfMarker> m_tnfMarkers;
    bool m_tnfGlobalEnabled{true};
    QVector<SpotMarker> m_spotMarkers;
    QVector<SwrSweepPoint> m_swrSweepPoints;
    bool   m_swrSweepRunning{false};
    double m_swrSweepCurrentFreqMhz{-1.0};
    QString m_swrSweepSourceLabel;
    struct SpotHitRect {
        QRect rect;
        double freqMhz;
        int markerIndex;  // index into m_spotMarkers for tooltip data
        QString callsign; // stable hover key (index can go stale on list rebuild)
    };
    QVector<SpotHitRect> m_spotClickRects;
    QString m_hoveredSpotKey;          // callsign@freqKHz, empty when no spot hovered
    bool    m_tooltipRefreshPending{false}; // guards against duplicate queued refreshes
    QRect   m_lastTooltipRect;              // suppresses showText() when hr.rect unchanged

    QVector<SpotCluster> m_spotClusters;
    bool m_showSpots{true};
    bool m_showSHistory{false};
    bool m_showSHistoryQrm{false};
    bool m_sHistorySnapToStep{false};
    bool   m_smartSpotFilter{false};
    qint64 m_smartSpotFilterEnabledMs{0};
    int    m_smartSpotFilterOpacity{80};
    int    m_smartSpotFilterDelayS{30};
    int    m_smartSpotFilterMatchHz{1000};  // ±Hz to count as a spot↔S-History match (#2609)
    QHash<QString, qint64> m_spotConfirmedMs; // key = callsign@freqKHz → last confirmed ms
    QVector<SpotMarker> m_sHistoryMarkers;
    int  m_spotFontSize{16};
    int  m_spotMaxLevels{3};
    int  m_spotStartPct{50};      // % down from top of spectrum
    bool   m_spotOverrideColors{false};
    bool   m_spotOverrideBg{true};
    bool   m_spotShowLines{true};
    QColor m_spotColor{Qt::yellow};
    QColor m_spotBgColor{Qt::black};
    int    m_spotBgOpacity{48};
    int  m_draggingTnfId{-1};
    int  m_hoveredTnfId{-1};
    int    m_dragTnfOrigWidthHz{100};
    double m_dragTnfLastFreq{0.0};
    int    m_dragTnfLastWidthHz{100};
    QPoint m_tnfDragStartPos;
    QLabel* m_tnfHoverPopup{nullptr};

    // Floating overlay menu (child widget, anchored top-left)
    SpectrumOverlayMenu* m_overlayMenu{nullptr};
    // VFO info widgets (one per slice, attached to VFO markers)
    QMap<int, VfoWidget*> m_vfoWidgets;
#ifdef AETHER_GPU_SPECTRUM
    // #3617 GPU-flag state (only with the GPU spectrum path; the software path
    // keeps the flags as ordinary live widgets).
    bool m_gpuFlagMode{false};   // flags GPU-composited, panels hidden
    QTimer* m_flagRefreshTimer{nullptr};   // re-snapshot live flag content
    QPointer<QWidget> m_flagTimerFilteredWindow;   // #3746/#3769 — top-level watched for minimize/restore (QPointer: auto-nulls if the window is destroyed out from under us)
    // hover-swap: the flag whose panel is currently shown LIVE (interactive)
    // instead of GPU-composited, because the cursor is over it / it has focus / a
    // popup is open.  -1 = none (all flags GPU).  The panel hosts interactive
    // children (freq edit, antenna/mode/filter buttons, tabs) that only work as a
    // real widget, so we go live on interaction and snapshot back to GPU at rest.
    int m_liveFlagSliceId{-1};
    QElapsedTimer m_liveFlagIdleClock;   // debounce hiding when the cursor leaves
    static constexpr int kLiveFlagHideDelayMs = 200;
#endif
    VfoWidget* m_vfoWidget{nullptr};  // alias to active slice widget (compat)

    // Bottom-left waterfall buttons: S(egment), B(and), −/+.
    QPushButton* m_zoomSegBtn{nullptr};
    QPushButton* m_zoomBandBtn{nullptr};
    QPushButton* m_zoomOutBtn{nullptr};
    QPushButton* m_zoomInBtn{nullptr};

#ifdef AETHER_GPU_SPECTRUM
    bool m_rhiInitialized{false};

    // Waterfall GPU resources
    QRhiGraphicsPipeline* m_wfPipeline{nullptr};
    QRhiShaderResourceBindings* m_wfSrb{nullptr};
    QRhiBuffer* m_wfVbo{nullptr};
    QRhiBuffer* m_wfUbo{nullptr};
    QRhiTexture* m_wfGpuTex{nullptr};
    QRhiSampler* m_wfSampler{nullptr};
    int m_wfGpuTexW{0};
    int m_wfGpuTexH{0};
    bool m_wfTexFullUpload{true};  // full re-upload needed (resize/init)
    int m_wfLastUploadedRow{-1};   // last row uploaded to GPU (-1 = none)

    // Overlay GPU resources (QPainter → QImage → texture)
    // Static: grid, band plan, scales, slice markers, TNF, spots (repainted on state change)
    // Dynamic: FFT spectrum line (repainted every frame)
    QRhiGraphicsPipeline* m_ovPipeline{nullptr};
    QRhiShaderResourceBindings* m_ovSrb{nullptr};
    QRhiBuffer* m_ovVbo{nullptr};
    QRhiTexture* m_ovGpuTex{nullptr};
    QRhiSampler* m_ovSampler{nullptr};
    QImage m_overlayStatic;     // grid, band plan, scales, markers — drawn ABOVE FFT
    QImage m_overlayDynamic;    // FFT spectrum — repainted every frame
    bool m_overlayStaticDirty{true};
    bool m_overlayNeedsUpload{true};

    // #3617 GPU flag sprites: each slice flag is its OWN GPU texture (a "sprite"),
    // rasterized via QWidget::render into a CPU image on the refresh timer —
    // OUTSIDE the QRhi render callback (rasterizing widgets inside it re-enters
    // the renderer and crashes) and only ~12 Hz (no per-move re-raster → no audio
    // stall).  Each frame the sprite is drawn as a textured quad positioned at the
    // flag's CURRENT on-screen rect (reusing m_ovPipeline/m_ovSampler), so a
    // dragged flag follows at frame rate for free — no re-raster on move.
    struct FlagSprite {
        QRhiTexture* tex{nullptr};
        QRhiShaderResourceBindings* srb{nullptr};
        QImage img;             // CPU raster (filled by grabFlagSprites, off-frame)
        QSize texSize;          // device-px size the texture was created at
        bool imgDirty{false};   // img changed → upload to tex next frame
    };
    QHash<int, FlagSprite> m_flagSprites;       // keyed by sliceId
    QVector<int> m_flagDrawOrder;               // sliceIds to draw this frame, back→front
    QRhiBuffer* m_flagQuadVbo{nullptr};         // dynamic: 4 verts × up to kMaxFlagSprites
    QRhiSampler* m_flagSampler{nullptr};        // NEAREST — 1:1 crisp sprite text
    static constexpr int kMaxFlagSprites = 16;  // generous cap (radios have ≤8 slices)
    void grabFlagSprites();                     // QWidget::render flags → sprite imgs (off-frame)
    void releaseFlagSprite(FlagSprite& s);      // free one sprite's GPU resources

    // Background-image layer — kept separate from m_overlayStatic so it can
    // render BELOW the FFT trace (parity with the software paint path).  Same
    // pipeline + VBO + sampler as m_overlayStatic; we just rebind the SRB
    // between draws so the same overlay shader can paint a different texture.
    QRhiShaderResourceBindings* m_bgSrb{nullptr};
    QRhiTexture* m_bgGpuTex{nullptr};
    QImage m_overlayBg;
    bool m_overlayBgNeedsUpload{true};

    void initWaterfallPipeline();
    void initOverlayPipeline();
    void initSpectrumPipeline();
    void renderGpuFrame(QRhiCommandBuffer* cb);

    // FFT spectrum GPU resources — vertex color, no uniforms
    QRhiGraphicsPipeline* m_fftLinePipeline{nullptr};
    QRhiGraphicsPipeline* m_fftFillPipeline{nullptr};
    QRhiShaderResourceBindings* m_fftSrb{nullptr};
    QRhiBuffer* m_fftLineVbo{nullptr};    // dynamic, N × (vec2 pos + vec4 color)
    QRhiBuffer* m_fftFillVbo{nullptr};    // dynamic, 2N × (vec2 pos + vec4 color)
    static constexpr int kMaxFftBins = 8192;
    static constexpr int kFftVertStride = 6; // x, y, r, g, b, a
#endif

    // Mark the static overlay for repaint and schedule a frame update.
    // In non-GPU mode this is just update().
    void markOverlayDirty() {
#ifdef AETHER_GPU_SPECTRUM
        m_overlayStaticDirty = true;
#endif
        update();
    }

    void reprojectWaterfall(double oldCenterMhz, double oldBandwidthMhz,
                            double newCenterMhz, double newBandwidthMhz);
    bool reprojectSpectrum(double oldCenterMhz, double oldBandwidthMhz,
                           double newCenterMhz, double newBandwidthMhz);
};

} // namespace AetherSDR
