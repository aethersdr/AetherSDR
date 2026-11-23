// Feed captured success and documented failure frames into the state machines.
// No socket or synthetic firmware peer is involved.
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "models/AntennaGeniusModel.h"
#include "gui/PeripheralAuthStore.h"
#include "core/PeripheralAuthCode.h"
#include "core/PeripheralEndpointFallback.h"
#include "core/PeripheralRemovalGuard.h"
#include "core/AppSettings.h"
#include "TestSettingsProfile.h"
#include "models/AmpModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QMetaObject>
#include <QSignalSpy>
#include <QStringList>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <utility>

namespace AetherSDR {
struct PeripheralConnectionTestAccess {
    template<typename Connection>
    static void injectConnect(Connection& connection, int& calls)
    {
        connection.m_connectTransport = [&calls](const QString&, quint16) { ++calls; };
    }
};
// Inject only the auth write, never a live peer, without widening the model's
// production constructor API.
struct AntennaGeniusModelTestAccess {
    static void setAuthWriter(AntennaGeniusModel& model,
                             std::function<void(const QByteArray&)> writer = [](const QByteArray&) {})
    {
        model.m_authCommandWriter = std::move(writer);
    }
    static void captureAntennaCommands(AntennaGeniusModel& model, QStringList& lines)
    {
        model.m_commandWriter = [&lines](const QByteArray& bytes) {
            lines.append(QString::fromUtf8(bytes));
        };
        model.m_connected = true;
        model.m_device.name = QStringLiteral("Antenna Genius");
        model.m_device.radioPorts = 2;
        const quint16 bandMask = quint16(1U << 5);
        model.m_antennas = {
            {1, QStringLiteral("RX-only"), 0, bandMask, 0},
            {2, QStringLiteral("Both"), bandMask, bandMask, 0},
            {3, QStringLiteral("TX-only"), bandMask, 0, 0},
        };
        model.m_bands = {{5, QStringLiteral("20m"), 14.0, 14.35}};
        model.m_portA = {1, true, QStringLiteral("MANUAL"), 5, 1, 2, false, false};
        model.m_portB = {2, true, QStringLiteral("MANUAL"), 5, 0, 0, false, false};
    }
    static void recallBand(AntennaGeniusModel& model, int portId, int bandId)
    {
        model.recallBandAntennas(portId, bandId);
    }
    static void status(AntennaGeniusModel& model, int portId, int band, int rx, int tx)
    {
        model.processStatus(QStringLiteral("port %1 band=%2 rxant=%3 txant=%4")
                                .arg(portId).arg(band).arg(rx).arg(tx));
    }
    static void response(AntennaGeniusModel& model, int seq, int code, const QString& body)
    {
        model.processResponse(seq, code, body);
    }
    static void beginInit(AntennaGeniusModel& model) { model.runInitSequence(); }
    static void setBand(AntennaGeniusModel& model, int portId, int band)
    {
        (portId == 1 ? model.m_portA : model.m_portB).band = band;
    }
    static void setOtherPort(AntennaGeniusModel& model, int rx, int tx)
    {
        model.m_portB.rxAntenna = rx;
        model.m_portB.txAntenna = tx;
    }
};

// Inject only the command writer and the connected flag, never a live peer.
struct PgxlConnectionTestAccess {
    static void captureCommands(PgxlConnection& connection, QStringList& lines)
    {
        connection.m_commandWriter = [&lines](const QByteArray& line) {
            lines.append(QString::fromUtf8(line));
        };
        connection.m_connected = true;
    }
};
}

using namespace AetherSDR;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (false)

template<typename Connection>
void feed(Connection& connection, const char* frame)
{
    CHECK(QMetaObject::invokeMethod(&connection, "processLine", Qt::DirectConnection,
                                    Q_ARG(QString, QString::fromLatin1(frame))));
}

template<typename Connection>
void checkReconnectSuppression(const QString& timerName, const char* greeting,
                               const char* rejection, quint16 port)
{
    for (const char* failure : {"rejection", "onAuthTimeout", "onDisconnected"}) {
        Connection connection;
        connection.setAutoReconnect(true);
        QTimer* retry = connection.template findChild<QTimer*>(timerName);
        CHECK(retry != nullptr);
        if (!retry) {
            continue;
        }
        QSignalSpy required(&connection, &Connection::authCodeRequired);
        const bool rejecting = QByteArray(failure) == "rejection";
        for (int strike = 1; strike <= (rejecting ? 1 : 3); ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, port)));
            CHECK(!retry->isActive());
            feed(connection, greeting);
            CHECK(required.size() == strike);
            if (required.isEmpty()) {
                break;
            }
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                             QStringLiteral("saved-code"));
            if (rejecting) {
                feed(connection, rejection);
            } else {
                CHECK(QMetaObject::invokeMethod(&connection, failure, Qt::DirectConnection));
            }
            const bool blocked = rejecting || strike == 3;
            CHECK(connection.isAuthBlocked() == blocked);
            CHECK(retry->isActive() == !blocked);
        }
        // A close or error delivered after rejection must not re-arm retries.
        CHECK(QMetaObject::invokeMethod(&connection, "onDisconnected", Qt::DirectConnection));
        CHECK(!retry->isActive());
        CHECK(QMetaObject::invokeMethod(&connection, "onError", Qt::DirectConnection,
            Q_ARG(QAbstractSocket::SocketError, QAbstractSocket::RemoteHostClosedError)));
        CHECK(!retry->isActive());
        retry->stop(); // Never allow a timer to connect to a synthetic peer.
    }
}

