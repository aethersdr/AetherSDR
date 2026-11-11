#include "TciRxGainExtension.h"

#include "core/backends/PanDisplayRequest.h"
#include "core/backends/RadioCapabilities.h"
#include "models/PanadapterModel.h"

#include <QLatin1String>
#include <QtGlobal>

namespace AetherSDR::TciRxGainExtension {

namespace {

std::optional<int> toInt(const QString& text)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    return ok ? std::optional<int>(value) : std::nullopt;
}

} // namespace

std::optional<GainRange> panRange(const PanadapterModel& pan)
{
    if (!pan.hasRfGainRange() || pan.rfGainHigh() <= pan.rfGainLow() || pan.rfGainStep() <= 0
        || pan.rfGainUnitSuffix().trimmed().compare(QLatin1String("dB"), Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    return GainRange{pan.rfGainLow(), pan.rfGainHigh(), pan.rfGainStep()};
}

bool declaresPanRfGain(const RadioCapabilities& caps)
{
    return caps.panDisplayControl && caps.panDisplayControl->supports(PanDisplayField::RfGain);
}

bool gainOffered(bool connected, const RadioCapabilities& caps, bool anyPanRange)
{
    return connected && declaresPanRfGain(caps) && anyPanRange;
}

bool overloadOffered(bool connected, const RadioCapabilities& caps)
{
    return connected && caps.frontEndOverloadObservation.has_value();
}

std::optional<GainRequest> parseGain(const QStringList& args)
{
    if (args.size() != 1 && args.size() != 2) {
        return std::nullopt;
    }
    const std::optional<int> trx = toInt(args.at(0));
    if (!trx || *trx < 0) {
        return std::nullopt;
    }
    GainRequest request;
    request.trx = *trx;
    if (args.size() == 1) {
        return request;
    }
    const std::optional<int> db = toInt(args.at(1));
    if (!db) {
        return std::nullopt;
    }
    request.isSet = true;
    request.db = *db;
    return request;
}

std::optional<int> parseTrx(const QStringList& args)
{
    if (args.size() != 1) {
        return std::nullopt;
    }
    const std::optional<int> trx = toInt(args.at(0));
    return trx && *trx >= 0 ? trx : std::nullopt;
}

int snap(int db, const GainRange& range)
{
    const qint64 offset = static_cast<qint64>(qBound(range.lowDb, db, range.highDb)) - range.lowDb;
    const qint64 step = qMax(range.stepDb, 1);
    qint64 value = range.lowDb + (offset + step / 2) / step * step;
    if (value > range.highDb) {
        value -= step;
    }
    return static_cast<int>(value);
}

QString rangeLine(int trx, const GainRange& range)
{
    return QStringLiteral("%1:%2,%3,%4,%5;")
        .arg(QLatin1String(kRangeCommand)).arg(trx)
        .arg(range.lowDb).arg(range.highDb).arg(range.stepDb);
}

QString gainLine(int trx, int db)
{
    return QStringLiteral("%1:%2,%3;").arg(QLatin1String(kGainCommand)).arg(trx).arg(db);
}

QString autoLine(bool armed)
{
    return QStringLiteral("%1:%2;")
        .arg(QLatin1String(kAutoCommand), armed ? QStringLiteral("true") : QStringLiteral("false"));
}

QString levelName(FrontEndLevel level)
{
    switch (level) {
    case FrontEndLevel::Unobserved: return QStringLiteral("unobserved");
    case FrontEndLevel::Clean:      return QStringLiteral("clean");
    case FrontEndLevel::Marginal:   return QStringLiteral("marginal");
    case FrontEndLevel::Hot:        return QStringLiteral("hot");
    case FrontEndLevel::AtFloor:    return QStringLiteral("at_floor");
    }
    return QStringLiteral("unobserved");
}

QString overloadLine(const FrontEndOverload& state)
{
    return QStringLiteral("%1:%2,%3,%4;")
        .arg(QLatin1String(kOverloadCommand), levelName(state.level),
             state.autoArmed ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(qMax(state.autoOffsetDb, 0));
}

} // namespace AetherSDR::TciRxGainExtension
