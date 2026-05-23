#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace MasterSDR {

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
};

} // namespace MasterSDR

Q_DECLARE_METATYPE(MasterSDR::Ax25DecodedFrame)
