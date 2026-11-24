#include "FlexSessionControls.h"
#include "FlexBackend.h"

#include <QHostAddress>
#include <QPointer>
#include <QStringView>
#include <array>

namespace AetherSDR {
namespace {
bool clientIdentityValid(const QString& text)
{
    if (text.size() > 128) { return false; }
    for (const QChar ch : text) {
        const ushort c = ch.unicode();
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || QStringLiteral("-_.{}").contains(ch))) {
            return false;
        }
    }
    return true;
}
bool responseTextValid(const QString& text)
{
    if (text.size() > 65536) { return false; }
    for (const QChar ch : text) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) { return false; }
    }
    return true;
}
SessionControlReply malformed()
{
    return {{RadioControlReply::Kind::Failed, -1, QStringLiteral("Malformed session response")}};
}
} // namespace

namespace FlexSessionControls {
QString encode(const SessionControlRequest& request)
{
    using Field = SessionControlField;
    if (request.field != Field::RegisterGui && !request.clientId.isEmpty()) { return {}; }
    if (request.field != Field::UdpPort && request.field != Field::DisconnectClient && request.value) { return {}; }
    if (request.field != Field::Subscribe && request.topic != SessionTopic::Count) { return {}; }
    switch (request.field) {
    case Field::IdentifyProgram: return QStringLiteral("client program AetherSDR");
    case Field::LowBandwidth: return QStringLiteral("client low_bw_connect");
    case Field::RegisterGui:
        if (!clientIdentityValid(request.clientId)) { return {}; }
        return request.clientId.isEmpty() ? QStringLiteral("client gui")
            : QStringLiteral("client gui ") + request.clientId;
    case Field::ReducedBandwidthDax: return QStringLiteral("client set send_reduced_bw_dax=1");
    case Field::Subscribe: {
        static constexpr std::array topics = {"slice", "pan", "tx", "atu", "amplifier", "meter",
            "audio", "gps", "apd", "client", "xvtr", "tnf", "memories", "cwx", "dax", "daxiq",
            "radio", "codec", "dvk", "navtex", "usb_cable", "spot", "waveform", "license"};
        static_assert(topics.size() == static_cast<size_t>(SessionTopic::Count));
        const int topic = static_cast<int>(request.topic);
        if (topic < 0 || topic >= static_cast<int>(topics.size())) { return {}; }
        return QStringLiteral("sub %1 all").arg(QLatin1String(topics[topic]));
    }
    case Field::UdpPort:
        return request.value > 0 && request.value <= 65535
            ? QStringLiteral("client udpport %1").arg(request.value) : QString();
    case Field::ClientIp: return QStringLiteral("client ip");
    case Field::RadioInfo: return QStringLiteral("info");
    case Field::MicrophoneInputs: return QStringLiteral("mic list");
    case Field::SliceInventory: return QStringLiteral("slice list");
    case Field::Keepalive: return QStringLiteral("keepalive enable");
    case Field::Ping: return QStringLiteral("ping");
    case Field::DisconnectClient:
        return request.value ? QStringLiteral("client disconnect 0x%1").arg(request.value, 0, 16) : QString();
    case Field::Count: return {};
    }
    return {};
}

RadioDelta infoMetadata(const QString& body)
{
    // FlexLib 4.3.16 UpdateInfo: missing/invalid IARU region means unknown
    // for this snapshot, unlike the present-only ordinary radio status delta.
    RadioDelta delta;
    delta.iaruRegion = 0;
    for (QStringView field : QStringView(body).split(QLatin1Char(','))) {
        const qsizetype eq = field.indexOf(QLatin1Char('='));
        if (eq < 0 || field.left(eq).trimmed() != QLatin1String("iaru_region")) { continue; }
        QStringView value = field.mid(eq + 1).trimmed();
        delta.iaruRegion = 0;
        if (value.size() > 16) { continue; }
        const auto wrapper = [](QChar ch) { return ch == QLatin1Char('\\') || ch == QLatin1Char('"'); };
        while (!value.isEmpty() && wrapper(value.front())) { value = value.mid(1); }
        while (!value.isEmpty() && wrapper(value.back())) { value = value.left(value.size() - 1); }
        bool ok = false;
        const int region = value.size() <= 8 ? value.toInt(&ok) : 0;
        delta.iaruRegion = ok && region >= 1 && region <= 3 ? region : 0;
    }
    return delta;
}

SessionControlReply decode(SessionControlField field, const RadioControlReply& reply)
{
    SessionControlReply result{reply};
    if (!result.accepted()) { return result; }
    if (!responseTextValid(reply.message)) { return malformed(); }
    const QString body = reply.message.trimmed();
    switch (field) {
    case SessionControlField::RegisterGui:
        if (!clientIdentityValid(body)) { return malformed(); }
        result.clientId = body;
        break;
    case SessionControlField::ClientIp: {
        const QHostAddress address(body);
        if (address.isNull()) { return malformed(); }
        result.clientIp = address.toString();
        break;
    }
    case SessionControlField::MicrophoneInputs:
        if (body.size() > 4096) { return malformed(); }
        for (const QString& input : body.split(',', Qt::SkipEmptyParts)) {
            const QString name = input.trimmed();
            if (name.isEmpty() || name.size() > 128 || result.microphones.size() >= 128) { return malformed(); }
            result.microphones.append(name);
        }
        break;
    case SessionControlField::SliceInventory:
        for (const QString& token : body.split(' ', Qt::SkipEmptyParts)) {
            if (result.slices.size() >= 256 || token.size() > 10) { return malformed(); }
            for (const QChar ch : token) {
                if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) { return malformed(); }
            }
            bool ok = false;
            const int id = token.toInt(&ok);
            if (!ok || id < 0 || result.slices.contains(id)) { return malformed(); }
            result.slices.append(id);
        }
        break;
    case SessionControlField::RadioInfo: {
        SessionRadioInfo info;
        info.radio = infoMetadata(body);
        for (QStringView pair : QStringView(body).split(QLatin1Char(','))) {
            const qsizetype equals = pair.indexOf(QLatin1Char('='));
            if (equals < 0) { continue; }
            const QString key = pair.left(equals).trimmed().toString();
            const QString value = pair.mid(equals + 1).trimmed().toString().remove('\\').remove('"');
            if (value.size() > 4096) { continue; }
            if (key == QLatin1String("callsign")) { info.radio.callsign = value; }
            else if (key == QLatin1String("name")) { info.radio.nickname = value; }
            else if (key == QLatin1String("region")) { info.radio.region = value; }
            else if (key == QLatin1String("options")) { info.radio.radioOptions = value; }
            else if (key == QLatin1String("model") && !value.isEmpty()) { info.radio.model = value; }
            else if (key == QLatin1String("chassis_serial") && !value.isEmpty()) { info.chassisSerial = value; }
            else if (key == QLatin1String("software_ver") && !value.isEmpty()) { info.softwareVersion = value; }
            else if (key == QLatin1String("mac") && !value.isEmpty()) { info.mac = value; }
            else if (!QHostAddress(value).isNull()) {
                if (key == QLatin1String("ip")) { info.radio.ip = value; }
                else if (key == QLatin1String("netmask")) { info.radio.netmask = value; }
                else if (key == QLatin1String("gateway")) { info.radio.gateway = value; }
            }
        }
        result.info = std::move(info);
        break;
    }
    default: break;
    }
    result.result.message.clear();
    return result;
}
} // namespace FlexSessionControls

ReceiveDispatch FlexBackend::requestSessionControl(const SessionControlRequest& request,
    const SessionCommandGuard& guard, const SessionControlCompletion& completion)
{
    const QString command = FlexSessionControls::encode(request);
    if (command.isEmpty() || !guard.current() || !m_sessionSink) { return ReceiveDispatch::Unsupported; }
    const QPointer<FlexBackend> alive(this);
    const RadioControlCompletion reply = completion
        ? RadioControlCompletion([alive, guard, field = request.field, completion](const RadioControlReply& result) {
            if (!alive || !guard.current()) {
                completion({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original session ended")}});
                return;
            }
            completion(FlexSessionControls::decode(field, result));
        }) : RadioControlCompletion{};
    const SessionCommandSink sink = m_sessionSink;
    return sink(command, guard, reply) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}
} // namespace AetherSDR
