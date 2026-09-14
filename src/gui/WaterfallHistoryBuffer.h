#pragma once

#include <QByteArray>
#include <QSize>
#include <QVector>

#include <array>

namespace AetherSDR {

// How long a panadapter retains waterfall scrollback, in minutes, and the
// lengths the UI offers. 0 is "Off": no ring at all.
//
// This lives beside the buffer rather than with the other display settings
// because it is a statement about THIS storage. Capacity is
// (minutes x 60000 / kWaterfallHistoryCapacityMsPerRow) rows, and a pan
// allocates up to two rings of (capacity x pan width) bytes, so the number is
// a memory budget as much as a time window: on a 1564 px pan, 20 minutes is
// 75.1 MB and each step down halves it.
//
// 20 is the upper bound because it is what shipped. Longer windows were
// considered and rejected: a screenful of waterfall is ~11-19 s at typical
// rates, so the far end of a 20-minute ring is already ~60 screenfuls back.
inline constexpr std::array<int, 4> kWaterfallHistoryMinutes{0, 5, 10, 20};

inline constexpr int kDefaultWaterfallHistoryMinutes = 20;

// Unknown values resolve to the DEFAULT, never to 0. A corrupt or hand-edited
// setting silently disabling scrollback would read as data loss; falling back
// to the shipped length reads as the setting being ignored, which it is.
inline int validWaterfallHistoryMinutes(int minutes)
{
    for (const int value : kWaterfallHistoryMinutes) {
        if (value == minutes) {
            return minutes;
        }
    }
    return kDefaultWaterfallHistoryMinutes;
}

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