// A keychain outage ("unavailable") fails the attempt without blocking, so the
// reconnect timer is armed; an empty code with a working keychain blocks.
// unreachable() reports only an automatic attempt whose socket never
// connected; an alternate attempt keeps the reconnect target, and a missing
// saved code for the alternate address does not block reconnects.
template<typename Connection>
void checkUnreachableAndAlternate(const QString& timerName, const char* greeting,
                                  quint16 port)
{
    const QString manual = QStringLiteral("tgxl.example.net");
    const QString reported = QStringLiteral("192.0.2.20");
    auto attempt = [&](Connection& connection, const char* method, const QString& host) {
        CHECK(QMetaObject::invokeMethod(&connection, method, Qt::DirectConnection,
            Q_ARG(QString, host), Q_ARG(quint16, port)));
    };
    auto refuse = [](Connection& connection) {
        CHECK(QMetaObject::invokeMethod(&connection, "onError", Qt::DirectConnection,
            Q_ARG(QAbstractSocket::SocketError, QAbstractSocket::ConnectionRefusedError)));
    };
    for (const char* scenario : {"automatic", "operator", "tcpReached", "deliberate"}) {
        Connection connection;
        connection.setAutoReconnect(true);
        QTimer* retry = connection.template findChild<QTimer*>(timerName);
        CHECK(retry != nullptr);
        QSignalSpy unreachable(&connection, &Connection::unreachable);
        const QByteArray which(scenario);
        attempt(connection, which == "operator" ? "beginAttemptAt" : "beginAutomaticAttemptAt",
                manual);
        if (which == "tcpReached") {
            CHECK(QMetaObject::invokeMethod(&connection, "onConnected", Qt::DirectConnection));
        } else if (which == "deliberate") {
            connection.disconnect();
        }
        refuse(connection);
        const bool expected = which == "automatic";
        CHECK(unreachable.size() == (expected ? 1 : 0));
        if (expected && !unreachable.isEmpty()) {
            CHECK(unreachable.first().at(0).toString() == manual);
        }
        if (retry) {
            retry->stop(); // Never allow a timer to connect to a synthetic peer.
        }
    }
    {
        Connection connection;
        connection.setAutoReconnect(true);
        QTimer* retry = connection.template findChild<QTimer*>(timerName);
        CHECK(retry != nullptr);
        if (!retry) {
            return;
        }
        QSignalSpy unreachable(&connection, &Connection::unreachable);
        QSignalSpy required(&connection, &Connection::authCodeRequired);
        attempt(connection, "beginAutomaticAttemptAt", manual);
        attempt(connection, "beginAlternateAttemptAt", reported);
        CHECK(connection.attemptHost() == reported);
        CHECK(connection.reconnectHost() == manual);
        feed(connection, greeting);
        CHECK(required.size() == 1);
        if (!required.isEmpty()) {
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(), QString());
        }
        CHECK(!connection.isAuthBlocked());
        CHECK(retry->isActive());
        retry->stop();
        // The alternate address failing at TCP is not itself reported.
        attempt(connection, "beginAlternateAttemptAt", reported);
        refuse(connection);
        CHECK(unreachable.isEmpty());
        CHECK(connection.reconnectHost() == manual);
        retry->stop();
    }
}

// A pending removal must stop every route to a socket, not only the explicit
// Connect: automatic, alternate and the reconnect timer all reach openSocket().
template<typename Connection>
void checkRemovalGuardStopsEveryConnectPath(const QString& timerName,
                                            PeripheralRemovalGuard::Device device,
                                            void (Connection::*autoConnect)(const QString&, quint16),
                                            void (Connection::*alternate)(const QString&, quint16),
                                            void (Connection::*explicitConnect)(const QString&, quint16),
                                            quint16 port)
{
    const QString host = QStringLiteral("192.0.2.10");
    Connection connection;
    connection.setAutoReconnect(true);
    int opened = 0;
    PeripheralConnectionTestAccess::injectConnect(connection, opened);
    // Every attempt that opens announces itself, so the Setup list can show
    // Connecting… for automatic attempts too; a blocked one announces nothing.
    QSignalSpy started(&connection, &Connection::attemptStarted);
    QTimer* retry = connection.template findChild<QTimer*>(timerName);
    CHECK(retry != nullptr);
    if (!retry) {
        return;
    }
    retry->stop();
    // Seed the reconnect target the way a first automatic attempt does.
    CHECK(QMetaObject::invokeMethod(&connection, "beginAutomaticAttemptAt", Qt::DirectConnection,
        Q_ARG(QString, host), Q_ARG(quint16, port)));
    {
        PeripheralRemovalGuard guard(device);
        (connection.*autoConnect)(host, port);
        (connection.*alternate)(host, port);
        (connection.*explicitConnect)(host, port);
        CHECK(QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection));
        CHECK(opened == 0);
        CHECK(started.isEmpty());
        // A blocked attempt leaves the reconnect target alone.
        (connection.*autoConnect)(QStringLiteral("192.0.2.99"), port);
        CHECK(connection.reconnectHost() == host);
    }
    CHECK(!PeripheralRemovalGuard::pending(device));
    (connection.*autoConnect)(host, port);
    CHECK(opened == 1);
    (connection.*alternate)(host, port);
    CHECK(opened == 2);
    CHECK(QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection));
    CHECK(opened == 3);
    CHECK(started.size() == 3);
    CHECK(!started.isEmpty() && started.constLast().at(0).toString() == host);
    retry->stop();
}

template<typename Connection>
void checkKeychainOutageKeepsReconnect(const QString& timerName, const char* greeting,
                                       quint16 port)
{
    for (const bool unavailable : {true, false}) {
        Connection connection;
        connection.setAutoReconnect(true);
        QTimer* retry = connection.template findChild<QTimer*>(timerName);
        CHECK(retry != nullptr);
        if (!retry) {
            continue;
        }
        QSignalSpy required(&connection, &Connection::authCodeRequired);
        QSignalSpy failed(&connection, &Connection::connectionFailed);
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, port)));
        feed(connection, greeting);
        CHECK(required.size() == 1);
        if (required.isEmpty()) {
            continue;
        }
        connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(), QString(),
                                         unavailable);
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked() == !unavailable);
        CHECK(retry->isActive() == unavailable);
        retry->stop(); // Never allow a timer to connect to a synthetic peer.
    }
}

// `setup` writes: the authcode key follows what `setup read` reported.
void checkSetupAuthcode(const QMap<QString, QString>& setupRead, const QString& expectedTail,
                        bool mustNotLeak)
{
    PgxlConnection connection;
    AmpModel model;
    model.setDirectConnection(&connection);
    QStringList sent;
    PgxlConnectionTestAccess::captureCommands(connection, sent);
    emit connection.statusUpdated({{QStringLiteral("meffa"), QStringLiteral("STANDBY")},
                                   {QStringLiteral("fanmode"), QStringLiteral("STANDARD")}});
    emit connection.setupRead(setupRead);
    model.setFanMode(QStringLiteral("broadcast"));
    CHECK(sent.size() == 1);
    if (sent.isEmpty()) {
        return;
    }
    CHECK(sent.constFirst().contains(QStringLiteral("|setup nickname=PGXL-HOME meffa=AUTO"
                                                    " ledintens=74 fanmode=BROADCAST")));
    CHECK(sent.constFirst().trimmed().endsWith(expectedTail));
    CHECK(sent.constFirst().contains(QStringLiteral("authcode=")) == (expectedTail == QLatin1String("authcode=")));
    if (mustNotLeak) {
        CHECK(!sent.constFirst().contains(QStringLiteral("1234")));
    }
}

