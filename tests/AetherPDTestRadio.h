#pragma once

#include "AetherPDTestNetwork.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"

namespace AetherSDR {
class SessionControlTestAccess {
public:
    static void install(RadioModel& model, Network& network)
    {
        std::unique_ptr<FlexBackend> backend = std::make_unique<FlexBackend>();
        FlexBackend* pointer = backend.get();
        model.setBackendForTest(std::move(backend), QStringLiteral("flex"));
        model.m_connection = pointer->connection();
        model.m_panStream = pointer->panStream();
        model.m_connection->m_commandSinkForTest = [](quint32, const QString&) {};
        model.m_aetherPDModel = std::make_unique<AetherPDModel>(network);
    }
    static void begin(RadioModel& model, quint32 handle = 7)
    {
        ++model.m_sessionGeneration;
        model.m_txSessionClosing = false;
        model.m_connection->m_state.store(ConnectionState::Connected);
        model.m_connection->m_handle.store(handle);
        model.m_sessionCommands = SessionCommandGuard::create();
        model.m_sessionWire = model.m_connection;
        model.m_sessionWireGeneration = model.m_connection->commandSessionGeneration();
        model.m_guiClientRegistrationState.complete(0, {});
        model.m_clientInfoMap[handle].clientId = QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469");
        model.m_lastInfo.address = QHostAddress(QStringLiteral("192.168.50.100"));
        model.m_lastInfo.serial = QStringLiteral("PD-TEST");
    }
    static void retire(RadioModel& model) { model.resetSessionControls(); }
    static void changeGui(RadioModel& model)
    {
        model.m_clientInfoMap[7].clientId = QStringLiteral("replacement-gui");
        emit model.otherClientsChanged(0, {});
    }
    static void changeTransport(RadioModel& model) { ++model.m_connection->m_sessionGeneration; }
};
}
