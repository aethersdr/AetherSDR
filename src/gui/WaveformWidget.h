#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#include <rhi/qrhi.h>
#define WAVEFORM_BASE_CLASS QRhiWidget
#else
#define WAVEFORM_BASE_CLASS QWidget
#endif

#include "WaveformScopeModel.h"

class QMouseEvent;
class QPainter;
class QPaintEvent;

namespace AetherSDR {

// Audio scope shared by the sidebar WAVE applet and the Aetherial strip's
// waveform panels. The strip used to be a near-identical fork
// (StripWaveform); Profile carries the few real differences so both
// surfaces share one implementation and one perf story.
//
// Reduction is incremental (WaveformScopeModel): samples fold into bins as
// they arrive, and a repaint only merges bins into pixel columns — the
// per-frame cost no longer scales with the time window (#3283).
//
// Rendering follows the SpectrumWidget dual-path pattern: with
// AETHER_GPU_SPECTRUM the widget is a QRhiWidget and the waveform (all four
// view modes, grid lines, clip ticks) is evaluated per-pixel in a fragment
// shader from a 1-D column texture; text (dB labels, readout, footer,
// badges) rides a QPainter-drawn overlay texture refreshed only when it
// changes. Without the flag it stays the QPainter paintEvent path.
class WaveformWidget : public WAVEFORM_BASE_CLASS {
    Q_OBJECT

public:
    enum class ViewMode {
        Graph,
        Envelope,
        Bars,
        VerticalBars
    };

    enum class Profile {
        Applet,  // sidebar WAVE applet — 20 s window cap, crisp stroke
        Strip,   // Aetherial strip panel — 30 s window cap, AA stroke
    };

    explicit WaveformWidget(QWidget* parent = nullptr);
    explicit WaveformWidget(Profile profile, QWidget* parent = nullptr);

    QSize sizeHint() const override { return {240, 160}; }
    QSize minimumSizeHint() const override { return {220, 110}; }

    ViewMode viewMode() const { return m_viewMode; }
    int zoomWindowMs() const { return m_windowMs; }
    int refreshRateHz() const { return m_refreshRateHz; }
    float amplitudeZoom() const { return m_amplitudeZoom; }

    void setViewMode(ViewMode mode);
    void setZoomWindowMs(int windowMs);
    void setRefreshRateHz(int hz);
    void setAmplitudeZoom(float zoom);

    // Live counters for the automation bridge's `get wavestats` verb —
    // per-widget paint cost is measurable without a profiler attach.
    struct PerfStats {
        quint64 paintCount{0};
        quint64 paintUsTotal{0};
        quint64 paintUsMax{0};
        quint64 appendCount{0};
        quint64 appendedSamples{0};
        qint64 sinceMs{0};   // ms the counters have been accumulating
    };
    PerfStats perfStats() const;
    void resetPerfStats();
    // Bridge-facing snapshot (invoked by name from AutomationServer, which
    // deliberately includes no GUI headers). Optionally resets the
    // counters so successive reads measure disjoint intervals.
    Q_INVOKABLE QVariantMap wavestatsSnapshot(bool reset);