void prepareAg(AntennaGeniusModel& connection,
               const QString& host = QStringLiteral("192.0.2.10"),
               quint16 port = 9007)
{
    CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                                    Q_ARG(QString, host), Q_ARG(quint16, port)));
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("peripheral-auth-handshake-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::Tgxl, QStringLiteral("sample"))
          == QByteArray("C1|auth sample\n")); // captured TGXL form
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::Pgxl, QStringLiteral("sample"))
          == QByteArray("C1|auth code=sample\n")); // captured PGXL form
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::AntennaGenius, QStringLiteral("sample"))
          == QByteArray("C1|auth code=sample\r\n")); // documented syntax, capture-compatible framing
    {
        auto& settings = AppSettings::instance();
        const QString legacyKey = QStringLiteral("AG_Port1_Band5");
        const QString rootKey = QStringLiteral("AntennaGenius");
        settings.remove(rootKey);
        settings.setValue(legacyKey, QStringLiteral("1"));

        AntennaGeniusModel connection;
        QStringList commands;
        AntennaGeniusModelTestAccess::captureAntennaCommands(connection, commands);
        CHECK(!settings.contains(legacyKey));
        AntennaGeniusModelTestAccess::recallBand(connection, 1, 5);
        CHECK(commands.size() == 1);
        CHECK(commands.constLast().contains(QStringLiteral("port set 1 rxant=1 txant=2")));
        commands.clear();

        connection.selectRxAntenna(1, 3);
        CHECK(commands.isEmpty()); // TX-only antennas cannot be selected for RX.
        connection.selectRxAntenna(1, 2);
        CHECK(commands.size() == 1);
        CHECK(commands.constLast().contains(QStringLiteral("port set 1 rxant=2 txant=2")));

        connection.selectTxAntenna(1, 3);
        CHECK(commands.size() == 2);
        CHECK(commands.constLast().contains(QStringLiteral("port set 1 rxant=2 txant=3")));
        CHECK(connection.portA().rxAntenna == 1); // Wait for device status.
        CHECK(connection.portA().txAntenna == 2);
        AntennaGeniusModelTestAccess::status(connection, 1, 5, 2, 3);
        CHECK(connection.portA().rxAntenna == 2);
        CHECK(connection.portA().txAntenna == 3);
        QString stored;
        CHECK(settings.readAppRowFromDisk(rootKey, stored));
        CHECK(stored.contains(QStringLiteral("\"tx\":3")));

        // A confirmed split pair is restored in one command.
        AntennaGeniusModel reloaded;
        QStringList reloadCommands;
        AntennaGeniusModelTestAccess::captureAntennaCommands(reloaded, reloadCommands);
        AntennaGeniusModelTestAccess::recallBand(reloaded, 1, 5);
        CHECK(reloadCommands.size() == 1);
        CHECK(reloadCommands.constLast().contains(QStringLiteral("rxant=2 txant=3")));
        reloaded.selectRxAntenna(1, 0);
        AntennaGeniusModelTestAccess::status(reloaded, 1, 5, 0, 3);
        AntennaGeniusModel zeroReloaded;
        reloadCommands.clear();
        AntennaGeniusModelTestAccess::captureAntennaCommands(zeroReloaded, reloadCommands);
        AntennaGeniusModelTestAccess::recallBand(zeroReloaded, 1, 5);
        CHECK(reloadCommands.constLast().contains(QStringLiteral("rxant=0 txant=3")));
        commands.clear();
        AntennaGeniusModelTestAccess::recallBand(connection, 1, 5);
        CHECK(commands.size() == 1);
        CHECK(commands.constFirst().contains(
            QStringLiteral("port set 1 rxant=2 txant=3")));

        // A refused pick neither changes the displayed state nor the memory.
        connection.selectTxAntenna(1, 2);
        const int rejectedSeq = commands.constLast().section('|', 0, 0).mid(1).toInt();
        AntennaGeniusModelTestAccess::response(connection, rejectedSeq, 1, QStringLiteral("denied"));
        CHECK(connection.portA().txAntenna == 3);
        commands.clear();
        AntennaGeniusModelTestAccess::recallBand(connection, 1, 5);
        CHECK(commands.constLast().contains(QStringLiteral("rxant=2 txant=3")));

        AntennaGeniusModelTestAccess::setBand(connection, 2, 0);
        commands.clear();
        connection.selectAntenna(2, 1);
        CHECK(commands.constLast().contains(QStringLiteral("port set 2 rxant=1 txant=0")));
        const int before = commands.size();
        connection.selectTxAntenna(2, 2);
        CHECK(commands.size() == before);

        // A conflicting unchanged half must not block clearing or changing the other.
        AntennaGeniusModelTestAccess::setOtherPort(connection, 2, 0);
        commands.clear();
        connection.selectTxAntenna(1, 0);
        CHECK(commands.size() == 1);
        CHECK(commands.constLast().contains(QStringLiteral("rxant=2 txant=0")));

        settings.remove(rootKey);
        settings.remove(legacyKey);
    }
    {
        auto& settings = AppSettings::instance();
        settings.remove(QStringLiteral("AntennaGenius"));
        settings.setValue(QStringLiteral("AG_Port1_Band5"), QStringLiteral("0"));
        AntennaGeniusModel connection;
        QStringList commands;
        AntennaGeniusModelTestAccess::captureAntennaCommands(connection, commands);
        AntennaGeniusModelTestAccess::recallBand(connection, 1, 5);
        CHECK(commands.isEmpty());
        settings.remove(QStringLiteral("AntennaGenius"));
        settings.setValue(QStringLiteral("AG_Port1_Band5"), QStringLiteral("2"));
        AntennaGeniusModel migrated;
        AntennaGeniusModelTestAccess::captureAntennaCommands(migrated, commands);
        AntennaGeniusModelTestAccess::recallBand(migrated, 1, 5);
        CHECK(commands.size() == 1);
        CHECK(commands.constLast().contains(QStringLiteral("rxant=2 txant=2")));
        settings.remove(QStringLiteral("AntennaGenius"));
    }
    {
        auto& settings = AppSettings::instance();
        settings.remove(QStringLiteral("AntennaGenius"));
        settings.setValue(QStringLiteral("AG_Port1_Band5"), QStringLiteral("2"));
        AntennaGeniusModel connection;
        QStringList commands;
        AntennaGeniusModelTestAccess::captureAntennaCommands(connection, commands);
        AntennaGeniusModelTestAccess::beginInit(connection);
        CHECK(commands.size() == 6);
        const int antSeq = commands.at(0).section('|', 0, 0).mid(1).toInt();
        const int portASeq = commands.at(2).section('|', 0, 0).mid(1).toInt();
        const int portBSeq = commands.at(3).section('|', 0, 0).mid(1).toInt();
        AntennaGeniusModelTestAccess::response(connection, antSeq, 0,
            QStringLiteral("antenna 2 name=Both tx=0020 rx=0020"));
        AntennaGeniusModelTestAccess::response(connection, antSeq, 0, QString());
        AntennaGeniusModelTestAccess::response(connection, portASeq, 0,
            QStringLiteral("port 1 band=5 rxant=1 txant=1"));
        AntennaGeniusModelTestAccess::response(connection, portASeq, 0, QString());
        CHECK(commands.size() == 6);
        connection.selectAntenna(1, 2);
        CHECK(commands.size() == 6);
        AntennaGeniusModelTestAccess::response(connection, portBSeq, 0,
            QStringLiteral("port 2 band=5 rxant=0 txant=2"));
        AntennaGeniusModelTestAccess::response(connection, portBSeq, 0, QString());
        CHECK(commands.size() == 6); // Port B's TX antenna prevents the recall.
        settings.remove(QStringLiteral("AntennaGenius"));
    }
    checkReconnectSuppression<TgxlConnection>(QStringLiteral("tgxlReconnectTimer"),
        "V1.2.17 AUTH", "R1|0|Unauthorized", 9010);
    checkReconnectSuppression<PgxlConnection>(QStringLiteral("pgxlReconnectTimer"),
        "V3.9.1 AUTH", "R1|FF|Denied", 9008);
    checkKeychainOutageKeepsReconnect<TgxlConnection>(QStringLiteral("tgxlReconnectTimer"),
        "V1.2.17 AUTH", 9010);
    checkKeychainOutageKeepsReconnect<PgxlConnection>(QStringLiteral("pgxlReconnectTimer"),
        "V3.9.1 AUTH", 9008);
    checkUnreachableAndAlternate<TgxlConnection>(QStringLiteral("tgxlReconnectTimer"),
        "V1.2.17 AUTH", 9010);
    checkUnreachableAndAlternate<PgxlConnection>(QStringLiteral("pgxlReconnectTimer"),
        "V3.9.1 AUTH", 9008);
    checkRemovalGuardStopsEveryConnectPath<TgxlConnection>(
        QStringLiteral("tgxlReconnectTimer"), PeripheralRemovalGuard::Device::Tgxl,
        &TgxlConnection::autoConnectToTgxl, &TgxlConnection::tryAlternateTgxl,
        &TgxlConnection::connectToTgxl, 9010);
    checkRemovalGuardStopsEveryConnectPath<PgxlConnection>(
        QStringLiteral("pgxlReconnectTimer"), PeripheralRemovalGuard::Device::Pgxl,
        &PgxlConnection::autoConnectToPgxl, &PgxlConnection::tryAlternatePgxl,
        &PgxlConnection::connectToPgxl, 9008);
    // A code set on the amplifier is never sent back; an empty one is sent as
    // the vendor utility does; a reply that omits the key gets none.
    checkSetupAuthcode({{QStringLiteral("ledintens"), QStringLiteral("74")},
                        {QStringLiteral("nickname"), QStringLiteral("PGXL-HOME")},
                        {QStringLiteral("authcode"), QStringLiteral("1234")}},
                       QStringLiteral("fanmode=BROADCAST"), true);
    checkSetupAuthcode({{QStringLiteral("ledintens"), QStringLiteral("74")},
                        {QStringLiteral("nickname"), QStringLiteral("PGXL-HOME")},
                        {QStringLiteral("authcode"), QString()}},
                       QStringLiteral("authcode="), false);
    checkSetupAuthcode({{QStringLiteral("ledintens"), QStringLiteral("74")},
                        {QStringLiteral("nickname"), QStringLiteral("PGXL-HOME")}},
                       QStringLiteral("fanmode=BROADCAST"), false);
    {
        // The radio-reported address is tried once, only after the saved
        // manual address failed, and never when auth is blocked.
        const QString manual = QStringLiteral("192.0.2.10");
        const QString reported = QStringLiteral("192.0.2.20");
        CHECK(peripheralFallbackHost(manual, manual, reported, false, false) == reported);
        CHECK(peripheralFallbackHost(QStringLiteral(" 192.0.2.10 "), manual, reported, false, false) == reported);
        CHECK(peripheralFallbackHost(manual, manual, reported, true, false).isEmpty());
        CHECK(peripheralFallbackHost(manual, manual, reported, false, true).isEmpty());
        CHECK(peripheralFallbackHost(manual, manual, manual, false, false).isEmpty());
        CHECK(peripheralFallbackHost(manual, manual, QString(), false, false).isEmpty());
        CHECK(peripheralFallbackHost(manual, QString(), reported, false, false).isEmpty());
        // A failed attempt at the radio-reported address is not retried.
        CHECK(peripheralFallbackHost(reported, manual, reported, false, false).isEmpty());
    }
    int strikes = 0;
    CHECK(recordPeripheralAuthFailure(strikes) < 3);
    CHECK(recordPeripheralAuthFailure(strikes) < 3);
    CHECK(recordPeripheralAuthFailure(strikes) == 3);
    CHECK(recordPeripheralAuthFailure(strikes) == 3); // saturates instead of overflowing
    strikes = 0; // a successful handshake or newly entered code resets the budget
    CHECK(recordPeripheralAuthFailure(strikes) < 3);
    {
        TgxlConnection connection;
        connection.setAuthCode("test-code");
        QStringList signalOrder;
        QObject::connect(&connection, &TgxlConnection::authCodeAccepted, &app,
                         [&signalOrder](const QString&) { signalOrder.append("accepted"); });
        QObject::connect(&connection, &TgxlConnection::connected, &app,
                         [&signalOrder]() { signalOrder.append("connected"); });
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy accepted(&connection, &TgxlConnection::authCodeAccepted);
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        QSignalSpy disconnected(&connection, &TgxlConnection::disconnected);
        feed(connection, "V1.2.17 AUTH");
        CHECK(connection.version() == "1.2.17");
        CHECK(connected.isEmpty());
        feed(connection, "R0|0|auth OK"); // observed on firmware 1.2.17
        CHECK(connected.size() == 1);
        CHECK(accepted.size() == 1);
        CHECK(signalOrder == QStringList({"accepted", "connected"}));
        CHECK(failed.isEmpty());
        connection.disconnect();
        CHECK(disconnected.size() == 1);
    }
    {
        TgxlConnection connection;
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        QSignalSpy disconnected(&connection, &TgxlConnection::disconnected);
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        feed(connection, "V1.2.17 AUTH");
        CHECK(connected.isEmpty());
        CHECK(required.size() == 1);
        CHECK(failed.isEmpty());
        // A code entered for another host must not answer this old challenge.
        connection.setAuthCode("new-host-code");
        CHECK(failed.isEmpty());
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(), QString());
        CHECK(failed.size() == 1);
        CHECK(disconnected.isEmpty()); // a failed handshake was never online
    }
    {
        TgxlConnection connection;
        QSignalSpy discarded(&connection, &TgxlConnection::enteredAuthCodeDiscarded);
        connection.setAuthCode(QStringLiteral("interrupted-new-code"));
        connection.disconnect();
        CHECK(discarded.size() == 1);
        connection.disconnect();
        CHECK(discarded.size() == 1);
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        feed(connection, "V1.2.17 AUTH");
        CHECK(required.size() == 1); // interrupted code cannot answer next challenge
    }
    {
        TgxlConnection connection;
        QSignalSpy discarded(&connection, &TgxlConnection::enteredAuthCodeDiscarded);
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                                        Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9010)));
        connection.setAuthCode(QStringLiteral("first-host-code"));
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                                        Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9010)));
        CHECK(discarded.size() == 1);
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        feed(connection, "V1.2.17 AUTH");
        CHECK(required.size() == 1); // typed code cannot follow a host switch
    }
    {
        TgxlConnection connection;
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        feed(connection, "V1.2.17 AUTH");
        connection.setAuthCode(QString()); // clearing a pending challenge ends it
        CHECK(failed.size() == 1);
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(),
                                         QStringLiteral("stale-code"));
        CHECK(failed.size() == 1);
    }
    {
        TgxlConnection connection;
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        CHECK(QMetaObject::invokeMethod(&connection, "processBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
    }
    {
        TgxlConnection connection;
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        feed(connection, "V1.2.17");
        CHECK(connection.isConnected());
        CHECK(QMetaObject::invokeMethod(&connection, "processBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(!connection.isAuthBlocked()); // a bad status line may reconnect
    }
    {
        TgxlConnection connection;
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        QSignalSpy status(&connection, &TgxlConnection::statusUpdated);
        feed(connection, "V1.2.17 AUTH");
        CHECK(required.size() == 1);
        feed(connection, "S0|fwd=100");
        CHECK(status.isEmpty()); // no unauthenticated status enters the model
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong() + 1,
                                         QStringLiteral("test-code"));
        feed(connection, "R0|0|auth OK");
        CHECK(connected.isEmpty()); // stale keychain reply was ignored
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(),
                                         QStringLiteral("test-code"));
        feed(connection, "R1|0|auth OK"); // vendor-documented success form
        CHECK(connected.size() == 1);
        connection.disconnect();
    }
    {
        TgxlConnection connection;
        connection.setAuthCode("bad\nC2|status");
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        feed(connection, "V1.2.17 AUTH");
        CHECK(connected.isEmpty());
        CHECK(failed.size() == 1);
    }
    {
        TgxlConnection connection;
        connection.setAuthCode("test-code");
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        QSignalSpy accepted(&connection, &TgxlConnection::authCodeAccepted);
        feed(connection, "V1.2.17 AUTH");
        feed(connection, "R1|0|Unauthorized"); // documented in the TGXL API
        CHECK(connected.isEmpty());
        CHECK(accepted.isEmpty());
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
        QSignalSpy cleared(&connection, &TgxlConnection::authBlockCleared);
        connection.setAuthCode(QString());
        CHECK(cleared.size() == 1);
        CHECK(!connection.isAuthBlocked());
    }
    {
        TgxlConnection connection;
        connection.setAuthCode("test-code");
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        feed(connection, "V1.2.17 AUTH");
        CHECK(QMetaObject::invokeMethod(&connection, "onDisconnected",
                                        Qt::DirectConnection));
        CHECK(failed.size() == 1);
        CHECK(!connection.isAuthBlocked()); // a single WAN drop remains retryable
    }
    for (const char* failureMethod : {"onDisconnected", "onAuthTimeout"}) {
        TgxlConnection connection;
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy failed(&connection, &TgxlConnection::connectionFailed);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            feed(connection, "V1.2.17 AUTH");
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
        connection.setAuthCode(QStringLiteral("operator-new-code"));
        CHECK(!connection.isAuthBlocked());
        CHECK(failed.size() == 3);
    }
    for (const char* failureMethod : {"onDisconnected", "onAuthTimeout"}) {
        TgxlConnection connection;
        QSignalSpy required(&connection, &TgxlConnection::authCodeRequired);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            feed(connection, "V1.2.17 AUTH");
            CHECK(required.size() == strike);
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                             QStringLiteral("saved-code"));
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
    }
    {
        TgxlConnection connection;
        connection.setAuthCode("unchecked-lan-code");
        QSignalSpy connected(&connection, &TgxlConnection::connected);
        QSignalSpy accepted(&connection, &TgxlConnection::authCodeAccepted);
        feed(connection, "V1.2.17");
        CHECK(connected.size() == 1);
        CHECK(accepted.isEmpty()); // no AUTH reply, so do not overwrite keychain
        connection.disconnect();
    }
    {
        PgxlConnection connection;
        connection.setAuthCode("test-code");
        QSignalSpy connected(&connection, &PgxlConnection::connected);
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        QSignalSpy accepted(&connection, &PgxlConnection::authCodeAccepted);
        feed(connection, "V3.9.1 AUTH");
        CHECK(connection.version() == "3.9.1");
        CHECK(connected.isEmpty());
        CHECK(accepted.isEmpty());
        feed(connection, "R1|50000016|Incorrect parameter");
        CHECK(connected.isEmpty());
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
        QSignalSpy cleared(&connection, &PgxlConnection::authBlockCleared);
        connection.setAuthCode(QString());
        CHECK(cleared.size() == 1);
        CHECK(!connection.isAuthBlocked());
    }
    {
        PgxlConnection connection;
        connection.setAuthCode("test-code");
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        QSignalSpy disconnected(&connection, &PgxlConnection::disconnected);
        feed(connection, "V3.9.1 AUTH");
        CHECK(QMetaObject::invokeMethod(&connection, "onDisconnected",
                                        Qt::DirectConnection));
        CHECK(failed.size() == 1);
        CHECK(disconnected.isEmpty());
        CHECK(!connection.isAuthBlocked());
    }
    for (const char* failureMethod : {"onDisconnected", "onAuthTimeout"}) {
        PgxlConnection connection;
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            feed(connection, "V3.9.1 AUTH");
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
        connection.setAuthCode(QStringLiteral("operator-new-code"));
        CHECK(!connection.isAuthBlocked());
        CHECK(failed.size() == 3);
    }
    for (const char* failureMethod : {"onDisconnected", "onAuthTimeout"}) {
        PgxlConnection connection;
        QSignalSpy required(&connection, &PgxlConnection::authCodeRequired);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            feed(connection, "V3.9.1 AUTH");
            CHECK(required.size() == strike);
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                             QStringLiteral("saved-code"));
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
    }
    {
        PgxlConnection connection;
        connection.setAuthCode("test-code");
        QSignalSpy connected(&connection, &PgxlConnection::connected);
        feed(connection, "V3.9.1 AUTH");
        feed(connection, "R1|0|Unexpected reply");
        CHECK(connected.isEmpty());
        CHECK(connection.isAuthBlocked());
    }
    {
        PgxlConnection connection;
        connection.setAuthCode("test-code");
        QStringList signalOrder;
        QObject::connect(&connection, &PgxlConnection::authCodeAccepted, &app,
                         [&signalOrder](const QString&) { signalOrder.append("accepted"); });
        QObject::connect(&connection, &PgxlConnection::connected, &app,
                         [&signalOrder]() { signalOrder.append("connected"); });
        QSignalSpy connected(&connection, &PgxlConnection::connected);
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        feed(connection, "V3.9.1 AUTH");
        CHECK(connected.isEmpty());
        feed(connection, "R1|0|Authorized"); // observed on firmware 3.9.1
        CHECK(connected.size() == 1);
        CHECK(signalOrder == QStringList({"accepted", "connected"}));
        CHECK(failed.isEmpty());
        connection.disconnect();
    }
    {
        PgxlConnection connection;
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        CHECK(QMetaObject::invokeMethod(&connection, "processBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
    }
    {
        PgxlConnection connection;
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        feed(connection, "V3.9.1");
        CHECK(connection.isConnected());
        CHECK(QMetaObject::invokeMethod(&connection, "processBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(!connection.isAuthBlocked());
    }
    {
        PgxlConnection connection;
        connection.setAuthCode(QStringLiteral("interrupted-new-code"));
        connection.disconnect();
        QSignalSpy required(&connection, &PgxlConnection::authCodeRequired);
        feed(connection, "V3.9.1 AUTH");
        CHECK(required.size() == 1);
    }
    {
        PgxlConnection connection;
        QSignalSpy required(&connection, &PgxlConnection::authCodeRequired);
        QSignalSpy failed(&connection, &PgxlConnection::connectionFailed);
        feed(connection, "V3.9.1 AUTH");
        CHECK(required.size() == 1);
        CHECK(failed.isEmpty());
        connection.setAuthCode("new-host-code");
        CHECK(failed.isEmpty());
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(), QString());
        CHECK(failed.size() == 1);
    }
    {
        PgxlConnection connection;
        QSignalSpy discarded(&connection, &PgxlConnection::enteredAuthCodeDiscarded);
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                                        Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9008)));
        connection.setAuthCode(QStringLiteral("first-host-code"));
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttemptAt", Qt::DirectConnection,
                                        Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9008)));
        CHECK(discarded.size() == 1);
        QSignalSpy required(&connection, &PgxlConnection::authCodeRequired);
        feed(connection, "V3.9.1 AUTH");
        CHECK(required.size() == 1); // typed code cannot follow a host switch
    }
    {
        PgxlConnection connection;
        QSignalSpy discarded(&connection, &PgxlConnection::enteredAuthCodeDiscarded);
        connection.setAuthCode(QStringLiteral("interrupted-code"));
        connection.disconnect();
        CHECK(discarded.size() == 1);
    }
    {
        PgxlConnection connection;
        connection.setAuthCode("unchecked-lan-code");
        QSignalSpy connected(&connection, &PgxlConnection::connected);
        QSignalSpy accepted(&connection, &PgxlConnection::authCodeAccepted);
        feed(connection, "V3.9.1");
        CHECK(connected.size() == 1);
        CHECK(accepted.isEmpty());
        connection.disconnect();
    }
    {
        AntennaGeniusModel connection;
        prepareAg(connection);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
    }
    {
        AntennaGeniusModel connection;
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG\r\n"))));
        CHECK(connection.isConnected());
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(failed.size() == 1);
        CHECK(!connection.isAuthBlocked());
        connection.disconnectFromDevice();
    }
    {
        QByteArray command;
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection,
            [&command](const QByteArray& bytes) { command = bytes; });
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QStringList signalOrder;
        QObject::connect(&connection, &AntennaGeniusModel::authCodeAccepted, &app,
                         [&signalOrder](const QString&) { signalOrder.append(QStringLiteral("accepted")); });
        QObject::connect(&connection, &AntennaGeniusModel::connected, &app,
                         [&signalOrder]() { signalOrder.append(QStringLiteral("connected")); });
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(command == QByteArray("C1|auth code=sample\r\n"));
        CHECK(signalOrder.isEmpty());
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0|\r\n"))));
        CHECK(signalOrder == QStringList({QStringLiteral("accepted"), QStringLiteral("connected")}));
        connection.disconnectFromDevice();
    }
    {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy connected(&connection, &AntennaGeniusModel::connected);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0|OK\r\n"))));
        CHECK(connected.size() == 1); // explicitly allowlisted success body
        connection.disconnectFromDevice();
    }
    {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy connected(&connection, &AntennaGeniusModel::connected);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0\r\n"))));
        CHECK(connected.size() == 1); // optional message and delimiter omitted
        connection.disconnectFromDevice();
    }
    {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy accepted(&connection, &AntennaGeniusModel::authCodeAccepted);
        QSignalSpy connected(&connection, &AntennaGeniusModel::connected);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|FF|Denied\r\n"))));
        CHECK(accepted.isEmpty());
        CHECK(connected.isEmpty());
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
    }
    for (const QByteArray& reply : {QByteArray("R1|0|Unauthorized\r\n"),
                                   QByteArray("R1|0|Denied\r\n"),
                                   QByteArray("R1|0|unknown\r\n"),
                                   QByteArray("R1|0|OK|Unauthorized\r\n"),
                                   QByteArray("R2|0|OK\r\n")}) {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("rejected-code"));
        QSignalSpy accepted(&connection, &AntennaGeniusModel::authCodeAccepted);
        QSignalSpy connected(&connection, &AntennaGeniusModel::connected);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, reply)));
        CHECK(accepted.isEmpty()); // no rejected credential may reach the vault
        CHECK(connected.isEmpty());
        CHECK(!connection.isConnected());
        CHECK(failed.size() == 1);
        CHECK(connection.isAuthBlocked());
    }
    {
        AntennaGeniusModel connection;
        prepareAg(connection, {}, 0);
        CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray(64 * 1024 + 1, 'x'))));
        CHECK(!connection.isAuthBlocked());
        CHECK(!connection.isAuthBlockedFor({}, 9007));
        CHECK(!connection.isAuthBlockedFor(QStringLiteral("192.0.2.10"), 0));
    }
    {
        // Discovery churn cannot evict an old block or grow retry history forever.
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        for (int target = 0; target < 256; ++target) {
            prepareAg(connection, QStringLiteral("target-%1.example").arg(target));
            CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        }
        CHECK(connection.isAuthBlockedFor(QStringLiteral("unknown.example"), 9007));
        CHECK(!connection.isAuthBlockedFor(QStringLiteral("target-0.example"), 9007));
        prepareAg(connection, QStringLiteral("target-0.example"));
        CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        CHECK(connection.isAuthBlocked());
        AgDeviceInfo reset;
        reset.host = QStringLiteral("target-1.example");
        reset.port = 9007;
        connection.resetAuthBudgetFor(reset);
        CHECK(!connection.isAuthBlockedFor(QStringLiteral("unknown.example"), 9007));
        CHECK(connection.isAuthBlockedFor(QStringLiteral("target-0.example"), 9007));
    }
    {
        // AG and ShackSwitch share a model, but their failure budgets and
        // automatic-connect blocks belong to their individual targets.
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        const QString agHost = QStringLiteral("192.0.2.10");
        const QString switchHost = QStringLiteral("192.0.2.11");
        prepareAg(connection, agHost);
        connection.setAuthCode(QStringLiteral("rejected-code"));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|FF|Denied\r\n"))));
        CHECK(connection.isAuthBlockedFor(agHost, 9007));
        CHECK(!connection.isAuthBlockedFor(switchHost, 9007));
        AgDeviceInfo agInfo;
        agInfo.ip = QHostAddress(agHost);
        agInfo.port = 9007;
        AgDeviceInfo switchInfo;
        switchInfo.ip = QHostAddress(switchHost);
        switchInfo.port = 9007;
        switchInfo.name = QStringLiteral("ShackSwitch");
        CHECK(!connection.isAuthBlockedFor(switchInfo));

        prepareAg(connection, switchHost);
        CHECK(!connection.isAuthBlocked());
        connection.setAuthCode(QStringLiteral("switch-code"));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        CHECK(!connection.isAuthBlockedFor(switchHost, 9007)); // first timeout
        CHECK(connection.isAuthBlockedFor(agHost, 9007));
        connection.resetAuthBudgetFor(switchInfo);
        CHECK(connection.isAuthBlockedFor(agHost, 9007));
        // A manual retry starts this target's timeout budget from zero.
        prepareAg(connection, switchHost);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        CHECK(!connection.isAuthBlockedFor(switchHost, 9007));

        prepareAg(connection, agHost);
        CHECK(connection.isAuthBlocked()); // switching back cannot bypass the AG block
        prepareAg(connection, switchHost);
        connection.setAuthCode(QStringLiteral("new-switch-code"));
        CHECK(connection.isAuthBlockedFor(agHost, 9007)); // reset affects only ShackSwitch
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0|\r\n"))));
        CHECK(connection.isConnected());
        CHECK(connection.isAuthBlockedFor(agHost, 9007)); // SS success does not unblock AG
        connection.resetAuthBudgetFor(agInfo);
        CHECK(!connection.isAuthBlockedFor(agHost, 9007));
    }
    {
        QByteArray command;
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection,
            [&command](const QByteArray& bytes) { command = bytes; });
        prepareAg(connection);
        QSignalSpy discarded(&connection, &AntennaGeniusModel::enteredAuthCodeDiscarded);
        connection.setAuthCode(QStringLiteral("typed-for-first-host"));
        prepareAg(connection, QStringLiteral("192.0.2.11"));
        CHECK(discarded.size() == 1);
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(required.size() == 1);
        CHECK(command.isEmpty()); // a new discovered peer never gets the typed code
    }
    {
        QByteArray command;
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection,
            [&command](const QByteArray& bytes) { command = bytes; });
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("typed-for-first-host"));
        prepareAg(connection); // reconnect to the same target preserves the code
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(command == QByteArray("C1|auth code=typed-for-first-host\r\n"));
    }
    {
        AntennaGeniusModel connection;
        QTimer* retry = connection.findChild<QTimer*>(QStringLiteral("agReconnectTimer"));
        CHECK(retry != nullptr);
        if (retry) {
            retry->start();
            CHECK(retry->isActive());
            prepareAg(connection, QStringLiteral("192.0.2.11"));
            CHECK(!retry->isActive()); // an old retry cannot abort a new handshake
        }
    }
    for (const char* failureMethod : {"onTcpDisconnected", "onAuthTimeout"}) {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                            Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
        connection.setAuthCode(QStringLiteral("operator-new-code"));
        CHECK(!connection.isAuthBlocked());
        CHECK(failed.size() == 3);
    }
    {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                            Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                             QStringLiteral("saved-code"));
            CHECK(QMetaObject::invokeMethod(&connection, "onAuthTimeout", Qt::DirectConnection));
        }
        CHECK(connection.isAuthBlocked());
        CHECK(failed.size() == 3);
        // A deliberate Connect may start while blocked. A close during its
        // challenge must still be reported to the operator.
        CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                         QStringLiteral("saved-code"));
        CHECK(QMetaObject::invokeMethod(&connection, "onTcpDisconnected", Qt::DirectConnection));
        CHECK(failed.size() == 4);
        CHECK(connection.isAuthBlocked());
    }
    for (const char* failureMethod : {"onTcpDisconnected", "onAuthTimeout"}) {
        AntennaGeniusModel connection;
        AntennaGeniusModelTestAccess::setAuthWriter(connection);
        prepareAg(connection);
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        for (int strike = 1; strike <= 3; ++strike) {
            CHECK(QMetaObject::invokeMethod(&connection, "beginAttempt", Qt::DirectConnection));
            CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                            Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
            CHECK(required.size() == strike);
            connection.setAuthCodeForAttempt(required.last().at(0).toULongLong(),
                                             QStringLiteral("saved-code"));
            CHECK(QMetaObject::invokeMethod(&connection, failureMethod, Qt::DirectConnection));
            CHECK(connection.isAuthBlocked() == (strike == 3));
        }
    }
    {
        AntennaGeniusModel connection;
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("interrupted-new-code"));
        connection.disconnectFromDevice();
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(required.size() == 1);
    }
    {
        AntennaGeniusModel connection;
        prepareAg(connection);
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(required.size() == 1);
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(), QString(), true);
        CHECK(failed.size() == 1);
        CHECK(failed.at(0).at(0).toString() == QStringLiteral("Stored authorization code unavailable"));
        CHECK(!connection.isAuthBlocked()); // a keychain outage must not block reconnect
    }
    {
        // The no-keychain build keeps accepted codes only in the process
        // session vault, and clearing one must report success.
        CHECK(PeripheralAuthStore::validCode(QStringLiteral("test-code")));
        CHECK(!PeripheralAuthStore::validCode(QStringLiteral("bad\nC2|status")));
        CHECK(!PeripheralAuthStore::validCode(QStringLiteral("two words")));
        CHECK(PeripheralAuthStore::validCode(QStringLiteral("code=value")));
        bool savedPersistently = true;
        const QString boundEndpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.10"), 9010);
        const QString wrongEndpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.11"), QStringLiteral("192.0.2.11"), 9010);
        CHECK(boundEndpoint != wrongEndpoint);
        // Nothing binds before a socket has a real peer, name or not.
        CHECK(PeripheralAuthStore::endpoint(QStringLiteral("unresolved.example"),
                                            QStringLiteral("unresolved.example"), 9010).isEmpty());
        CHECK(PeripheralAuthStore::endpoint(QStringLiteral("home.example.net"), QString(), 9010).isEmpty());
        CHECK(PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"),
                                            QStringLiteral("192.0.2.10"), 0).isEmpty());
        // A name binds to the name: the residential IP behind it changing does
        // not orphan the saved code (#2313's DDNS operators), and the case of
        // the name does not matter.
        const QString byName = PeripheralAuthStore::endpoint(
            QStringLiteral("Home.Example.Net"), QStringLiteral("198.51.100.7"), 9010);
        CHECK(!byName.isEmpty());
        CHECK(byName == PeripheralAuthStore::endpoint(
            QStringLiteral(" home.example.net "), QStringLiteral("203.0.113.9"), 9010));
        // A different name, a different port, or a literal IP is a different
        // identity, so a code saved for one is never offered to another.
        CHECK(byName != PeripheralAuthStore::endpoint(
            QStringLiteral("other.example.net"), QStringLiteral("198.51.100.7"), 9010));
        CHECK(byName != PeripheralAuthStore::endpoint(
            QStringLiteral("home.example.net"), QStringLiteral("198.51.100.7"), 9008));
        CHECK(byName != PeripheralAuthStore::endpoint(
            QStringLiteral("198.51.100.7"), QStringLiteral("198.51.100.7"), 9010));
        // A literal IP still binds to the connected address, exactly as before.
        CHECK(PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"),
                                            QStringLiteral("192.0.2.10"), 9010)
              == QStringLiteral("192.0.2.10:9010"));
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl,
            boundEndpoint, QStringLiteral("test-code"), &app, [&](bool ok) {
                savedPersistently = ok;
                loop.quit();
            });
        loop.exec();
        CHECK(!savedPersistently);
        PeripheralAuthStore::LoadResult loaded;
        PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, boundEndpoint, &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(loaded.code == QStringLiteral("test-code"));
        CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Found);
        loaded = {};
        PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, wrongEndpoint, &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(loaded.code.isEmpty());
        CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
        bool cleared = false;
        PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl,
            QString(), QString(), &app, [&](bool ok) { cleared = ok; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(cleared);
    }
    {
        // Cold session-vault loads preserve their endpoint binding. An old
        // unbound plaintext value cannot be sent to a discovered peer.
        const QString endpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.20"), QStringLiteral("192.0.2.20"), 9008);
        AppSettings::instance().setSessionCredential(QStringLiteral("pgxl_auth_code"),
            QStringLiteral("{\"version\":1,\"endpoint\":\"%1\",\"code\":\"restored\"}")
                .arg(endpoint));
        PeripheralAuthStore::LoadResult loaded;
        QEventLoop loop;
        PeripheralAuthStore::load(PeripheralAuthStore::Device::Pgxl, endpoint, &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(loaded.code == QStringLiteral("restored"));
        AppSettings::instance().setSessionCredential(QStringLiteral("antenna_genius_auth_code"),
                                                     QStringLiteral("old-unbound-code"));
        PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius,
            PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.21"), QStringLiteral("192.0.2.21"), 9007), &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(loaded.code.isEmpty());
        CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    }
    return failures == 0 ? 0 : 1;
}
