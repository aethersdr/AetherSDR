#pragma once

#include "core/CommandParser.h"

#include <QMap>
#include <QRegularExpression>
#include <QString>

namespace AetherSDR::TransmitInhibitPolicy {

struct SliceTxCommand {
    bool valid{false};
    int sliceId{-1};
    bool txEnabled{false};
};

inline SliceTxCommand parseSliceTxCommand(const QString& command)
{
    static const QRegularExpression kSliceSetRe(
        QStringLiteral(R"(^\s*slice\s+set\s+(\d+)\s+(.+?)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = kSliceSetRe.match(command);
    if (!match.hasMatch()) {
        return {};
    }

    bool ok = false;
    const int sliceId = match.captured(1).toInt(&ok);
    if (!ok) {
        return {};
    }

    const QMap<QString, QString> kvs =
        CommandParser::parseKVs(match.captured(2));
    if (!kvs.contains(QStringLiteral("tx"))) {
        return {};
    }

    const QString tx = kvs.value(QStringLiteral("tx")).trimmed();
    if (tx != QStringLiteral("0") && tx != QStringLiteral("1")) {
        return {};
    }

    return {.valid = true,
            .sliceId = sliceId,
            .txEnabled = tx == QStringLiteral("1")};
}

} // namespace AetherSDR::TransmitInhibitPolicy
