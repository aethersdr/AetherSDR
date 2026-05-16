#pragma once

#include "Ax25DecodedFrame.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>

namespace AetherSDR {

class Ax25FrameFormatter {
public:
    static std::optional<Ax25DecodedFrame> decodeFrameBytes(
        const QByteArray& frameWithFcs,
        const QDateTime& timestampUtc = QDateTime::currentDateTimeUtc(),
        double quality = 1.0);

    static QString formatLogLine(const Ax25DecodedFrame& frame);
    static QString payloadText(const QByteArray& payload);
    static QString payloadHex(const QByteArray& payload);
    static bool hasValidFcs(const QByteArray& frameWithFcs);

private:
    static quint16 computeFcs(const QByteArray& bytes);
};

} // namespace AetherSDR
