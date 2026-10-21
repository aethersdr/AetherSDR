#include "core/Kpa500Connection.h"

#include <QCoreApplication>
#include <QList>
#include <QIODevice>
#include <algorithm>

#include <cstdio>

namespace AetherSDR {

// In-memory byte transport only: no listener, peer, serial port or firmware model.
class Kpa500TestStream : public QIODevice {
public:
    Kpa500TestStream() { open(QIODevice::ReadWrite | QIODevice::Unbuffered); }
    QByteArray incoming;
    QByteArray written;
    qint64 pendingBytes{0};
    bool failWrite{false};
    bool shortWrite{false};
    qint64 bytesToWrite() const override { return pendingBytes; }
    qint64 bytesAvailable() const override { return incoming.size() + QIODevice::bytesAvailable(); }
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 count = std::min(maxSize, qint64(incoming.size()));
        std::copy_n(incoming.constData(), count, data);
        incoming.remove(0, count);
        return count;
    }
    qint64 writeData(const char* data, qint64 size) override
    {
        if (failWrite) { return -1; }
        const qint64 count = shortWrite ? size - 1 : size;
        written.append(data, count);
        return count;
    }
};

class Kpa500ConnectionTest {
public:
    static int run()
    {
        int failed = 0;
        const auto check = [&failed](const char* description, bool ok) {
            std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", description);
            if (!ok) {
                ++failed;
            }
        };

        check("Band and speaker command bounds follow public reference",
              Kpa500::buildSetBand(-1).isEmpty() && Kpa500::buildSetBand(11).isEmpty()
              && Kpa500::buildSetBand(0) == "^BN00;" && Kpa500::buildSetBand(10) == "^BN10;"
              && Kpa500::buildSetSpeaker(true) == "^SP1;" && Kpa500::buildSetSpeaker(false) == "^SP0;");
        Kpa500::Status speaker;
        Kpa500::applyMessage("SP", "1", speaker);
        Kpa500::applyMessage("SP", "2", speaker);
        check("Malformed speaker response cannot overwrite confirmed state", speaker.speakerOn == true);
        Kpa500TestStream stream;
        QStringList diagnostics;
        Kpa500Connection connection;
        connection.m_device = &stream;
        Kpa500::Status latest;
        int updates = 0;
        QObject::connect(&connection, &Kpa500Connection::statusUpdated,
                         [&latest, &updates](const Kpa500::Status& status) {
            latest = status;
            ++updates;
        });
        QList<bool> detectionChanges;
        QObject::connect(&connection, &Kpa500Connection::detectedChanged,
                         [&detectionChanges](bool detected) { detectionChanges.append(detected); });
        connection.onTransportUp();
        check("Opening the port alone does not detect an amplifier",
              !connection.isDetected() && detectionChanges.isEmpty());
        connection.m_parser.feed(";^BN05;^OS0;");
        check("Echo and replies ignored during the initial power probe do not detect an amp",
              !connection.isDetected() && detectionChanges.isEmpty());
        for (int i = 0; i < 3; ++i) {
            connection.fastPollTick();
        }
        connection.m_parser.feed("^XX1;^OS2;^FC07;^RVMgarbage;");
        check("Malformed and unknown replies cannot establish liveness",
              !connection.isResponding() && !connection.isDetected()
              && detectionChanges.isEmpty() && updates == 0);

        QObject::connect(&connection, &Kpa500Connection::diagnostic,
                         [&diagnostics](const QString& line) { diagnostics.append(line); });
        bool rejectedUnsafeBytes = true;
        for (int value = 0; value <= 255; ++value) {
            if (value >= 0x20 && value <= 0x7e) { continue; }
            diagnostics.clear();
            stream.incoming = QByteArray("^OS1") + char(value) + ";";
            connection.onReadyRead();
            rejectedUnsafeBytes &= !diagnostics.isEmpty() && !connection.isResponding();
            for (const QString& line : diagnostics) {
                for (const QChar c : line) {
                    rejectedUnsafeBytes &= c.unicode() >= 0x20 && c.unicode() <= 0x7e;
                }
            }
        }
        check("Malformed serial bytes cannot inject control characters into diagnostics",
              rejectedUnsafeBytes);

        connection.m_parser.feed("^ON1;^OS1;^WS500 015;^SN12345;^FC00;");
        check("Accepted replies establish liveness and populate the snapshot",
              connection.isResponding() && latest.operate == true
              && latest.forwardPowerW == 500.0f && latest.swr == 1.5f
              && latest.fanMinSpeed == 0);
        check("Accepted replies detect the amplifier exactly once",
              connection.isDetected() && detectionChanges == QList<bool>{true});
        const int populatedUpdates = updates;
        for (int i = 0; i < 40; ++i) {
            connection.m_parser.feed("^OS1;");
            connection.fastPollTick();
        }
        check("Unchanged accepted replies keep the amplifier live without redundant updates",
              connection.isResponding() && updates == populatedUpdates
              && detectionChanges == QList<bool>{true});

        connection.m_parser.feed("^WS9");
        for (int i = 0; i <= Kpa500Connection::kSilentTickLimit; ++i) {
            connection.m_parser.feed("^OS2;");
            connection.fastPollTick();
        }
        check("Malformed replies do not prevent the silence deadline",
              !connection.isResponding());
        check("A detected amplifier remains available during silence on an open port",
              connection.isDetected() && detectionChanges == QList<bool>{true});
        check("Silence clears cached telemetry and publishes an empty snapshot",
              !connection.lastStatus().hasAnyField() && !latest.hasAnyField()
              && updates == populatedUpdates + 1);

        // A suffix from before the silence boundary cannot resurrect telemetry.
        connection.m_parser.feed("99 990;^BN05;");
        check("Non-power replies cannot resume normal polling after silence", !connection.isResponding());
        connection.m_lastPowerCheckAt = connection.m_clock.elapsed() - Kpa500Connection::kPowerCheckMs;
        connection.fastPollTick();
        connection.m_parser.feed("^ON1;^BN05;");
        check("Recovery contains only newly confirmed fields",
              connection.isResponding() && latest.band == 5
              && !latest.operate && !latest.forwardPowerW && !latest.swr
              && !latest.serialNumber && !latest.fanMinSpeed);
        check("Recovery schedules a fresh identity query",
              !connection.m_connectCmdsDone && connection.m_connectCmdIndex == 0);
        connection.fastPollTick();
        check("Identity refresh advances on the next fast tick",
              connection.m_connectCmdIndex == 1);

        // Exercise a partial valid frame immediately before the loss boundary.
        for (int i = 0; i < 3; ++i) {
            connection.fastPollTick();
        }
        connection.m_parser.feed("^WS9");
        for (int i = 0; i <= Kpa500Connection::kSilentTickLimit; ++i) {
            connection.fastPollTick();
        }
        connection.m_parser.feed("99 990;");
        check("Silence discards a pending partial frame",
              !connection.isResponding() && !connection.lastStatus().hasAnyField());

        connection.m_lastPowerCheckAt = connection.m_clock.elapsed() - Kpa500Connection::kPowerCheckMs;
        connection.fastPollTick();
        connection.m_parser.feed("^ON1;^OS0;^WS000 000;");
        check("Standby and zero watts recover without a fabricated SWR",
              connection.isResponding() && latest.operate == false
              && latest.forwardPowerW == 0.0f && !latest.swr);
        connection.disconnect();
        check("Transport loss clears cached telemetry",
              !connection.isConnected() && !connection.isResponding()
              && !connection.lastStatus().hasAnyField() && !latest.hasAnyField());
        check("Closing the transport clears detection",
              !connection.isDetected() && detectionChanges == QList<bool>{true, false});
        connection.m_parser.feed("^ON1;^OS1;");
        check("Replies injected after disconnect cannot revive liveness",
              !connection.isResponding() && !connection.isDetected()
              && !connection.lastStatus().hasAnyField());
        connection.m_device = &stream;
        connection.onTransportUp();
        check("A reopened port requires fresh detection",
              !connection.isDetected() && detectionChanges == QList<bool>{true, false});
        connection.m_parser.feed("^ON0;");
        check("A confirmed off reply also detects the amplifier",
              connection.isDetected() && connection.lastStatus().powerOn == false
              && detectionChanges == QList<bool>{true, false, true});
        for (int i = 0; i <= Kpa500Connection::kSilentTickLimit; ++i) {
            connection.fastPollTick();
        }
        check("An off amplifier stays available for waking on its open port",
              connection.isDetected() && detectionChanges == QList<bool>{true, false, true});
        connection.disconnect();

        Kpa500TestStream followStream;
        Kpa500Connection follow;
        follow.m_device = &followStream;
        QStringList writes;
        QObject::connect(&follow, &Kpa500Connection::diagnostic, [&writes](const QString& line) {
            if (line.startsWith("TX ")) { writes.append(line.mid(3)); }
        });
        follow.onTransportUp();
        check("Follow TX is enabled by default", follow.m_followTx);
        follow.setTxContext(5, "A", false);
        check("No band change with unknown amplifier state", !writes.contains("^BN05;"));
        follow.m_parser.feed("^ON1;^BN03;^WS000 000;^OS0;");
        check("Confirmed idle amp follows TX source", writes.count("^BN05;") == 1);
        check("Requested band does not replace confirmed readback", follow.lastStatus().band == 3);
        follow.m_parser.feed("^BN05;");
        check("Band reply acknowledges command", follow.m_sentBand == -1 && follow.lastStatus().band == 5);
        writes.clear();
        follow.setTransmitting(true);
        follow.setTxContext(1, "B", true);
        follow.setTxContext(3, "B", true);
        check("Retunes during TX are deferred", writes.isEmpty() && follow.bandTrackingText().contains("queued"));
        follow.setTransmitting(false);
        check("RX sends only latest queued band", writes.count("^BN03;") == 1 && !writes.contains("^BN01;"));
        follow.m_parser.feed("^BN03;");
        writes.clear();
        follow.setTxContext(-1, "No source", false);
        check("Missing TX source leaves amplifier unchanged", writes.isEmpty());
        follow.setFollowTx(false);
        follow.setTxContext(9, "A", false);
        check("Manual mode ignores TX source changes", writes.isEmpty());
        follow.m_parser.feed("^WS100 015;");
        follow.requestBand(1);
        check("Measured RF also defers manual band changes", writes.isEmpty());
        follow.m_parser.feed("^WS000 000;");
        check("Manual band applies when RF is idle", writes.count("^BN01;") == 1);
        follow.m_bandSentAt = follow.m_clock.elapsed() - 3001;
        writes.clear();
        follow.updateBand();
        follow.updateBand();
        check("Missing readback times out without retry storm", writes.isEmpty() && follow.m_failedBand == 1);
        follow.requestBand(1);
        check("Explicit reselection retries a failed command", writes.count("^BN01;") == 1);
        follow.m_parser.feed("^BN01;");
        follow.m_parser.feed("^OS1;^WS100 015;");
        check("Fresh powered Operate telemetry selects amplifier meters", follow.hasLiveTxMeters());
        follow.m_received["WS"] = follow.m_clock.elapsed() - 3001;
        follow.expireTelemetry();
        check("Individual stale power clears despite other valid replies", !follow.lastStatus().forwardPowerW && follow.isResponding());
        check("Other live replies cannot retain amplifier meter selection after power expires",
              !follow.hasLiveTxMeters());
        follow.m_parser.feed("^BN01;^OS1;");
        check("Band and Operate replies cannot restore the expired amplifier meter source",
              !follow.hasLiveTxMeters());
        writes.clear();
        follow.requestBand(5);
        check("Unknown RF measurement blocks a band change", writes.isEmpty());
        follow.m_parser.feed("^WS000 000;");
        check("A fresh measured zero restores amplifier meter selection", follow.hasLiveTxMeters());
        follow.m_parser.feed("^OS0;");
        check("Standby releases amplifier meter selection", !follow.hasLiveTxMeters());
        follow.m_parser.feed("^BN05;");
        writes.clear();
        follow.setTransmitting(true);
        follow.setPower(false);
        check("Power switch does not act during TX", writes.isEmpty());
        follow.setTransmitting(false);
        follow.setPower(false);
        check("OFF uses documented command", writes.contains("^ON0;"));
        writes.clear();
        follow.m_connectCmdsDone = true;
        for (int i = 0; i < 10; ++i) { follow.slowPollTick(); }
        check("Power-off polling never sends bootloader P via speaker query", !writes.contains("^SP;"));
        writes.clear();
        for (int i = 0; i < 25; ++i) { follow.fastPollTick(); follow.slowPollTick(); }
        check("OFF stops normal polling immediately", writes.isEmpty());
        follow.m_parser.feed("^ON1;^WS100 015;^OS1;");
        check("In-flight replies cannot revive an OFF request", follow.m_powerCheckOnly && !follow.isResponding());
        follow.m_lastPowerCheckAt = follow.m_clock.elapsed() - Kpa500Connection::kPowerCheckMs;
        follow.fastPollTick();
        check("Off-state check sends only the non-waking power query", writes == QStringList{"^ON;"});
        writes.clear();
        for (int i = 0; i < 10; ++i) { follow.fastPollTick(); follow.slowPollTick(); }
        check("Power checks do not repeat before five seconds", writes.isEmpty());
        follow.m_parser.feed("^ON0;");
        check("Confirmed OFF clears operating telemetry", follow.lastStatus().powerOn == false && !follow.lastStatus().operate && !follow.lastStatus().forwardPowerW);
        follow.m_lastPowerCheckAt = follow.m_clock.elapsed() - Kpa500Connection::kPowerCheckMs;
        follow.fastPollTick();
        follow.m_parser.feed("^ON1;");
        writes.clear();
        for (int i = 0; i < 4; ++i) { follow.fastPollTick(); }
        check("Front-panel ON resumes identity and telemetry polling", !follow.m_powerCheckOnly && !follow.m_powerOffRequested && writes.contains("^RVM;") && writes.contains("^SN;") && writes.contains("^WS;"));
        writes.clear();
        follow.m_parser.feed("^ON0;");
        follow.fastPollTick(); follow.slowPollTick();
        check("Front-panel OFF also stops normal polling", follow.m_powerCheckOnly && writes.isEmpty());
        follow.setPower(true);
        check("ON uses single bootloader P", writes == QStringList{"P"});
        follow.slowPollTick();
        follow.fastPollTick();
        check("Wake allows boot checks without poll traffic", writes == QStringList{"P"});
        for (int i = 0; i < 10; ++i) { follow.fastPollTick(); }
        check("Wake delay is followed only by a power check", writes == QStringList{"P", "^ON;"});
        follow.m_parser.feed("^ON1;");
        check("Explicit wake resumes only after ON readback", !follow.m_powerCheckOnly && follow.lastStatus().powerOn == true);
        follow.disconnect();
        writes.clear();
        follow.setPower(true);
        follow.requestBand(3);
        check("Disconnected controls emit no serial commands", writes.isEmpty());

        Kpa500TestStream retryStream;
        Kpa500Connection retryConnection;
        retryConnection.m_device = &retryStream;
        retryConnection.setAutoReconnect(true);
        retryConnection.armReconnect();
        check("An automatic retry can be armed", retryConnection.m_reconnectTimer.isActive());
        retryConnection.setAutoReconnect(false);
        check("Disabling automatic reconnect cancels a pending retry",
              !retryConnection.m_reconnectTimer.isActive());
        retryConnection.armReconnect();
        check("Automatic retry cannot rearm while disabled",
              !retryConnection.m_reconnectTimer.isActive());
        // Inject the successful completion of an explicit connection attempt.
        retryConnection.onTransportUp();
        retryConnection.m_parser.feed("^ON1;^OS0;");
        check("Explicit connection can become live with automatic reconnect disabled",
              retryConnection.isConnected() && retryConnection.isResponding()
              && retryConnection.lastStatus().operate == false);
        retryConnection.disconnect();
        check("Explicit disconnect leaves no pending automatic retry",
              !retryConnection.m_reconnectTimer.isActive());
        Kpa500TestStream tcpStream;
        Kpa500Connection tcp;
        int connectedEvents = 0;
        int disconnectedEvents = 0;
        int failures = 0;
        QObject::connect(&tcp, &Kpa500Connection::connected, [&connectedEvents] { ++connectedEvents; });
        QObject::connect(&tcp, &Kpa500Connection::disconnected, [&disconnectedEvents] { ++disconnectedEvents; });
        QObject::connect(&tcp, &Kpa500Connection::connectionFailed,
                         [&failures](const QString&) { ++failures; });
        tcp.setAutoReconnect(true);
        tcp.connectToHost("", 7000);
        tcp.connectToHost("localhost", 0);
        tcp.connectToHost("tcp://localhost", 7000);
        tcp.connectToHost("bad host", 7000);
        check("Invalid TCP endpoints fail before starting I/O or reconnect",
              failures == 4 && !tcp.isConnecting() && !tcp.isConnected()
              && !tcp.m_connectTimer.isActive() && !tcp.m_reconnectTimer.isActive()
              && tcp.m_socket.state() == QAbstractSocket::UnconnectedState);

        // Inject a pending raw TCP connection instead of opening a socket.
        tcp.m_network = true;
        tcp.m_lastHost = "2001:db8::1";
        tcp.m_lastTcpPort = 7000;
        tcp.m_connecting = true;
        tcp.m_device = &tcpStream;
        tcp.m_connectTimer.start();
        check("TCP description names the endpoint and server-owned baud",
              tcp.isNetworkConnection() && tcp.isConnecting()
              && tcp.description().contains("[2001:db8::1]:7000")
              && tcp.description().contains("baud set on server"));
        tcp.connectToHost("another-host", 9000);
        check("A second connect cannot replace an in-flight endpoint",
              tcp.m_lastHost == "2001:db8::1" && tcp.m_lastTcpPort == 7000);
        tcp.disconnect();
        QMetaObject::invokeMethod(&tcp.m_socket, "connected", Qt::DirectConnection);
        check("Disconnect cancels a pending connection and ignores its late completion",
              !tcp.isConnecting() && !tcp.isConnected() && !tcp.isDetected()
              && !tcp.m_connectTimer.isActive() && !tcp.m_reconnectTimer.isActive()
              && !tcp.m_device && connectedEvents == 0 && disconnectedEvents == 1);
        tcp.retryConnection();
        check("A cancelled connection cannot restart through a stale retry event",
              !tcp.isConnecting() && tcp.m_socket.state() == QAbstractSocket::UnconnectedState);

        tcp.m_deliberateDisconnect = false;
        tcp.m_connecting = true;
        tcp.m_device = &tcpStream;
        tcp.m_connectTimer.start();
        tcp.onConnectTimeout();
        check("A pending TCP deadline fails once and schedules an automatic retry",
              failures == 5 && disconnectedEvents == 2 && !tcp.isConnecting()
              && !tcp.isConnected() && !tcp.m_device && tcp.m_reconnectTimer.isActive()
              && !tcp.m_connectTimer.isActive());
        tcp.setAutoReconnect(false);
        tcp.retryConnection();
        check("Disabling reconnect also suppresses a stale retry callback",
              !tcp.isConnecting() && !tcp.m_reconnectTimer.isActive());

        tcp.m_connecting = true;
        tcp.m_device = &tcpStream;
        tcp.m_connectTimer.start();
        tcp.onTransportUp();
        check("TCP transport-up sends only the null and non-waking power probes",
              tcp.isConnected() && !tcp.isConnecting() && !tcp.isDetected()
              && !tcp.m_connectTimer.isActive() && connectedEvents == 1
              && tcpStream.written == ";^ON;");
        tcpStream.incoming = "^ON";
        tcp.onReadyRead();
        check("A TCP fragment cannot establish amplifier detection", !tcp.isDetected());
        tcpStream.incoming = "1;^WS000 000;^OS0;";
        tcp.onReadyRead();
        check("Fragmented and coalesced TCP bytes share the validated serial parser",
              tcp.isDetected() && tcp.isResponding() && tcp.lastStatus().powerOn == true
              && tcp.lastStatus().forwardPowerW == 0.0f && tcp.lastStatus().operate == false);
        tcp.m_parser.feed("^WS9");
        tcp.setAutoReconnect(true);
        QMetaObject::invokeMethod(&tcp.m_socket, "disconnected", Qt::DirectConnection);
        check("A dropped TCP stream clears detection, telemetry and poll timers",
              !tcp.isConnected() && !tcp.isDetected() && !tcp.isResponding()
              && !tcp.lastStatus().hasAnyField() && !tcp.m_device
              && !tcp.m_fastPollTimer.isActive() && !tcp.m_slowPollTimer.isActive()
              && tcp.m_reconnectTimer.isActive());
        tcp.m_device = &tcpStream;
        tcp.onTransportUp();
        tcpStream.incoming = "99 015;^OS1;";
        tcp.onReadyRead();
        check("TCP reconnect requires fresh detection and discards the old partial frame",
              !tcp.isDetected() && !tcp.lastStatus().hasAnyField()
              && !tcp.m_reconnectTimer.isActive());
        tcpStream.incoming = "^ON0;";
        tcp.onReadyRead();
        tcpStream.written.clear();
        tcp.fastPollTick();
        tcp.slowPollTick();
        check("A powered-off amplifier also keeps TCP quiet between power checks",
              tcp.isDetected() && tcp.lastStatus().powerOn == false && tcpStream.written.isEmpty());

        tcpStream.pendingBytes = Kpa500Connection::kMaxPendingWriteBytes;
        tcp.sendRaw("^ON;");
        check("A stalled stream closes before adding more buffered commands",
              !tcp.isConnected() && !tcp.isDetected() && !tcp.m_device
              && tcpStream.written.isEmpty() && failures == 6);
        tcpStream.pendingBytes = 0;
        tcp.m_device = &tcpStream;
        tcpStream.failWrite = true;
        tcp.onTransportUp();
        check("A failed initial write cannot announce a connection or start polling",
              !tcp.isConnected() && !tcp.m_device && connectedEvents == 2
              && !tcp.m_fastPollTimer.isActive() && !tcp.m_slowPollTimer.isActive()
              && failures == 7);
        tcpStream.failWrite = false;
        tcpStream.shortWrite = true;
        tcp.m_device = &tcpStream;
        tcp.onTransportUp();
        check("A partial write fails closed instead of replaying a partial command",
              !tcp.isConnected() && !tcp.m_device && connectedEvents == 2 && failures == 8);
        tcp.disconnect();
        check("Final TCP cancellation leaves no poll, connect or retry timers",
              !tcp.m_fastPollTimer.isActive() && !tcp.m_slowPollTimer.isActive()
              && !tcp.m_connectTimer.isActive() && !tcp.m_reconnectTimer.isActive());
        Kpa500TestStream maintenanceStream;
        Kpa500Connection maintenance;
        QString lastMaintenanceError;
        QString lastMaintenanceMessage;
        QByteArray configuration;
        QByteArray faultHistory;
        QString banner;
        int maintenanceSuccesses = 0;
        QObject::connect(&maintenance, &Kpa500Connection::maintenanceFailed,
                         [&lastMaintenanceError](const QString& reason) { lastMaintenanceError = reason; });
        QObject::connect(&maintenance, &Kpa500Connection::maintenanceFinished,
                         [&](const QString& message) { lastMaintenanceMessage = message; ++maintenanceSuccesses; });
        QObject::connect(&maintenance, &Kpa500Connection::configurationRead,
                         [&configuration](const QByteArray& data) { configuration = data; });
        QObject::connect(&maintenance, &Kpa500Connection::faultHistoryRead,
                         [&faultHistory](const QByteArray& data) { faultHistory = data; });
        QObject::connect(&maintenance, &Kpa500Connection::powerOnBannerRead,
                         [&banner](const QString& text) { banner = text; });
        const auto readyForMaintenance = [&] {
            maintenance.disconnect();
            maintenance.m_deliberateDisconnect = false;
            maintenance.m_transmitting = false;
            maintenance.m_txBand = -1;
            maintenance.m_device = &maintenanceStream;
            maintenance.onTransportUp();
            maintenance.m_parser.feed("^ON1;^OS0;^WS000 000;^RVM01.54;^SN01199;^BN05;");
            maintenanceStream.written.clear();
            lastMaintenanceError.clear();
        };
        const auto eepromReply = [](int address, const QByteArray& data, bool writeAck = false) {
            QByteArray payload;
            payload.append(char((address >> 8) & 0xff));
            payload.append(char(address & 0xff));
            payload.append(char(data.size()));
            if (!writeAck) { payload += data; }
            unsigned int sum = 0;
            for (const char byte : payload) { sum += static_cast<unsigned char>(byte); }
            payload.append(char((0x100u - (sum & 0xffu)) & 0xffu));
            return QByteArray(writeAck ? "^EW" : "^ER") + payload.toHex().toUpper() + ';';
        };
        readyForMaintenance();
        check("Maintenance requires confirmed supported firmware and identity",
              maintenance.maintenanceUnavailableReason(true).isEmpty());
        maintenance.m_status.firmwareVersion = "01.53";
        maintenance.readConfiguration();
        check("Unknown firmware cannot start EEPROM I/O",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.m_status.serialNumber.reset();
        maintenance.readConfiguration();
        check("Missing serial identity cannot start EEPROM I/O",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.setTransmitting(true);
        maintenance.readFaultHistory();
        maintenance.writePowerOnBanner("TEST");
        check("Radio transmit blocks both EEPROM reads and writes",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.m_parser.feed("^WS001 015;");
        maintenance.readConfiguration();
        check("Positive amplifier RF also blocks maintenance",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.m_parser.feed("^ON0;");
        maintenance.readConfiguration();
        maintenance.writePowerOnBanner("TEST");
        check("Powered-off maintenance requests cannot wake or write the amplifier",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.m_status.operate = true;
        maintenance.writePowerOnBanner("TEST");
        check("Operate state blocks configuration writes",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());
        readyForMaintenance();
        maintenance.writePowerOnBanner("TOO LONG!");
        maintenance.writePowerOnBanner(QString::fromUtf8("é"));
        maintenance.restoreConfiguration(QByteArray(223, 'a'), "01199", "01.54");
        maintenance.restoreConfiguration(QByteArray(224, 'a'), "01200", "01.54");
        maintenance.restoreConfiguration(QByteArray(224, 'a'), "01199", "01.53");
        check("Invalid banner, backup size and identity are rejected before I/O",
              !maintenance.maintenanceBusy() && maintenanceStream.written.isEmpty());

        QByteArray backup(224, '\0');
        for (int i = 0; i < backup.size(); ++i) { backup[i] = char(i); }
        maintenance.readConfiguration();
        check("Configuration read starts at zero with a bounded 64-byte request",
              maintenance.maintenanceBusy() && maintenanceStream.written == "^ER000040C0;"
              && !maintenance.m_fastPollTimer.isActive() && !maintenance.m_slowPollTimer.isActive());
        const QByteArray initialRequest = maintenanceStream.written;
        maintenance.readPowerOnBanner();
        maintenance.fastPollTick();
        maintenance.slowPollTick();
        maintenance.setPower(false);
        maintenance.setOperate(true);
        maintenance.clearFault();
        maintenance.setFanSpeed(2);
        maintenance.setSpeaker(true);
        maintenance.setTxContext(3, "A", false);
        maintenance.setFollowTx(false);
        maintenance.requestBand(1);
        check("Maintenance excludes concurrent operations, normal polls and control setters",
              maintenance.maintenanceBusy() && maintenanceStream.written == initialRequest);
        for (int address = 0; address < 224; address += 64) {
            maintenance.m_parser.feed(eepromReply(address, backup.mid(address, std::min(64, 224 - address))));
        }
        check("Four configuration blocks produce the exact complete backup",
              !maintenance.maintenanceBusy() && configuration == backup && maintenanceSuccesses == 1
              && maintenanceStream.written == "^ER000040C0;^ER00404080;^ER00804040;^ER00C02020;");
        check("Completion reacquires live state without losing backup identity",
              !maintenance.isResponding() && !maintenance.lastStatus().forwardPowerW
              && maintenance.lastStatus().serialNumber == "01199"
              && maintenance.m_fastPollTimer.isActive() && maintenance.m_powerCheckOnly);

        readyForMaintenance();
        maintenance.readFaultHistory();
        const QByteArray eeprom(1024, char(0xa5));
        for (int address = 0; address < 1024; address += 64) {
            maintenance.m_parser.feed(eepromReply(address, eeprom.mid(address, 64)));
        }
        check("Fault history reads all 1024 bytes without a write command",
              !maintenance.maintenanceBusy() && faultHistory == eeprom
              && maintenanceStream.written.count("^ER") == 16
              && !maintenanceStream.written.contains("^EW"));
        readyForMaintenance();
        maintenance.readPowerOnBanner();
        maintenance.m_parser.feed(eepromReply(0x50, QByteArray("KPA500\0x", 8)));
        check("Power-on banner stops at the stored NUL",
              !maintenance.maintenanceBusy() && banner == "KPA500"
              && maintenanceStream.written == "^ER005008A8;");

        readyForMaintenance();
        maintenance.readConfiguration();
        QByteArray corrupted = eepromReply(0, backup.left(64));
        corrupted[corrupted.size() - 2] = corrupted[corrupted.size() - 2] == '0' ? '1' : '0';
        maintenance.m_parser.feed(corrupted);
        check("Bad EEPROM checksum cannot populate or advance a transaction",
              !maintenance.maintenanceBusy() && maintenanceStream.written.count("^ER") == 1
              && lastMaintenanceError.contains("Invalid"));
        readyForMaintenance();
        maintenance.readConfiguration();
        maintenance.m_parser.feed(eepromReply(64, backup.left(64)));
        check("Valid-checksum reply for the wrong address is rejected",
              !maintenance.maintenanceBusy() && maintenanceStream.written.count("^ER") == 1);
        readyForMaintenance();
        maintenance.readConfiguration();
        maintenance.m_parser.feed(eepromReply(0, backup.left(32)));
        check("Wrong EEPROM reply length is rejected", !maintenance.maintenanceBusy());
        readyForMaintenance();
        maintenance.readConfiguration();
        maintenance.onMaintenanceTimeout();
        maintenance.onMaintenanceTimeout();
        maintenance.onMaintenanceTimeout();
        check("Read timeout retries at most twice before releasing the transaction",
              !maintenance.maintenanceBusy() && maintenanceStream.written.count("^ER") == 3
              && lastMaintenanceError.contains("timed out"));
        readyForMaintenance();
        maintenance.readFaultHistory();
        maintenance.cancelMaintenance();
        const QByteArray cancelledRead = maintenanceStream.written;
        maintenance.m_parser.feed(eepromReply(0, backup.left(64)));
        check("Cancelled maintenance cannot be resumed by a late reply",
              !maintenance.maintenanceBusy() && maintenanceStream.written == cancelledRead
              && !maintenance.m_maintenanceTimer.isActive());

        readyForMaintenance();
        maintenance.m_received["ON"] = -10000;
        maintenance.m_received["OS"] = -10000;
        maintenance.m_received["WS"] = -10000;
        maintenance.writePowerOnBanner("LABTEST1");
        check("Stale cached Standby and power authorize only fresh read-only preflight",
              maintenance.maintenanceBusy() && maintenanceStream.written == "^SN;");
        maintenance.m_parser.feed("^ON1;^OS0;^WS000 000;");
        check("In-flight status replies cannot pass the identity preflight fence",
              maintenanceStream.written == "^SN;");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;");
        check("No EEPROM write occurs before a fresh zero-RF reply",
              maintenanceStream.written == "^SN;^ON;^OS;^WS;");
        maintenance.m_parser.feed("^WS000 000;");
        check("Explicit banner intent writes its ASCII bytes and terminator after preflight",
              maintenanceStream.written.endsWith("^EW0050094C414254455354310067;"));
        const int successesBeforeAck = maintenanceSuccesses;
        maintenance.m_parser.feed("^EW005009A7;");
        check("Write acknowledgement starts readback and cannot announce success",
              maintenance.maintenanceBusy() && maintenanceSuccesses == successesBeforeAck
              && maintenanceStream.written.endsWith("^ER005009A7;"));
        maintenance.m_parser.feed(eepromReply(0x50, QByteArray("LABTEST1\0", 9)));
        check("Verified banner reports success and manual power-cycle guidance without reboot",
              !maintenance.maintenanceBusy() && banner == "LABTEST1"
              && maintenanceSuccesses == successesBeforeAck + 1
              && lastMaintenanceMessage.contains("Power-cycle")
              && !maintenanceStream.written.contains("^rpm") && !maintenanceStream.written.contains("^RPM"));

        readyForMaintenance();
        maintenance.restoreConfiguration(backup, "01199", "01.54");
        for (int address = 0; address < 224; address += 64) {
            maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS000 000;");
            maintenance.m_parser.feed(eepromReply(address, backup.mid(address, std::min(64, 224 - address)), true));
        }
        check("Restore writes four blocks, each after a fresh Standby and zero-RF preflight",
              maintenance.maintenanceBusy() && maintenanceStream.written.count("^EW") == 4
              && maintenanceStream.written.count("^ON;") == 4
              && maintenanceStream.written.count("^OS;") == 4
              && maintenanceStream.written.count("^WS;") == 4);
        for (int address = 0; address < 224; address += 64) {
            maintenance.m_parser.feed(eepromReply(address, backup.mid(address, std::min(64, 224 - address))));
        }
        check("Restore completes only after readback of all written bytes",
              !maintenance.maintenanceBusy() && maintenanceStream.written.count("^ER") == 4
              && !maintenanceStream.written.contains("^rpm"));

        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;");
        maintenance.m_received["ON"] = maintenance.m_clock.elapsed() - 3001;
        maintenance.m_parser.feed("^WS000 000;");
        check("Expired preflight power readback cannot authorize an EEPROM write",
              !maintenance.maintenanceBusy() && !maintenanceStream.written.contains("^EW")
              && lastMaintenanceError.contains("expired"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^OS0;^WS000 000;^ON2;");
        check("Out-of-order and malformed status cannot complete write preflight",
              maintenance.maintenanceBusy() && maintenanceStream.written == "^SN;^ON;");
        maintenance.cancelMaintenance();
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01200;");
        check("A mismatched identity fence aborts before reading or writing configuration",
              !maintenance.maintenanceBusy() && maintenanceStream.written == "^SN;"
              && lastMaintenanceError.contains("identity"));
        readyForMaintenance();
        maintenance.readConfiguration();
        maintenance.m_parser.feed(eepromReply(0, backup.left(64), true));
        check("An EW acknowledgement cannot satisfy an ER read request",
              !maintenance.maintenanceBusy() && lastMaintenanceError.contains("unexpected"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS1;");
        check("Fresh Operate reply aborts writing despite cached Standby",
              !maintenance.maintenanceBusy() && !maintenanceStream.written.contains("^EW"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS001 015;");
        check("Fresh positive RF aborts preflight without a write",
              !maintenance.maintenanceBusy() && !maintenanceStream.written.contains("^EW"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS000 000;");
        maintenance.onMaintenanceTimeout();
        check("An unacknowledged write is never retried and reports possible partial change",
              !maintenance.maintenanceBusy() && maintenanceStream.written.count("^EW") == 1
              && lastMaintenanceError.contains("incomplete") && lastMaintenanceError.contains("may already"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS000 000;");
        maintenance.m_parser.feed(eepromReply(0x50, QByteArray("TEST\0", 5), true));
        maintenance.m_parser.feed(eepromReply(0x50, QByteArray("FAIL\0", 5)));
        check("Mismatching readback fails instead of claiming a successful write",
              !maintenance.maintenanceBusy() && lastMaintenanceError.contains("did not match")
              && lastMaintenanceError.contains("incomplete"));
        readyForMaintenance();
        maintenance.writePowerOnBanner("TEST");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS000 000;");
        maintenance.cancelMaintenance();
        check("Cancelling after a write reports incomplete hardware state",
              !maintenance.maintenanceBusy() && lastMaintenanceError.contains("incomplete")
              && maintenanceStream.written.count("^EW") == 1);
        readyForMaintenance();
        maintenance.restoreConfiguration(backup, "01199", "01.54");
        maintenance.m_parser.feed("^SN01199;^ON1;^OS0;^WS000 000;");
        maintenance.disconnect();
        check("Disconnect discards maintenance and never retains a write retry",
              !maintenance.maintenanceBusy() && !maintenance.m_maintenanceTimer.isActive()
              && lastMaintenanceError.contains("incomplete"));
        readyForMaintenance();
        maintenance.readFaultHistory();
        maintenance.setTxContext(5, "A", true);
        check("A radio TX transition immediately aborts maintenance",
              !maintenance.maintenanceBusy() && lastMaintenanceError.contains("transmitting"));
        readyForMaintenance();
        maintenance.readFaultHistory();
        maintenance.m_parser.feed("^WS002 015;");
        check("Amplifier RF readback immediately aborts maintenance", !maintenance.maintenanceBusy());
        maintenance.disconnect();
        return failed == 0 ? 0 : 1;
    }
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return AetherSDR::Kpa500ConnectionTest::run();
}
