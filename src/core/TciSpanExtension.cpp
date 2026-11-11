#include "TciSpanExtension.h"

#include "core/backends/PanDisplayRequest.h"
#include "core/backends/RadioCapabilities.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::TciSpanExtension {

namespace {

bool inLimits(qint64 hz, qint64 minHz, qint64 maxHz)
{
    return (minHz <= 0 || hz >= minHz) && (maxHz <= 0 || hz <= maxHz);
}

} // namespace

Model model(const RadioCapabilities& caps)
{
    Model m;
    if (caps.panSpanModel) {
        m.followsRate = caps.panSpanModel->followsSampleRate;
        m.radioWide = caps.panSpanModel->radioWide;
    }
    if (m.followsRate) {
        for (const int rate : caps.sampleRatesHz) {
            if (rate > 0 && !m.rates.contains(rate)) {
                m.rates.append(rate);
            }
        }
        std::sort(m.rates.begin(), m.rates.end());
    }
    return m;
}

bool offered(bool connected, const RadioCapabilities& caps)
{
    if (!connected || !caps.panDisplayControl || !caps.panDisplayControl->declaresSpan()) {
        return false;
    }
    const Model m = model(caps);
    return !m.followsRate || !m.rates.isEmpty();
}

std::optional<Request> parseSpan(const QStringList& args)
{
    if (args.size() != 1 && args.size() != 2) {
        return std::nullopt;
    }
    bool ok = false;
    const int trx = args.at(0).toInt(&ok);
    if (!ok || trx < 0) {
        return std::nullopt;
    }
    Request request;
    request.trx = trx;
    if (args.size() == 1) {
        return request;
    }
    const qint64 hz = args.at(1).toLongLong(&ok);
    if (!ok || hz <= 0) {
        return std::nullopt;
    }
    request.isSet = true;
    request.hz = hz;
    return request;
}

QVector<int> reachableRates(const Model& model, qint64 minHz, qint64 maxHz)
{
    QVector<int> out;
    for (const int rate : model.rates) {
        if (inLimits(rate, minHz, maxHz)) {
            out.append(rate);
        }
    }
    return out;
}

qint64 snap(qint64 hz, const Model& model, qint64 minHz, qint64 maxHz)
{
    if (!model.followsRate) {
        qint64 out = hz;
        if (maxHz > 0) {
            out = std::min(out, maxHz);
        }
        if (minHz > 0) {
            out = std::max(out, minHz);
        }
        return out;
    }
    QVector<int> candidates = reachableRates(model, minHz, maxHz);
    if (candidates.isEmpty()) {
        candidates = model.rates;  // limits exclude every rate: the radio decides
    }
    if (candidates.isEmpty()) {
        return hz;
    }
    int best = candidates.first();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int rate : candidates) {
        const double distance = std::abs(std::log(static_cast<double>(hz) / rate));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = rate;
        }
    }
    return best;
}

QString ratesLine(const Model& model, const QVector<int>& reachable)
{
    QString line = QStringLiteral("%1:%2,%3")
        .arg(QLatin1String(kRatesCommand),
             model.followsRate ? QStringLiteral("true") : QStringLiteral("false"),
             model.radioWide ? QStringLiteral("true") : QStringLiteral("false"));
    for (const int rate : reachable) {
        line += QLatin1Char(',') + QString::number(rate);
    }
    return line + QLatin1Char(';');
}

QString spanLine(int trx, qint64 hz)
{
    return QStringLiteral("%1:%2,%3;").arg(QLatin1String(kSpanCommand)).arg(trx).arg(hz);
}

} // namespace AetherSDR::TciSpanExtension
