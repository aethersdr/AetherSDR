#pragma once

#include <QByteArray>
#include <QSize>
#include <QVector>

namespace AetherSDR {

// Lazily allocated ring-row backing for retained waterfall intensity. The
// logical slot count is fixed, but storage appears in small row chunks only as
// history reaches them. Each sample is a normalized palette index (0..255),
// not a four-byte color pixel; the visible viewport applies the active palette.
class WaterfallHistoryBuffer final {
public:
    static constexpr int kRowsPerChunk = 256;

    WaterfallHistoryBuffer() = default;
    WaterfallHistoryBuffer(const WaterfallHistoryBuffer&) = default;
    WaterfallHistoryBuffer& operator=(const WaterfallHistoryBuffer&) = default;
    WaterfallHistoryBuffer(WaterfallHistoryBuffer&& other) noexcept;
    WaterfallHistoryBuffer& operator=(WaterfallHistoryBuffer&& other) noexcept;

    void configure(int width, int capacityRows);
    void discardRows();
    void reset();
    bool resizeWidth(int width);

    bool isConfigured() const { return m_width > 0 && m_capacityRows > 0; }
    int width() const { return m_width; }
    int capacityRows() const { return m_capacityRows; }
    QSize size() const { return QSize(m_width, m_capacityRows); }

    quint8* writableRow(int row);
    const quint8* row(int row) const;

    qsizetype allocatedBytes() const;
    int allocatedChunkCount() const;

private:
    int rowsInChunk(int chunkIndex) const;

    int m_width{0};
    int m_capacityRows{0};
    QVector<QByteArray> m_chunks;
};

} // namespace AetherSDR
