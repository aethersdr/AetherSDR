#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexWanSession.h"
#include "models/RadioModel.h"

namespace AetherSDR {
// A request sink, not a simulated firmware peer. All transport actions are
// inert; status fixtures inject only value events through production ingress.
class InertRemoteAccount : public RemoteRadioAccount {
public:
    quint64 beginConnection(const QString&, quint16, const QList<quint32>&, Ready, Failed) override { return 1; }
    void cancelConnection(quint64) override {}
};
class FlexWanSessionTestAccess {
public:
    static FlexWanSession& session(RadioModel& model)
    { return *static_cast<FlexBackend*>(model.backend())->m_wanSession; }
    static WanConnection& wire(RadioModel& model) { return session(model).m_connection; }
    static WanConnection& connect(RadioModel& model)
    {
        if (!model.m_flexBackend) { model.rebuildBackendForFamily(QStringLiteral("flex")); }
        FlexWanSession& wan = session(model);
        wan.m_prepareReceive = [](const WanRadioInfo&) { return ReceiveTransportResult{true, 49152}; };
        wan.m_stopReceive = [] {};
        wan.m_dial = [](const QString&, quint16, const QString&) {};
        RemoteRadioRequest request;
        request.account = std::make_shared<InertRemoteAccount>();
        request.radio.serial = QStringLiteral("wan-test-radio");
        request.radio.publicIp = QStringLiteral("192.0.2.1");
        request.radio.publicTlsPort = 4994;
        request.radio.publicUdpPort = 4993;
        RadioInfo info;
        info.family = QStringLiteral("flex");
        info.serial = request.radio.serial;
        info.address = QHostAddress(request.radio.publicIp);
        info.port = quint16(request.radio.publicTlsPort);
        model.connectToRadioRequest(info, request);
        return wan.m_connection;
    }
    static void connected(RadioModel& model, bool connected)
    {
        FlexWanSession& wan = session(model);
        wan.m_connection.m_connected = connected;
        if (connected) {
            wan.m_snapshot = ConnectionSession{wan.m_epoch, 0x42, QHostAddress(QStringLiteral("192.0.2.1")),
                {}, 49153, ConnectionSession::Transport::Wan};
        } else { wan.m_snapshot.reset(); }
    }
    static void finish(RadioModel& model) { session(model).finish(session(model).m_epoch); }
    static quint64 requestCertificate(RadioModel& model)
    {
        FlexWanSession& wan = session(model);
        wan.m_certificateAttempt = wan.m_epoch;
        emit wan.certificateRequired(wan.m_epoch, QStringLiteral("192.0.2.1"),
                                    QStringLiteral("old"), QStringLiteral("new"));
        return model.m_certificateDecision->id;
    }
    static bool certificatePending(RadioModel& model) { return session(model).m_certificateAttempt != 0; }
    static void replaceBackend(RadioModel& model)
    {
        model.rebuildBackendForFamily(QStringLiteral("sim"));
        model.rebuildBackendForFamily(QStringLiteral("flex"));
    }
};
} // namespace AetherSDR
