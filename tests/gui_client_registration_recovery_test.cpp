#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return predicate();
}

int commandCount(const QStringList& commands, const QString& prefix)
{
    int count = 0;
    for (const QString& command : commands) {
        if (command.startsWith(prefix)) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-gui-registration-recovery-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().initializeGuiClientIdentity();

    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost, 0), "fake radio listens on loopback");
    if (!server.isListening()) {
        return 1;
    }

    bool rejectGuiRegistration = true;
    bool guiRegistrationAccepted = false;
    int connectionCount = 0;
    QStringList commands;
    QHash<QTcpSocket*, QByteArray> buffers;

    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            ++connectionCount;
            buffers.insert(socket, {});

            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray& buffer = buffers[socket];
                buffer.append(socket->readAll());
                int newline = -1;
                while ((newline = buffer.indexOf('\n')) >= 0) {
                    const QString line =
                        QString::fromUtf8(buffer.left(newline)).trimmed();
                    buffer.remove(0, newline + 1);
                    if (!line.startsWith(QLatin1Char('C'))) {
                        continue;
                    }

                    const int pipe = line.indexOf(QLatin1Char('|'));
                    if (pipe <= 1) {
                        continue;
                    }
                    const quint32 sequence = line.mid(1, pipe - 1).toUInt();
                    const QString command = line.mid(pipe + 1);
                    commands.append(command);

                    if (command.startsWith(QStringLiteral("client gui "))
                        && rejectGuiRegistration) {
                        socket->write(
                            "MF3000001|The maximum number of connected clients "
                            "has been reached\n");
                        socket->write(
                            QStringLiteral(
                                "R%1|F3000001|The maximum number of connected clients "
                                "has been reached\n")
                                .arg(sequence)
                                .toUtf8());
                    } else if (command.startsWith(QStringLiteral("client gui "))) {
                        guiRegistrationAccepted = true;
                        socket->write(
                            QStringLiteral("R%1|0|\n").arg(sequence).toUtf8());
                    } else if (guiRegistrationAccepted) {
                        // The successful client-gui callback sends the complete
                        // post-registration batch synchronously. Withhold later
                        // replies so the test can assert that batch and tear down
                        // without racing unrelated info/slice callbacks.
                    } else {
                        socket->write(
                            QStringLiteral("R%1|0|\n").arg(sequence).toUtf8());
                    }
                    socket->flush();
                }
            });
            socket->write("V1.4.0.0\nH12345678\nS0|radio mf_enable=1\n");
            socket->flush();
        }
    });

    RadioModel model;
    QSignalSpy connectionSpy(&model, &RadioModel::connectionStateChanged);
    QSignalSpy registrationFailureSpy(
        &model, &RadioModel::guiClientRegistrationFailed);
    QSignalSpy errorSpy(&model, &RadioModel::connectionError);

    RadioInfo info;
    info.family = QStringLiteral("flex");
    info.serial = QStringLiteral("TEST-REGISTRATION-RECOVERY");
    info.name = QStringLiteral("Fake Flex");
    info.model = QStringLiteral("FLEX-6700");
    info.version = QStringLiteral("4.2.18");
    info.address = QHostAddress::LocalHost;
    info.port = server.serverPort();

    model.connectToRadio(info);

    check(waitFor([&] {
              return registrationFailureSpy.count() == 1 && !model.isConnected();
          }, 5000),
          "rejected GUI registration tears down the connected TCP session");
    check(errorSpy.count() == 1,
          "rejected GUI registration emits a user-facing connection error");
    check(connectionSpy.count() >= 2
              && connectionSpy.first().at(0).toBool()
              && !connectionSpy.last().at(0).toBool(),
          "connection state returns from connected to disconnected");

    const QString failureMessage =
        registrationFailureSpy.isEmpty()
            ? QString()
            : registrationFailureSpy.first().at(0).toString();
    check(failureMessage.contains(
              QStringLiteral("The maximum number of connected clients has been reached")),
          "the radio's fatal detail is preserved in the terminal error");
    check(failureMessage.contains(QStringLiteral("0xf3000001")),
          "the terminal error includes the rejected command result code");
    check(failureMessage.contains(QStringLiteral("choose Connect")),
          "the terminal error explains the explicit recovery action");

    check(commandCount(commands, QStringLiteral("client gui ")) == 1,
          "the failed attempt sends one GUI registration command");
    check(commandCount(commands, QStringLiteral("client station ")) == 0,
          "the failed attempt sends no post-registration station command");
    check(commandCount(commands, QStringLiteral("sub slice all")) == 0,
          "the failed attempt sends no post-registration subscriptions");
    check(commandCount(commands, QStringLiteral("client udpport ")) == 0,
          "the failed attempt performs no UDP/data-plane setup");

    waitFor([&] { return false; }, 3300);
    check(connectionCount == 1,
          "rejected registration does not enter the automatic reconnect loop");
    check(connectionCount == 1
              && commandCount(commands, QStringLiteral("client gui ")) == 1,
          "no blind TCP or GUI-registration retry occurs while the radio is full");

    rejectGuiRegistration = false;
    guiRegistrationAccepted = false;
    model.connectToRadio(info);
    check(waitFor([&] {
              return connectionCount == 2
                  && commandCount(commands, QStringLiteral("client gui ")) == 2
                  && commandCount(commands, QStringLiteral("sub slice all")) >= 1;
          }, 5000),
          "a later explicit connect opens fresh TCP and resumes the successful handshake");
    check(model.isConnected(), "successful explicit reconnect remains connected");
    check(commandCount(commands, QStringLiteral("client station ")) >= 1,
          "successful registration reaches post-registration client setup");

    model.disconnectFromRadio();
    check(waitFor([&] { return !model.isConnected(); }, 3000),
          "test cleanup disconnects the successful session");

    return failures == 0 ? 0 : 1;
}
