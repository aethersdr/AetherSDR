#pragma once

#include <QColor>
#include <QImage>
#include <QSize>
#include <QVector>
#include <QtCore/qfloat16.h>

#include <array>
#include <functional>

// ─── Stacked-trace spectrum stream surface ──────────────────────────────────
//
// Renders a perspective stacked-trace spectrum stream: a rolling history of FFT
// rows drawn back-to-front (painter's algorithm) as a receding trapezoid. The
// newest trace spans the full width across the front; older traces recede into
// a narrower, higher trapezoid. Each ridge is filled down to the plot floor so
// nearer traces occlude farther ones. Fill colour follows amplitude via an
// injected palette and dims with depth for atmospheric perspective; a bright
// per-amplitude line tops each ridge.
//
// The rendered surface is cached in a QImage and rebuilt ONLY when a new row
// arrives, the target size changes, or the amplitude mapping / palette changes.
// This keeps it cheap enough to paint on the CPU and composite through the
// existing QRhi overlay pipeline (no new shaders). The renderer is standalone
// and knows nothing about SpectrumWidget or QRhi.
class DssRenderer
{
public:
    static constexpr int kRows = 96;   // history depth (front → back)
    static constexpr int kCols = 768;  // resampled columns per row

    // Perspective geometry of the surface, shared by this CPU renderer and the
    // GPU mesh UBO (SpectrumWidget::renderGpuFrame). dss_mesh.vert applies the
    // SAME formulas with these values passed as uniforms — single source of
    // truth so the CPU fallback and the GPU mesh can't drift apart.
    static constexpr float kBackWidthFrac     = 0.60f;  // back row width / front
    static constexpr float kDepthSpanFrac     = 0.58f;  // baseline rise to the back
    static constexpr float kFrontMaxRidgeFrac = 0.46f;  // front ridge height / plot H
    static constexpr float kHaze              = 0.16f;  // fade toward bg with depth

    // Maps a dBm value to an RGB colour using the host's panadapter palette.
    using PaletteFn = std::function<QRgb(float dbm)>;

    struct RowStats {
        float minDbm{0.0f};
        float maxDbm{0.0f};
        int finiteBins{0};
        int minValueBins{0};
        int longestFlatRunBins{0};
    };

    // Push one freshly-decoded FFT row (any bin count, dBm). Peak-preserving
    // downsample to kCols and store it as the newest (front) trace.
    void pushRow(const QVector<float>& binsDbm);
    void setHistoryCapacityRows(int rows);
    int historyCapacityRows() const { return m_historyCapacityRows; }
    int historyRowCount() const { return m_historyRowCount; }
    quint64 fixedStorageBytes() const;
    quint64 historyStorageBytes() const;
    quint64 cacheStorageBytes() const;
    quint64 allocatedBytes() const;
    void appendHistoryRow(const QVector<float>& binsDbm,
                          double centerMhz, double bandwidthMhz,
                          float fallbackDbm);
    void rebuildVisibleFromHistory(int offsetRows,
                                   double centerMhz, double bandwidthMhz,
                                   float fallbackDbm);
    void reprojectFrequencyFrame(double oldCenterMhz, double oldBandwidthMhz,
                                 double newCenterMhz, double newBandwidthMhz,
                                 float fallbackDbm);

    // Return the cached surface sized to px. The plot region (everything above
    // the bottom scaleStripPx) is painted opaque over bgFill; the scale strip
    // is left transparent so the host can composite a scale on top.
    //
    // Ridge HEIGHT is anchored to the noise floor: a column maps to
    // strength = clamp((dbm - floorDbm) / rangeDb, 0, 1), so floorDbm sits at
    // the baseline (≈0 height) and floorDbm+rangeDb reaches the full ridge. The
    // host supplies floorDbm from its measured-noise-floor estimate plus the 3D
    // floor offset, and rangeDb from the current dBm display span.
    // Colour comes from palette(dbm), independent of height. paletteToken lets
    // the host signal palette changes without us inspecting them. Rebuilds only
    // when something relevant changed.
    // zCurve (<1) lifts the floor→signal band, matching dss_mesh.vert's
    // pow(s, zCurve) so the CPU fallback surfaces the noise floor like the GPU.
    const QImage& image(const QSize& px, int scaleStripPx,
                        float floorDbm, float rangeDb, float zCurve,
                        const PaletteFn& palette, quint64 paletteToken,
                        const QColor& bgFill);

    void invalidate() { m_dirty = true; }
    bool hasData() const { return m_count > 0; }
    void clear();

    // ── Data-model accessors for the GPU mesh path ──────────────────────────
    // The renderer doubles as the smoothed dBm ring store that the GPU height-map
    // mesh uploads from (one new row per frame). Indices are RING indices.
    int cols() const { return kCols; }
    int rows() const { return kRows; }
    int rowCount() const { return m_count; }     // valid rows (0..kRows)
    int headRing() const { return m_head; }       // ring index of the newest row
    const float* rowDataRing(int ringIndex) const { return m_rows[ringIndex].data(); }
    RowStats rowStats(int age, float epsilonDb = 0.01f) const;
    quint64 rowGeneration() const { return m_rowGeneration; }

    // Increments on every cache rebuild — lets the GPU path upload the texture
    // only when the surface actually changed.
    quint64 generation() const { return m_generation; }

private:
    bool historyStorageMatchesCapacity() const;
    void resetHistorySmoothing();
    void rebuild(const QSize& px, int scaleStripPx, float floorDbm,
                 float rangeDb, float zCurve, const PaletteFn& palette,
                 const QColor& bgFill);

    // Returns the dBm row at the given age (0 = newest/front).
    const std::array<float, kCols>& rowAt(int age) const;

    // Circular store: m_head indexes the newest row.
    std::array<std::array<float, kCols>, kRows> m_rows{};
    int     m_head  = 0;         // index of the newest row
    int     m_count = 0;         // number of valid rows (0..kRows)
    bool    m_dirty = true;
    quint64 m_generation = 0;    // bumped on each rebuild
    quint64 m_rowGeneration = 0; // bumped whenever stored row contents change

    // Last two RAW (pre-smoothing) resampled rows, for temporal median-of-3
    // impulse rejection of broadband interference bursts.
    std::array<float, kCols> m_rawPrev1{};
    std::array<float, kCols> m_rawPrev2{};
    int m_rawHistCount = 0;

    // Retained scrollback store. This is intentionally separate from the
    // 96-row visible surface above: waterfall history can be much deeper, and
    // the visible 3D mesh is rebuilt from this ring only while viewing history.
    QVector<qfloat16> m_historyRows;      // flat [history row][kCols]
    QVector<double> m_historyRowCenterMhz;
    QVector<double> m_historyRowBandwidthMhz;
    int m_historyCapacityRows = 0;
    int m_historyWriteRow = 0;            // ring index of newest retained row
    int m_historyRowCount = 0;
    std::array<float, kCols> m_historyRawPrev1{};
    std::array<float, kCols> m_historyRawPrev2{};
    int m_historyRawHistCount = 0;

    // Cache + the parameters it was built for (rebuild on any change).
    QImage  m_cache;
    QSize   m_cacheSize;
    int     m_cacheScaleStrip   = -1;
    float   m_cacheFloor        = 0.0f;
    float   m_cacheRange        = 0.0f;
    float   m_cacheZCurve       = 0.0f;
    quint64 m_cachePaletteToken = ~0ull;
};
