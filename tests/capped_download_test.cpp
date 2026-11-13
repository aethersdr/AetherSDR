// The updater, firmware and catalog downloads all share this transport.
// Exercise a real Qt HTTP client against a localhost HTTP peer that can
// interrupt the transfer without contacting GitHub or a radio.
#include "core/CappedDownload.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace AetherSDR;

class CappedDownloadTest final : public QObject {
    Q_OBJECT
private:
    static QUrl endpoint(const QTcpServer& server)
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/release.tar.zst")
                        .arg(server.serverPort()));
    }

    static QByteArray response(int status, const QByteArray& body,
                               qint64 announcedLength = -1)
    {
        if (announcedLength < 0)
            announcedLength = body.size();
        return QByteArray("HTTP/1.1 ") +
            (status == 200 ? "200 OK" : "404 Not Found") +
            "\r\nConnection: close\r\nContent-Length: " +
            QByteArray::number(announcedLength) + "\r\n\r\n" + body;
    }

private slots:
    void retryAfterEmptyDisconnect()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                QTcpSocket* peer = server.nextPendingConnection();
                ++connections;
                if (connections <= 2) {
                    peer->abort();
                } else {
                    peer->write(response(200, "complete"));
                    peer->flush();
                    peer->disconnectFromHost();
                }
            }
        });
        CappedDownload download;
        QSignalSpy success(&download, &CappedDownload::finished);
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(endpoint(server), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(success.size(), 1, 5000);
        // Qt itself may transparently reconnect once before reporting an
        // error, so count on-wire connections, not our QNAM get() calls.
        QVERIFY(connections >= 3 && connections <= 4);
        QVERIFY(failure.isEmpty());
        QCOMPARE(success.at(0).at(1).toByteArray(), QByteArray("complete"));
    }

    void exhaustedRetryFailsOnce()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                ++connections;
                server.nextPendingConnection()->abort();
            }
        });
        CappedDownload download;
        QSignalSpy success(&download, &CappedDownload::finished);
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(endpoint(server), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
        QVERIFY(connections >= 2 && connections <= 4);
        QVERIFY(success.isEmpty());
    }

    void http404NeverRetries()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            ++connections;
            QTcpSocket* peer = server.nextPendingConnection();
            peer->write(response(404, {}));
            peer->flush();
            peer->disconnectFromHost();
        });
        CappedDownload download;
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(endpoint(server), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
        QCOMPARE(connections, 1);
    }

    void partialBodyNeverRetries()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            ++connections;
            QTcpSocket* peer = server.nextPendingConnection();
            peer->write(response(200, "partial", 100));
            peer->flush();
            peer->disconnectFromHost();
        });
        CappedDownload download;
        QSignalSpy success(&download, &CappedDownload::finished);
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(endpoint(server), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
        QCOMPARE(connections, 1);
        QVERIFY(success.isEmpty());
    }

    void oversizedBodyNeverRetries()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            ++connections;
            QTcpSocket* peer = server.nextPendingConnection();
            peer->write(response(200, "oversized", 1000));
            peer->flush();
            peer->disconnectFromHost();
        });
        CappedDownload download;
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(endpoint(server), 4, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
        QCOMPARE(connections, 1);
        QCOMPARE(failure.at(0).at(0).toString(), QStringLiteral("Too large"));
    }
};

QTEST_GUILESS_MAIN(CappedDownloadTest)
#include "capped_download_test.moc"