    // Deregister from the current top-level backing-store QRhi BEFORE a
    // float/dock reparent — the #2495 QRhiWidget stale-cleanup-callback
    // crash class. No-op in CPU builds. Called by ContainerManager.
    void prepareForTopLevelChange();
    bool isPausedByUser() const { return m_paused; }
    bool isTransmitSource() const { return m_transmitting; }
    int activeSampleRate() const;

public slots:
    void appendScopeSamples(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    void setTransmitting(bool tx);
    void clear();

signals:
    void settingsDrawerToggleRequested();

protected:
#ifdef AETHER_GPU_SPECTRUM
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
#else
    void paintEvent(QPaintEvent* event) override;
#endif
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    WaveformScopeModel& activeModel();
    const WaveformScopeModel& activeModel() const;
    const WaveformScopeModel& displayModel() const;
    void setPaused(bool paused);
    QRectF plotArea() const;
    void ensureGridCache(int kind, const QRectF& plotRect);
    void drawGrid(QPainter& painter, const QRectF& plotRect, int sampleRate) const;
    void drawBarsGrid(QPainter& painter, const QRectF& plotRect) const;
    void drawGraph(QPainter& painter, const QRectF& plotRect, int clipCount);
    void drawEnvelope(QPainter& painter, const QRectF& plotRect, int clipCount);
    void drawBars(QPainter& painter, const QRectF& plotRect);
    void drawVerticalBars(QPainter& painter, const QRectF& plotRect, int sampleRate);
    void drawNoAudio(QPainter& painter, const QRectF& plotRect, const QString& source) const;
    void drawPausedBadge(QPainter& painter, const QRectF& footerRect) const;
    void scheduleRepaint();

    int sanitizeWindowMs(int windowMs) const;
    int sanitizeRefreshRateHz(int hz) const;
    static int sanitizeSampleRate(int sampleRate);
    static float sanitizeAmplitudeZoom(float zoom);
    static float dbToAmplitude(float db);
    static float linearToDb(float value);

    // Bands-mode Goertzel band levels [0..1] from the model's raw tail —
    // shared by the QPainter draw path and the GPU column-texture upload.
    // Cached on the model generation with a floor interval so a 60 fps
    // render loop doesn't re-run the filter bank per frame.
    void computeBandLevels(const WaveformScopeModel& model,
                           int bandCount, QVector<float>& levels);

    // Force the next render to recompute Bands levels and re-upload the
    // column/clip textures. Called when the DISPLAYED model or view mode
    // changes: the generation() dirty counters are per-model and per-frame,
    // not namespaced to which model/mode produced the cached data, so a
    // model/view swap that leaves columnCount unchanged could otherwise skip
    // the refresh and render the previous source's data (#3955).
    void invalidateRenderCaches();

#ifdef AETHER_GPU_SPECTRUM
    struct WaveUniforms;    // std140 block mirrored in wavescope.frag
    void initWavePipeline();
    void initOverlayPipeline();
    void renderGpuFrame(QRhiCommandBuffer* cb);
    // Rebuild the text overlay image (dB labels, RMS/PK readout, footer,
    // CLIP / PAUSED / no-audio badges) — only when its content key changes.
    void updateOverlayImage(const WaveformScopeModel::WindowStats& stats,
                            const QString& source, int sampleRate, bool stale);
#endif

    WaveformScopeModel m_rx;
    WaveformScopeModel m_tx;
    // Frozen copy of the active model while paused; cleared on resume so
    // the (potentially multi-MB) raw ring copy isn't kept around.
    WaveformScopeModel m_pausedModel;
    QVector<WaveformScopeModel::ColumnStats> m_columns;
    QVector<float> m_tailScratch;   // Bands-mode analysis tail
    // Per-frame geometry scratch, reused to avoid realloc churn at 60 fps.
    QVector<QLineF> m_lineScratch;
    QVector<QPointF> m_peakTopPts;
    QVector<QPointF> m_peakBottomPts;
    QVector<QPointF> m_rmsTopPts;
    QVector<QPointF> m_rmsBottomPts;

    // The background + grid + dB labels are static per (size, zoom, theme,
    // mode family); rendering them once removes per-frame line drawing and
    // text shaping — HarfBuzz re-shaping every frame was a top cost in the
    // #3283 profile. A QImage so the GPU build can upload the same layer as
    // its base texture.
    QImage m_gridCache;
    bool m_gridCacheDirty{true};          // GPU: re-upload after rebuild
    QSize m_gridCacheSize;
    qreal m_gridCacheDpr{0.0};
    float m_gridCacheZoom{-1.0f};
    bool m_gridCacheShowGrid{false};
    int m_gridCacheKind{-1};        // 0 = centered scope grid, 1 = bars grid
    quint64 m_gridCacheThemeKey{0};
    QString m_gridCacheFontKey;
    int m_headerTick{0};            // full-widget repaint divider (~5 Hz text)

    QTimer m_clickTimer;
    QElapsedTimer m_repaintThrottle;
    bool m_ignoreNextRelease{false};
    bool m_paused{false};
    bool m_pausedTransmitting{false};
    bool m_transmitting{false};
    int m_windowMs{100};
    int m_refreshRateHz{24};
    float m_amplitudeZoom{1.7f};
    ViewMode m_viewMode{ViewMode::Graph};

    // Profile-dependent limits (the old WaveformWidget/StripWaveform fork).
    int m_maxWindowMs;
    bool m_antialiasedStroke;

    // Perf counters (reset via resetPerfStats()). In GPU builds the "paint"
    // numbers time render() — the main-thread cost of a frame either way.
    quint64 m_perfPaintCount{0};
    quint64 m_perfPaintUsTotal{0};
    quint64 m_perfPaintUsMax{0};
    quint64 m_perfAppendCount{0};
    quint64 m_perfAppendedSamples{0};
    QElapsedTimer m_perfSince;

    // Bands-mode level cache (see computeBandLevels()).
    QVector<float> m_bandLevels;
    quint64 m_bandCacheGen{~0ull};
    int m_bandCacheCount{0};
    QElapsedTimer m_bandCacheAge;

#ifdef AETHER_GPU_SPECTRUM
    QRhiGraphicsPipeline* m_wavePipeline{nullptr};
    QRhiShaderResourceBindings* m_waveSrb{nullptr};
    QRhiBuffer* m_waveVbo{nullptr};
    QRhiBuffer* m_waveUbo{nullptr};
    QRhiTexture* m_colTex{nullptr};       // columnCount×1 RGBA32F min/max/rms/peak
    QRhiTexture* m_clipTex{nullptr};      // columnCount×1 R8 clip flags
    quint64 m_lastColUploadGen{~0ull};    // model.generation() of the last col/clip upload
    QRhiSampler* m_colSampler{nullptr};   // linear — curves interpolate between columns
    QRhiSampler* m_clipSampler{nullptr};  // nearest
    int m_colTexW{0};

    // Textured-quad pipeline (overlay.vert/.frag, premultiplied blend),
    // drawn twice per frame with different SRBs: the grid/background image
    // UNDER the waveform, and the text overlay OVER it.
    QRhiGraphicsPipeline* m_ovPipeline{nullptr};
    QRhiShaderResourceBindings* m_gridSrb{nullptr};
    QRhiTexture* m_gridTex{nullptr};
    QRhiShaderResourceBindings* m_ovSrb{nullptr};
    QRhiTexture* m_ovTex{nullptr};
    QRhiSampler* m_ovSampler{nullptr};

    QImage m_overlayImage;
    QString m_overlayKey;                 // content key — skip repaint+upload when unchanged
    QElapsedTimer m_overlayTextAge;       // floor interval for readout refresh (~5 Hz)
    bool m_overlayNeedsUpload{false};
    bool m_rhiInitialized{false};
    bool m_shutdownPrepared{false};
    QVector<float> m_colUpload;           // columnCount × RGBA32F staging
    QVector<quint8> m_clipUpload;         // columnCount × R8 staging
#endif
};

} // namespace AetherSDR
