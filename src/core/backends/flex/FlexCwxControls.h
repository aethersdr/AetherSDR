#pragma once

#include "core/backends/CwTextRequest.h"
#include <QStringView>

namespace AetherSDR::FlexCwxControls {

inline QString encode(const CwTextRequest& request)
{
    if (!request.valid()) { return {}; }
    using Field = CwTextRequest::Field;
    QString text = request.text;
    // FlexLib 4.3.16 CWX.cs: quoted text uses DEL for spaces and RS for quotes.
    text.replace(QLatin1Char(' '), QChar(0x7f));
    text.replace(QLatin1Char('"'), QChar(0x1e));
    switch (request.field) {
    case Field::Send: return QStringLiteral("cwx send \"%1\" %2").arg(text).arg(request.block);
    case Field::SendMacro: return QStringLiteral("cwx macro send %1").arg(request.value);
    case Field::SaveMacro: return QStringLiteral("cwx macro save %1 \"%2\"").arg(request.value + 1).arg(text);
    case Field::Speed: return QStringLiteral("cwx wpm %1").arg(request.value);
    case Field::Delay: return QStringLiteral("cwx delay %1").arg(request.value);
    case Field::Qsk: return QStringLiteral("cwx qsk_enabled %1").arg(request.value);
    case Field::Erase: return QStringLiteral("cwx erase %1").arg(request.value);
    case Field::Clear: return QStringLiteral("cwx clear");
    }
    return {};
}

inline CwTextReply decodeReply(int code, const QString& body, int expectedBlock)
{
    if (code != 0) { return {CwTextReply::Kind::Failed, -1, expectedBlock, code}; }
    // FlexLib QueueMessageReply reports exactly <first character index>,<block>.
    // Bound the numeric reply independently of the size of the submitted text.
    if (expectedBlock <= 0 || body.size() > 64) { return {}; }
    const QStringView input(body);
    const qsizetype comma = input.indexOf(QLatin1Char(','));
    if (comma < 1 || input.indexOf(QLatin1Char(','), comma + 1) >= 0) { return {}; }
    bool indexOk = false;
    bool blockOk = false;
    const int index = input.first(comma).trimmed().toInt(&indexOk);
    const int block = input.sliced(comma + 1).trimmed().toInt(&blockOk);
    if (!indexOk || !blockOk || index < 0 || block != expectedBlock) { return {}; }
    return {CwTextReply::Kind::Queued, index, block, 0};
}

} // namespace AetherSDR::FlexCwxControls
