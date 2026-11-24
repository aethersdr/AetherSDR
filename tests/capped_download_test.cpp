// The updater, firmware and catalog downloads all share this transport.
// Exercise a real Qt HTTP client against a localhost HTTP peer that can
// interrupt the transfer without contacting GitHub or a radio.
#include "core/CappedDownload.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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

    // A peer that reads each HTTP request and then either drops the connection
    // with no reply or serves it. Requests are counted, not sockets: Qt may
    // open a spare connection that never carries one.
    struct DroppingPeer {
        QTcpServer server;
        int requests{0};
        int dropFirst{0};   // requests to drop; < 0 drops every one

        bool listen()
        {
            if (!server.listen(QHostAddress::LocalHost))
                return false;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
                while (server.hasPendingConnections()) {
                    QTcpSocket* peer = server.nextPendingConnection();
                    QObject::connect(peer, &QTcpSocket::readyRead, peer, [this, peer] {
                        if (peer->property("seen").toBool()
                            || !peer->peek(4096).contains("\r\n\r\n"))
                            return;
                        peer->setProperty("seen", true);
                        ++requests;
                        if (dropFirst < 0 || requests <= dropFirst) {
                            peer->abort();
                        } else {
                            peer->write(response(200, "complete"));
                            peer->flush();
                            peer->disconnectFromHost();
                        }
                    });
                }
            });
            return true;
        }
        QUrl url() const { return endpoint(server); }
    };

    // Qt's HTTP channel resends an idempotent GET on its own after the peer
    // drops it, and how often varies by Qt version. Count the requests one
    // GET with CappedDownload's settings makes, so the retry tests below
    // demand more than Qt alone sends.
    static int requestsPerQtGet()
    {
        DroppingPeer peer;
        peer.dropFirst = -1;
        if (!peer.listen())
            return -1;
        QNetworkAccessManager nam;
        nam.setTransferTimeout(30000);
        QNetworkRequest request(peer.url());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam.get(request);
        reply->setReadBufferSize(64 * 1024);
        QSignalSpy done(reply, &QNetworkReply::finished);
        if (!done.wait(5000))
            return -1;
        delete reply;
        return peer.requests;
    }

    int m_qtRequests{0};

private slots:
    void initTestCase()
    {
        m_qtRequests = requestsPerQtGet();
        QVERIFY2(m_qtRequests >= 1, "could not count Qt's own resends");
        qInfo("Qt sends %d request(s) for one GET to a dropping peer", m_qtRequests);
    }

    void retryAfterEmptyDisconnect()
    {
        // Every request Qt sends on its own is dropped, so a body arrives
        // only through CappedDownload's retry.
        DroppingPeer peer;
        peer.dropFirst = m_qtRequests;
        QVERIFY(peer.listen());
        CappedDownload download;
        QSignalSpy success(&download, &CappedDownload::finished);
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(peer.url(), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(success.size() + failure.size(), 1, 5000);
        QVERIFY(failure.isEmpty());
        QCOMPARE(success.at(0).at(1).toByteArray(), QByteArray("complete"));
        QCOMPARE(peer.requests, m_qtRequests + 1);
    }

    void exhaustedRetryFailsOnce()
    {
        DroppingPeer peer;
        peer.dropFirst = -1;
        QVERIFY(peer.listen());
        CappedDownload download;
        QSignalSpy success(&download, &CappedDownload::finished);
        QSignalSpy failure(&download, &CappedDownload::failed);
        download.get(peer.url(), 100, QStringLiteral("Too large"));
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(), 1, 5000);
        // One retry, then one terminal failed().
        QCOMPARE(peer.requests, 2 * m_qtRequests);
        QTest::qWait(200);
        QCOMPARE(failure.size(), 1);
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
