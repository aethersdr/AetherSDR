#pragma once

#include "RadioControlTypes.h"
#include "RadioDelta.h"
#include "SessionCommandGuard.h"
#include <QList>
#include <QSet>
#include <QStringList>
#include <utility>

namespace AetherSDR {

enum class SessionControlField {
    IdentifyProgram, LowBandwidth, RegisterGui, ReducedBandwidthDax,
    Subscribe, UdpPort, ClientIp, RadioInfo, MicrophoneInputs, SliceInventory,
    Keepalive, Ping, DisconnectClient, Count
};
enum class SessionTopic {
    Slice, Panadapter, Transmit, Atu, Amplifier, Meter, Audio, Gps, Apd,
    Client, Transverter, Tnf, Memories, Cwx, Dax, DaxIq, Radio, Codec,
    Dvk, Navtex, UsbCable, Spot, Waveform, License, Count
};
struct SessionControl {
    QSet<SessionControlField> fields;
    bool supports(SessionControlField field) const { return fields.contains(field); }
};
struct SessionControlRequest {
    using Field = SessionControlField;
    SessionControlRequest(Field field, QString clientId = {}, quint32 value = 0,
                          SessionTopic topic = SessionTopic::Count)
        : field(field), clientId(std::move(clientId)), value(value), topic(topic) {}
    Field field;
    QString clientId;
    quint32 value{0};
    SessionTopic topic{SessionTopic::Count};

    static SessionControlRequest subscription(SessionTopic topic)
    { return {Field::Subscribe, {}, 0, topic}; }
    static SessionControlRequest udpPort(quint16 port)
    { return {Field::UdpPort, {}, port}; }
    static SessionControlRequest disconnectClient(quint32 handle)
    { return {Field::DisconnectClient, {}, handle}; }
};
struct SessionRadioInfo {
    RadioDelta radio;
    std::optional<QString> chassisSerial;
    std::optional<QString> softwareVersion;
    std::optional<QString> mac;
};
struct SessionControlReply {
    SessionControlReply(RadioControlReply result) : result(std::move(result)) {}
    RadioControlReply result;
    QString clientId;
    QString clientIp;
    std::optional<SessionRadioInfo> info;
    QStringList microphones;
    QList<int> slices;
    bool accepted() const { return result.kind == RadioControlReply::Kind::Accepted; }
};
using SessionControlCompletion = std::function<void(const SessionControlReply&)>;

} // namespace AetherSDR
