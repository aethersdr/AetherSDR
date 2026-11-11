#include "TciHl2Extension.h"

#include "TciExtensionSet.h"

namespace AetherSDR::TciHl2Extension {

namespace {

const char* const kBoards[] = {"none", "n2adr_rxtx", "n2adr_txonly"};

QString boolText(bool on)
{
    return on ? QStringLiteral("true") : QStringLiteral("false");
}

// An int field of a reply, or nullopt when absent or not a number.
std::optional<int> intField(const QVariantMap& map, const char* key)
{
    const QVariant v = map.value(QLatin1String(key));
    bool ok = false;
    const int value = v.isValid() ? v.toInt(&ok) : 0;
    return ok ? std::optional<int>(value) : std::nullopt;
}

} // namespace

std::optional<int> boardFromToken(const QString& token)
{
    const QString lower = token.trimmed().toLower();
    for (int i = 0; i < 3; ++i) {
        if (lower == QLatin1String(kBoards[i])) {
            return i;
        }
    }
    return std::nullopt;
}

QString boardToken(int board)
{
    return board >= 0 && board < 3 ? QString::fromLatin1(kBoards[board]) : QString();
}

std::optional<FilterBoardRequest> parseFilterBoard(const QStringList& args)
{
    if (args.isEmpty()) {
        return FilterBoardRequest{};
    }
    if (args.size() != 2) {
        return std::nullopt;
    }
    const std::optional<int> board = boardFromToken(args.at(0));
    const std::optional<bool> hpf = TciExtensionSet::parseBool(args.at(1));
    if (!board || !hpf) {
        return std::nullopt;
    }
    return FilterBoardRequest{true, *board, *hpf};
}

QString filterBoardLine(const QVariantMap& hw)
{
    const std::optional<int> board = intField(hw, "filterBoard");
    const QString token = board ? boardToken(*board) : QString();
    if (token.isEmpty() || !hw.contains(QStringLiteral("n2adrHpf"))) {
        return {};
    }
    return QStringLiteral("%1:%2,%3;")
        .arg(QLatin1String(kFilterBoardCommand), token,
             boolText(hw.value(QStringLiteral("n2adrHpf")).toBool()));
}

QString ioBoardLine(const QVariantMap& s)
{
    const std::optional<int> pins = intField(s, "inputPins");
    const std::optional<int> tuner = intField(s, "antennaTuner");
    const std::optional<int> fault = intField(s, "fault");
    const std::optional<int> firmware = intField(s, "firmwareMajor");
    if (!pins || !tuner || !fault || !firmware) {
        return ioBoardNoneLine();
    }
    QStringList fields{QString::number(*pins),
                       boolText(s.value(QStringLiteral("receiving")).toBool())};
    for (const char* key : {"in1", "in2", "in3", "in4", "in5"}) {
        fields << boolText(s.value(QLatin1String(key)).toBool());
    }
    fields << QString::number(*tuner) << QString::number(*fault) << QString::number(*firmware);
    return QStringLiteral("%1:%2;").arg(QLatin1String(kIoBoardCommand), fields.join(QLatin1Char(',')));
}

QString ioBoardNoneLine()
{
    return QStringLiteral("%1:none;").arg(QLatin1String(kIoBoardCommand));
}

QString txTimingLine(const QVariantMap& health)
{
    const std::optional<int> hang = intField(health, "pttHangTimeMs");
    const std::optional<int> latency = intField(health, "txBufferLatencyMs");
    const QString autoUnkey = hang ? boolText(*hang != kPttHangAutoUnkeyOff)
                                   : QStringLiteral("-1");
    return QStringLiteral("%1:%2,%3,%4;")
        .arg(QLatin1String(kTxTimingCommand))
        .arg(hang.value_or(-1))
        .arg(autoUnkey)
        .arg(latency.value_or(-1));
}

QString extTrLine(const QVariantMap& health)
{
    const QVariant exttr = health.value(QStringLiteral("paExtTr"));
    bool ageOk = false;
    const qlonglong age = health.value(QStringLiteral("telemetryAgeMs")).toLongLong(&ageOk);
    const bool fresh = exttr.isValid() && ageOk && age >= 0 && age <= kExtTrFreshMs;
    return QStringLiteral("%1:%2;")
        .arg(QLatin1String(kExtTrCommand),
             fresh ? boolText(exttr.toBool()) : QStringLiteral("-1"));
}

} // namespace AetherSDR::TciHl2Extension
