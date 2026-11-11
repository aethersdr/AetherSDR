#include "KenwoodFilter.h"

#include <QVector>

#include <algorithm>
#include <cmath>

namespace AetherSDR::kenwood {
namespace {
const QVector<int> kSsbLow{0, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900,
    1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000};
const QVector<int> kSsbHigh{600, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500,
    1600, 1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700,
    2800, 2900, 3000, 3400, 4000, 5000};
const QVector<int> kSsbWidth{50, 80, 100, 150, 200, 250, 300, 350, 400, 450, 500,
    600, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800,
    1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000};
const QVector<int> kCwWidth{50, 80, 100, 150, 200, 250, 300, 350, 400, 450, 500,
    600, 700, 800, 900, 1000, 1500, 2000, 2500};
const QVector<int> kAmLow{0, 100, 200, 300};
const QVector<int> kAmHigh{2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700,
    2800, 2900, 3000, 3500, 4000, 5000};
const QVector<int> kFmLow{0, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
const QVector<int> kFmHigh{1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800,
    1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000,
    3400, 4000, 5000};
const QVector<int> kFskWidth{250, 300, 350, 400, 450, 500, 1000, 1500};
const QVector<int> kPskWidth{50, 80, 100, 150, 200, 250, 300, 350, 400, 450, 500,
    600, 700, 800, 900, 1000, 1200, 1400, 1500, 1600, 1800, 2000, 2200, 2400,
    2600, 2800, 3000};
QVector<int> steps(int first, int last, int step)
{
    QVector<int> values;
    for (int hz = first; hz <= last; hz += step) { values.append(hz); }
    return values;
}
bool ssb(const QString& mode)
{
    return mode == QLatin1String("USB") || mode == QLatin1String("LSB")
        || mode == QLatin1String("DIGU") || mode == QLatin1String("DIGL");
}
bool lower(const QString& mode) { return mode == QLatin1String("LSB") || mode == QLatin1String("DIGL"); }
bool cw(const QString& mode) { return mode == QLatin1String("CW") || mode == QLatin1String("CWR"); }
int nearest(const QVector<int>& table, int hz)
{
    const auto found = std::min_element(table.cbegin(), table.cend(), [hz](int left, int right) {
        return std::abs(left - hz) < std::abs(right - hz);
    });
    return static_cast<int>(std::distance(table.cbegin(), found));
}
QList<QByteArray> commands(int low, int high)
{
    return {"SL0" + QByteArray::number(low).rightJustified(2, '0'),
            "SH0" + QByteArray::number(high).rightJustified(3, '0')};
}
}

std::optional<int> txFilterGroup(const QString& mode)
{
    if (mode == QLatin1String("USB") || mode == QLatin1String("LSB") || mode == QLatin1String("AM")) { return 0; }
    if (mode == QLatin1String("DIGU") || mode == QLatin1String("DIGL") || mode == QLatin1String("DAM")) { return 1; }
    return std::nullopt;
}

const QList<int>& txFilterLowEdges()
{
    static const QList<int> edges{10, 100, 200, 300, 400, 500};
    return edges;
}

const QList<int>& txFilterHighEdges()
{
    static const QList<int> edges{2500, 2600, 2700, 2800, 2900, 3000, 3500, 4000};
    return edges;
}

QList<QByteArray> queryTxFilter(int group)
{
    if (group < 0 || group > 1) { return {}; }
    const int item = 6 + group * 2;
    return {"EX0060" + QByteArray::number(item), "EX0060" + QByteArray::number(item + 1)};
}

std::optional<TxFilterReport> decodeTxFilter(const QByteArray& message)
{
    // PC Command Reference (2019-01-30), p.27: EX menus 6-06 through 6-09.
    if (message.size() != 11 || !message.startsWith("EX0060") || message[6] < '6' || message[6] > '9'
        || message[7] != ' ' || message[8] != '0' || message[9] != '0'
        || message[10] < '0' || message[10] > '9') { return std::nullopt; }
    const int item = message[6] - '6';
    const bool high = item % 2 != 0;
    const QList<int>& edges = high ? txFilterHighEdges() : txFilterLowEdges();
    const int index = message[10] - '0';
    if (index >= edges.size()) { return std::nullopt; }
    return TxFilterReport{item / 2, high, edges[index]};
}

std::optional<QList<QByteArray>> encodeTxFilter(int group, int lowHz, int highHz)
{
    const qsizetype low = txFilterLowEdges().indexOf(lowHz);
    const qsizetype high = txFilterHighEdges().indexOf(highHz);
    const QList<QByteArray> queries = queryTxFilter(group);
    if (queries.isEmpty() || low < 0 || high < 0) { return std::nullopt; }
    return QList<QByteArray>{queries[0] + " " + QByteArray::number(low).rightJustified(3, '0'),
        queries[1] + " " + QByteArray::number(high).rightJustified(3, '0')};
}

std::optional<NativeFilterLayout> nativeFilterLayout(const QString& mode, int style)
{
    if (cw(mode)) {
        return NativeFilterLayout{{QStringLiteral("Passband width"), kCwWidth},
            {QStringLiteral("Shift"), steps(-800, 800, 10)}};
    }
    if (ssb(mode) && style == 0) {
        return NativeFilterLayout{{QStringLiteral("Low cut"), kSsbLow}, {QStringLiteral("High cut"), kSsbHigh}};
    }
    if (ssb(mode) && style == 1) {
        return NativeFilterLayout{{QStringLiteral("Passband width"), kSsbWidth},
            {QStringLiteral("Shift"), steps(50, 2500, 50)}};
    }
    if (mode == QLatin1String("AM") || mode == QLatin1String("DAM")) {
        return NativeFilterLayout{{QStringLiteral("Low cut"), kAmLow}, {QStringLiteral("High cut"), kAmHigh}};
    }
    if (mode == QLatin1String("FM") || mode == QLatin1String("DFM")) {
        return NativeFilterLayout{{QStringLiteral("Low cut"), kFmLow}, {QStringLiteral("High cut"), kFmHigh}};
    }
    if (mode == QLatin1String("RTTY") || mode == QLatin1String("RTTYR")) {
        return NativeFilterLayout{{QStringLiteral("Passband width"), kFskWidth}, {}};
    }
    if (mode == QLatin1String("PSK") || mode == QLatin1String("PSKR")) {
        return NativeFilterLayout{{QStringLiteral("Passband width"), kPskWidth}, {}};
    }
    return std::nullopt;
}

std::optional<FilterEdges> decodeFilter(const QString& mode, int style, int low, int high)
{
    if (low < 0 || high < 0) { return std::nullopt; }
    FilterEdges edges{};
    if (cw(mode)) {
        if (low >= kCwWidth.size() || high > 160) { return std::nullopt; }
        const int shift = high * 10 - 800;
        edges = {shift - kCwWidth[low] / 2, shift + kCwWidth[low] / 2};
    } else if (ssb(mode) && style == 0) {
        if (low >= kSsbLow.size() || high >= kSsbHigh.size()) { return std::nullopt; }
        edges = {kSsbLow[low], kSsbHigh[high]};
    } else if (ssb(mode) && style == 1) {
        if (low >= kSsbWidth.size() || high > 49) { return std::nullopt; }
        const int shift = high * 50 + 50;
        edges = {shift - kSsbWidth[low] / 2, shift + kSsbWidth[low] / 2};
    } else { return std::nullopt; }
    if (edges.lowHz >= edges.highHz) { return std::nullopt; }
    if (lower(mode)) { edges = {-edges.highHz, -edges.lowHz}; }
    return edges;
}

std::optional<QList<QByteArray>> encodeFilter(const QString& mode, int style, FilterEdges edges)
{
    if (edges.lowHz < -10000 || edges.highHz > 10000 || edges.lowHz >= edges.highHz) { return std::nullopt; }
    if (lower(mode)) { edges = {-edges.highHz, -edges.lowHz}; }
    const int width = edges.highHz - edges.lowHz;
    const int center = (edges.lowHz + edges.highHz) / 2;
    if (cw(mode)) {
        if (width < 50 || width > 2500 || center < -800 || center > 800) { return std::nullopt; }
        return commands(nearest(kCwWidth, width), qRound((center + 800) / 10.0));
    }
    if (ssb(mode) && style == 0) {
        if (edges.lowHz < 0 || edges.lowHz > 2000 || edges.highHz < 600 || edges.highHz > 5000) { return std::nullopt; }
        const int low = nearest(kSsbLow, edges.lowHz);
        const int high = nearest(kSsbHigh, edges.highHz);
        if (kSsbLow[low] >= kSsbHigh[high]) { return std::nullopt; }
        return commands(low, high);
    }
    if (ssb(mode) && style == 1) {
        if (width < 50 || width > 3000 || center < 50 || center > 2500) { return std::nullopt; }
        return commands(nearest(kSsbWidth, width), qRound((center - 50) / 50.0));
    }
    return std::nullopt;
}
} // namespace AetherSDR::kenwood
