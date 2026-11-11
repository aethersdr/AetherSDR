#pragma once

#include "core/backends/flex/FlexStatusIngress.h"

#include <QMap>
#include <QObject>
#include <QString>

// The IQ stream delta the Flex status decoder produces for a dax_iq stream
// status with these fields (daxiq_channel, daxiq_rate, pan, active,
// client_handle, ip, …), through the production decoder.
inline AetherSDR::AudioStreamDelta iqStatus(quint32 streamId, const QMap<QString, QString>& fields)
{
    QMap<QString, QString> kvs = fields;
    kvs.insert(QStringLiteral("type"), QStringLiteral("dax_iq"));
    AetherSDR::FlexStatusIngress ingress;
    AetherSDR::AudioStreamDelta delta;
    QObject::connect(&ingress, &AetherSDR::FlexStatusIngress::audioStreamChanged,
                     [&delta](const AetherSDR::AudioStreamDelta& decoded) { delta = decoded; });
    ingress.decodeAudioStreamStatus(QStringLiteral("stream 0x%1").arg(streamId, 0, 16), kvs);
    return delta;
}
