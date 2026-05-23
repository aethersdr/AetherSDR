#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QPainter;
class QPaintEvent;

namespace MasterSDR {

class WaveformWidget : public QWidget {
    Q_OBJECT

public:
    enum class ViewMode {
        Graph,
        Envelope,
        Bars,
        VerticalBars
    };

    explicit WaveformWidget(QWidget* parent = nullptr);

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

public slots:
    void appendScopeSamples(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    void setTransmitting(bool tx);
    void clear();

signals:
    void settingsDrawerToggleRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct RingBuffer {
        QVector<float> samples;
        int writeIndex{0};
        int filled{0};
        int sampleRate{24000};
        QElapsedTimer lastSamples;
    };

    struct ColumnStats {
        float min{0.0f};
        float max{0.0f};
        float peak{0.0f};
        float rms{0.0f};
        int clipped{0};
    };

    RingBuffer& activeBuffer();
    const RingBuffer& activeBuffer() const;
    void ensureCapacity(RingBuffer& buffer, int sampleRate);
    void appendToRing(RingBuffer& buffer, const float* samples, int count, int sampleRate);
    void clearRing(RingBuffer& buffer);
    void copyLatest(const RingBuffer& buffer, int count, QVector<float>& out) const;
    void setPaused(bool paused);
    void buildColumns(int columnCount);
    void drawGrid(QPainter& painter, const QRectF& plotRect, int sampleRate) const;
    void drawBarsGrid(QPainter& painter, const QRectF& plotRect) const;
    void drawGraph(QPainter& painter, const QRectF& plotRect, int clipCount);
    void drawEnvelope(QPainter& painter, const QRectF& plotRect, int clipCount);
    void drawBars(QPainter& painter, const QRectF& plotRect);
    void drawVerticalBars(QPainter& painter, const QRectF& plotRect, int sampleRate);
    void drawNoAudio(QPainter& painter, const QRectF& plotRect, const QString& source) const;
    void drawPausedBadge(QPainter& painter, const QRectF& footerRect) const;
    void scheduleRepaint();

    static int sanitizeSampleRate(int sampleRate);
    static int sanitizeWindowMs(int windowMs);
    static int sanitizeRefreshRateHz(int hz);
    static float sanitizeAmplitudeZoom(float zoom);
    static float clampSample(float sample);
    static float dbToAmplitude(float db);
    static float linearToDb(float value);

    RingBuffer m_rx;
    RingBuffer m_tx;
    QVector<float> m_displaySamples;
    QVector<float> m_pausedSamples;
    QVector<ColumnStats> m_columns;

    QTimer m_clickTimer;
    QElapsedTimer m_repaintThrottle;
    bool m_ignoreNextRelease{false};
    bool m_paused{false};
    bool m_pausedTransmitting{false};
    bool m_transmitting{false};
    int m_pausedSampleRate{24000};
    int m_windowMs{100};
    int m_refreshRateHz{24};
    float m_amplitudeZoom{1.7f};
    ViewMode m_viewMode{ViewMode::Graph};
};

} // namespace MasterSDR
