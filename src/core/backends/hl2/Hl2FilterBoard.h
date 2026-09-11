#pragma once

#include "core/backends/hl2/MetisProtocol.h"

#include <QMap>
#include <QString>

#include <cstdint>

namespace AetherSDR::hl2 {

// The manual J16 filter-board override (ticket #12 / issue #9). Automatic
// band switching (Hl2Backend::applyBandFilter, ocFilterByteForHz()) stays the
// unconditional default; this table is consulted only when the operator has
// opted into manual mode.
//
// Ten bands — NOT bandKeyForHz()'s persistence-key vocabulary (Hl2Bands.h).
// That table's buckets serve a different purpose (TX drive / LNA gain memory,
// RFC #4603 PR 3) and don't line up with this board's: it splits 8m/2200m/630m
// out separately and draws its edges elsewhere. These ten are exactly what
// the filter board itself distinguishes — kOcLpf60_40 already means one relay
// answers for both 60m and 40m, kOcLpf30_20 for 30m and 20m, kOcLpf17_15 for
// 17m and 15m, kOcLpf12_10 for 12m and 10m — grouped here as separate manual
// entries anyway because manual mode is exactly the case where an operator
// might want them to differ from the stock N2ADR grouping.
inline constexpr const char* kFilterBoardBands[] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m",
};
inline constexpr int kFilterBoardBandCount = 10;

// Independent receive and transmit pin masks for one band. Each mask is the
// same 7-bit encoding ocFilterByteForHz() returns (bits [6:0], kOc* in
// MetisProtocol.h) — this table does not invent a new bit layout, it just
// makes the existing one operator-configurable per band.
struct ManualFilterBand {
    std::uint8_t rxMask = kOcNone;
    std::uint8_t txMask = kOcNone;

    bool operator==(const ManualFilterBand&) const = default;
};

using ManualFilterTable = QMap<QString, ManualFilterBand>;

enum class FilterDirection { Receive, Transmit };

// A frequency inside `band`'s bucket, for seeding from ocFilterByteForHz().
// Values are band-plan mid-points; any frequency inside the band's edges
// would encode identically, since ocFilterByteForHz()'s ranges are wider than
// any single amateur band.
inline double representativeHzForFilterBoardBand(const QString& band) noexcept
{
    static const QMap<QString, double> kMid = {
        {QStringLiteral("160m"), 1.900e6}, {QStringLiteral("80m"), 3.750e6},
        {QStringLiteral("60m"), 5.350e6},  {QStringLiteral("40m"), 7.150e6},
        {QStringLiteral("30m"), 10.120e6}, {QStringLiteral("20m"), 14.200e6},
        {QStringLiteral("17m"), 18.100e6}, {QStringLiteral("15m"), 21.250e6},
        {QStringLiteral("12m"), 24.930e6}, {QStringLiteral("10m"), 28.400e6},
    };
    return kMid.value(band, 14.2e6);   // 20m: the safest fallback for an unknown key
}

// The pin mask this manual table assigns `band` for `dir`. A band absent from
// the table encodes kOcNone (every relay released) rather than inventing a
// value — the same "undefined stays a defined bypass" rule
// ocFilterByteForHz() already follows for an out-of-range frequency.
[[nodiscard]] inline std::uint8_t manualFilterByte(const ManualFilterTable& table,
                                                   const QString& band,
                                                   FilterDirection dir) noexcept
{
    const auto it = table.constFind(band);
    if (it == table.constEnd())
        return kOcNone;
    const std::uint8_t mask = (dir == FilterDirection::Receive) ? it->rxMask : it->txMask;
    return static_cast<std::uint8_t>(mask & 0x7F);
}

// Seed a manual table from today's automatic mapping — same value for RX and
// TX, since the automatic path does not distinguish them either except while
// keyed (Hl2Backend::applyBandFilter's TX-receiver override). This is what
// "Enable manual filter control" seeds from, and what "restore N2ADR
// defaults" re-seeds to.
[[nodiscard]] inline ManualFilterTable seedManualFilterTableFromAutomatic()
{
    ManualFilterTable table;
    for (const char* band : kFilterBoardBands) {
        const QString key = QString::fromLatin1(band);
        const auto oc = ocFilterByteForHz(representativeHzForFilterBoardBand(key));
        table.insert(key, ManualFilterBand{oc, oc});
    }
    return table;
}

}  // namespace AetherSDR::hl2
