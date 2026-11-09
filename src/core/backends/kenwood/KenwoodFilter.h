#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>
#include <optional>

namespace AetherSDR::kenwood {
struct TxFilterReport { int group; bool high; int hz; };
std::optional<int> txFilterGroup(const QString& mode);
const QList<int>& txFilterLowEdges();
const QList<int>& txFilterHighEdges();
std::optional<TxFilterReport> decodeTxFilter(const QByteArray& message);
std::optional<QList<QByteArray>> encodeTxFilter(int group, int lowHz, int highHz);
QList<QByteArray> queryTxFilter(int group);
struct FilterEdges { int lowHz; int highHz; };
struct NativeFilterAxis { QString label; QVector<int> valuesHz; };
struct NativeFilterLayout { NativeFilterAxis low; NativeFilterAxis high; };
std::optional<NativeFilterLayout> nativeFilterLayout(const QString& mode, int style);
// Official TS-890S PC Command Reference (2019-01-30), pp.60–61.
// style: 0 = high/low cut, 1 = shift/width (SSB menu 6-11 / 6-12).
std::optional<FilterEdges> decodeFilter(const QString& mode, int style, int lowIndex, int highIndex);
std::optional<QList<QByteArray>> encodeFilter(const QString& mode, int style, FilterEdges edges);
}
