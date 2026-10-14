// Opt-in localhost transport diagnostic. The subject is Qt socket reuse, not
// radio firmware; the peer sends only the parser's documented V/H greeting.
#include "core/backends/flex/RadioConnection.h"

#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <memory>

using namespace AetherSDR;

class RadioConnectionRetryTest final : public QObject {
    Q_OBJECT
    QNetworkProxy m_previousProxy;
private slots:
    void initTestCase()
    {
        m_previousProxy = QNetworkProxy::applicationProxy();
        QNetworkProxy::setApplicationProxy(QNetworkProxy(
            QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 1));
    }

    void cleanupTestCase()
    {
        QNetworkProxy::setApplicationProxy(m_previousProxy);
    }

    void unavailableSource_data()
    {
        QTest::addColumn<int>("bindMode");
        QTest::newRow("auto-fallback") << int(RadioBindMode::Auto);
        QTest::newRow("explicit-no-fallback") << int(RadioBindMode::Explicit);
    }

    void unavailableSource()
    {
        QFETCH(int, bindMode);
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
        RadioConnection connection;
        connection.init();
        QSignalSpy errors(&connection, &RadioConnection::errorOccurred);
        const QHostAddress unavailable(QStringLiteral("192.0.2.1"));
        connection.connectToHost(QHostAddress(QHostAddress::LocalHost), server.serverPort(),
            RadioBindMode(bindMode), unavailable, unavailable);
        QVERIFY(!errors.isEmpty());
        if (RadioBindMode(bindMode) == RadioBindMode::Explicit) {
            QCOMPARE(connection.state(), ConnectionState::Error);
            QVERIFY(!server.hasPendingConnections());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
            std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
            QVERIFY(peer);
            QSignalSpy connected(&connection, &RadioConnection::connected);
            peer->write("V1.4.0.0\nH00000001\n");
            peer->flush();
            QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 1000);
            peer->abort();
            QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionState::Disconnected, 1000);
        }
    }

    void refusedThenReady_data()
    {
        QTest::addColumn<int>("bindMode");
        QTest::addColumn<bool>("bindSource");
        QTest::newRow("auto-source") << int(RadioBindMode::Auto) << true;
        QTest::newRow("explicit-source") << int(RadioBindMode::Explicit) << true;
        QTest::newRow("os-routing") << int(RadioBindMode::Auto) << false;
    }

    void refusedThenReady()
    {
        QFETCH(int, bindMode);
        QFETCH(bool, bindSource);
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
        const quint16 port = server.serverPort();
        const QHostAddress source = bindSource ? QHostAddress(QHostAddress::LocalHost) : QHostAddress();
        RadioConnection connection;
        connection.init();
        QSignalSpy connected(&connection, &RadioConnection::connected);
        QSignalSpy errors(&connection, &RadioConnection::errorOccurred);
        const auto attempt = [&] {
            connection.connectToHost(QHostAddress(QHostAddress::LocalHost), port,
                RadioBindMode(bindMode), source, source);
        };
        const auto accept = [&] {
            std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection());
            if (peer) {
                peer->setParent(nullptr);
            }
            return peer;
        };
        attempt();
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
        std::unique_ptr<QTcpSocket> peer = accept();
        QVERIFY(peer);
        peer->write("V1.4.0.0\nH12345678\n");
        peer->flush();
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(), 1, 1000);

        for (int cycle = 0; cycle < 2; ++cycle) {
            server.close();
            peer->abort();
            peer.reset();
            QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionState::Disconnected, 1000);
            QCOMPARE(connection.clientHandle(), 0u);
            for (int retry = 0; retry < 3; ++retry) {
                const qsizetype previousErrors = errors.size();
                attempt();
                QTRY_VERIFY_WITH_TIMEOUT(errors.size() > previousErrors, 1000);
                QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionState::Disconnected, 1000);
                QCOMPARE(errors.size(), previousErrors + 1);
            }
            QVERIFY2(server.listen(QHostAddress::LocalHost, port), qPrintable(server.errorString()));
            // Independent new socket proves the destination is reachable before
            // retrying the same long-lived production RadioConnection.
            QTcpSocket probe;
            probe.setProxy(QNetworkProxy::NoProxy);
            probe.connectToHost(QHostAddress::LocalHost, port);
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
            std::unique_ptr<QTcpSocket> probePeer = accept();
            QVERIFY(probePeer);
            probe.abort();
            probePeer->abort();
            attempt();
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
            peer = accept();
            QVERIFY(peer);
            const QByteArray greeting = cycle == 0 ? "V1.4.0.0\nH00000002\n" : "V1.4.0.0\nH00000003\n";
            peer->write(greeting);
            peer->flush();
            QTRY_COMPARE_WITH_TIMEOUT(connected.size(), cycle + 2, 1000);
            QCOMPARE(connection.clientHandle(), quint32(cycle + 2));
            QCOMPARE(connection.localAddress(), QHostAddress(QHostAddress::LocalHost));
            QVERIFY(connection.localTcpPort() != 0);
        }
        peer->abort();
        QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionState::Disconnected, 1000);
    }
};

QTEST_GUILESS_MAIN(RadioConnectionRetryTest)
#include "radio_connection_retry_test.moc"
