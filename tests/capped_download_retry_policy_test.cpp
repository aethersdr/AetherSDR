// Which transport failures CappedDownload retries, without a socket: the HTTP/2
// refusals from #7109 cannot be produced by a loopback HTTP/1.1 peer.
#include "core/CappedDownload.h"

#include <QtTest>

using namespace AetherSDR;

class CappedDownloadRetryPolicyTest final : public QObject {
    Q_OBJECT
private slots:
    void retried_data()
    {
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("http2");
        // Graceful GOAWAY above the last stream id (Qt's "stopped accepting
        // new streams" message, #7109).
        QTest::newRow("goaway") << int(QNetworkReply::ContentReSendError) << true;
        QTest::newRow("goaway-with-error-code")
            << int(QNetworkReply::ProtocolUnknownError) << true;
        QTest::newRow("stream-reset") << int(QNetworkReply::ProtocolFailure) << true;
        QTest::newRow("frame-not-sent") << int(QNetworkReply::UnknownNetworkError) << true;
        QTest::newRow("connection-reset")
            << int(QNetworkReply::RemoteHostClosedError) << false;
    }
    void retried()
    {
        QFETCH(int, error);
        QFETCH(bool, http2);
        QVERIFY(isRetryableBeforeBody(QNetworkReply::NetworkError(error), http2));
    }

    void final_data()
    {
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("http2");
        QTest::newRow("unknown-over-http1")
            << int(QNetworkReply::UnknownNetworkError) << false;
        QTest::newRow("cancel-or-transfer-timeout")
            << int(QNetworkReply::OperationCanceledError) << true;
        QTest::newRow("timeout") << int(QNetworkReply::TimeoutError) << true;
        QTest::newRow("404") << int(QNetworkReply::ContentNotFoundError) << true;
        QTest::newRow("5xx") << int(QNetworkReply::InternalServerError) << true;
        QTest::newRow("insecure-redirect")
            << int(QNetworkReply::InsecureRedirectError) << true;
        QTest::newRow("too-many-redirects")
            << int(QNetworkReply::TooManyRedirectsError) << true;
        QTest::newRow("tls") << int(QNetworkReply::SslHandshakeFailedError) << true;
        QTest::newRow("refused") << int(QNetworkReply::ConnectionRefusedError) << true;
        QTest::newRow("host-not-found") << int(QNetworkReply::HostNotFoundError) << true;
    }
    void final()
    {
        QFETCH(int, error);
        QFETCH(bool, http2);
        QVERIFY(!isRetryableBeforeBody(QNetworkReply::NetworkError(error), http2));
    }
};

QTEST_GUILESS_MAIN(CappedDownloadRetryPolicyTest)
#include "capped_download_retry_policy_test.moc"
