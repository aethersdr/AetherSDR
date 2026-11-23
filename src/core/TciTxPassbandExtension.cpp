#include "TciTxPassbandExtension.h"

#include "core/backends/RadioCapabilities.h"
#include "models/TransmitModel.h"

#include <QtGlobal>

#include <cstdlib>
#include <utility>

namespace AetherSDR::TciTxPassbandExtension {

namespace {

std::optional<int> toInt(const QString& text)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    return ok ? std::optional<int>(value) : std::nullopt;
}

// The nearest listed edge; the lower one on a tie. An empty list is continuous.
int nearestEdge(const QList<int>& edges, int hz)
{
    if (edges.isEmpty()) {
        return hz;
    }
    int best = edges.first();
    for (const int edge : edges) {
        if (std::abs(edge - hz) < std::abs(best - hz)) {
            best = edge;
        }
    }
    return best;
}

} // namespace

bool offered(bool connected, const RadioCapabilities& caps)
{
    return connected && caps.hasTxFilterControls;
}

std::optional<Request> parse(const QStringList& args)
{
    if (args.size() != 1 && args.size() != 3) {
        return std::nullopt;
    }
    const std::optional<int> trx = toInt(args.at(0));
    if (!trx || *trx < 0) {
        return std::nullopt;
    }
    Request request;
    request.trx = *trx;
    if (args.size() == 1) {
        return request;
    }
    const std::optional<int> low = toInt(args.at(1));
    const std::optional<int> high = toInt(args.at(2));
    if (!low || !high) {
        return std::nullopt;
    }
    request.isSet = true;
    request.lowHz = *low;
    request.highHz = *high;
    return request;
}

Passband normalise(int lowHz, int highHz, const RadioCapabilities& caps)
{
    constexpr int kMax = TransmitModel::kTxFilterMaxHz;
    int low = std::abs(qBound(-kMax, lowHz, kMax));
    int high = std::abs(qBound(-kMax, highHz, kMax));
    if (low > high) {
        std::swap(low, high);
    }
    return {nearestEdge(caps.txFilterLowEdgesHz, low),
            nearestEdge(caps.txFilterHighEdgesHz, high)};
}

QString stateLine(int trx, int lowHz, int highHz)
{
    return QStringLiteral("%1:%2,%3,%4;")
        .arg(QLatin1String(kCommand)).arg(trx).arg(lowHz).arg(highHz);
}

} // namespace AetherSDR::TciTxPassbandExtension
