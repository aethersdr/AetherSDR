// Feed captured success and documented failure frames into the state machines.
// No socket or synthetic firmware peer is involved.
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "models/AntennaGeniusModel.h"
#include "gui/PeripheralAuthStore.h"
#include "core/PeripheralAuthCode.h"
#include "core/AppSettings.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QSignalSpy>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

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
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::Tgxl, QStringLiteral("sample"))
          == QByteArray("C1|auth sample\n")); // captured TGXL form
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::Pgxl, QStringLiteral("sample"))
          == QByteArray("C1|auth code=sample\n")); // captured PGXL form
    CHECK(peripheralAuthCommand(PeripheralAuthProtocol::AntennaGenius, QStringLiteral("sample"))
          == QByteArray("C1|auth code=sample\r")); // vendor-documented form
    int strikes = 0;
    CHECK(!peripheralAuthFailureBlocks(strikes));
    CHECK(!peripheralAuthFailureBlocks(strikes));
    CHECK(peripheralAuthFailureBlocks(strikes));
    CHECK(peripheralAuthFailureBlocks(strikes)); // saturates instead of overflowing
    strikes = 0; // a successful handshake or newly entered code resets the budget
    CHECK(!peripheralAuthFailureBlocks(strikes));
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
        AntennaGeniusModel connection(nullptr,
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
        CHECK(command == QByteArray("C1|auth code=sample\r"));
        CHECK(signalOrder.isEmpty());
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0|\r\n"))));
        CHECK(signalOrder == QStringList({QStringLiteral("accepted"), QStringLiteral("connected")}));
        connection.disconnectFromDevice();
    }
    {
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("sample"));
        QSignalSpy connected(&connection, &AntennaGeniusModel::connected);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("R1|0|OK\r\n"))));
        CHECK(connected.size() == 1); // optional vendor response message
        connection.disconnectFromDevice();
    }
    {
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
    {
        // AG and ShackSwitch share a model, but their failure budgets and
        // automatic-connect blocks belong to their individual targets.
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
        AntennaGeniusModel connection(nullptr,
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
        AntennaGeniusModel connection(nullptr,
            [&command](const QByteArray& bytes) { command = bytes; });
        prepareAg(connection);
        connection.setAuthCode(QStringLiteral("typed-for-first-host"));
        prepareAg(connection); // reconnect to the same target preserves the code
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(command == QByteArray("C1|auth code=typed-for-first-host\r"));
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
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
        AntennaGeniusModel connection(nullptr, [](const QByteArray&) {});
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
        QSignalSpy required(&connection, &AntennaGeniusModel::authCodeRequired);
        QSignalSpy failed(&connection, &AntennaGeniusModel::connectionError);
        CHECK(QMetaObject::invokeMethod(&connection, "processTcpBytes", Qt::DirectConnection,
                                        Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n"))));
        CHECK(required.size() == 1);
        connection.setAuthCodeForAttempt(required.at(0).at(0).toULongLong(), QString(), true);
        CHECK(failed.size() == 1);
        CHECK(failed.at(0).at(0).toString() == QStringLiteral("Stored authorization code unavailable"));
        CHECK(connection.isAuthBlocked());
    }
    {
        // The no-keychain build keeps accepted codes only in the process
        // session vault, and clearing one must report success.
        CHECK(PeripheralAuthStore::validCode(QStringLiteral("test-code")));
        CHECK(!PeripheralAuthStore::validCode(QStringLiteral("bad\nC2|status")));
        CHECK(!PeripheralAuthStore::validCode(QStringLiteral("two words")));
        CHECK(PeripheralAuthStore::validCode(QStringLiteral("code=value")));
        bool savedPersistently = true;
        const QString boundEndpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"), 9010);
        const QString wrongEndpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.11"), 9010);
        CHECK(boundEndpoint != wrongEndpoint);
        CHECK(PeripheralAuthStore::endpoint(QStringLiteral("unresolved.example"), 9010).isEmpty());
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
        const QString endpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.20"), 9008);
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
            PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.21"), 9007), &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; loop.quit(); });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(loaded.code.isEmpty());
        CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    }
    return failures == 0 ? 0 : 1;
}
