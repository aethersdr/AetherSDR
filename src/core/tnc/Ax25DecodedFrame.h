#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace AetherSDR {

enum class Ax25ModemProfile {
    Hf300,
    Vhf1200,
};

struct Ax25DecodedFrame {
    QDateTime timestampUtc;
    QString source;
    QString destination;
    QStringList path;
    quint8 control{0};
    quint8 pid{0};
    QByteArray payload;
    QString payloadText;
    QString payloadHex;
    bool isUiFrame{false};
    bool fcsOk{false};
    double confidenceOrQuality{0.0};
    int decodePhaseOffsetSamples{-1};
    // Raw on-air AX.25 frame bytes (address through info), WITHOUT HDLC flags or
    // the 2-byte FCS — i.e. exactly the payload of a KISS data frame, so the TNC
    // can forward a decode to host applications byte-for-byte.
    QByteArray ax25FrameNoFcs;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Ax25DecodedFrame)
