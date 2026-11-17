#include "AetherPDTestTransport.h"
#include "core/AetherPDInstallationRead.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <limits>
#include <memory>

using namespace AetherSDR;
namespace {
QByteArray record(bool first = false, bool committed = true)
{
    return QJsonDocument(QJsonObject{{"state", committed ? "committed" : "rolled_back"},
        {"installation_committed", committed}, {"profile", 1}, {"firmware", "4.3.16.42034"}, {"serial", "fixture-8600"},
        {first ? "first_install_plan_blake2b_256" : "upgrade_plan_blake2b_256", QString(64, u'1')},
        {"selected_release_blake2b_256", QString(64, u'2')}, {"selected_release_key", QString(64, u'3')}}).toJson();
}
}
class InstallationReadTest : public QObject {
    Q_OBJECT
private slots:
    void oneReadHasFixedPathAndNoMutationOrRedirect()
    {
        for (int mode = 0; mode < 3; ++mode) {
            AetherPDTest::Network network; qint64 clock = 0;
            AetherPDInstallationRead read(network, [&] { return clock; });
            QSignalSpy finished(&read, &AetherPDInstallationRead::finished);
            QVERIFY(read.begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
            QVERIFY(!read.begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
            QCOMPARE(network.sent.size(), std::size_t(1));
            const auto& sent = network.sent[0];
            QCOMPARE(sent.method, QNetworkAccessManager::GetOperation); QVERIFY(sent.body.isEmpty());
            QCOMPARE(sent.request.url().toString(), QStringLiteral("http://192.168.50.100:48992/v1/aetherpd/installation"));
            QCOMPARE(sent.request.rawHeader("Authorization"), QByteArray("Bearer ") + QByteArray(64, 'a'));
            QCOMPARE(sent.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
            QCOMPARE(sent.request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
            QCOMPARE(sent.request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), int(QNetworkRequest::Manual));
            network.last->complete(200, record(mode == 1, mode != 2));
            QCOMPARE(finished.size(), 1); QCOMPARE(read.state(), AetherPDInstallationRead::State::Ready);
            const auto observed = read.observation(); QVERIFY(observed);
            QCOMPARE(observed->firstInstall, mode == 1); QCOMPARE(observed->committed, mode != 2);
            QVERIFY(!read.begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
        }
    }
    void invalidTargetOrTokenStartsNothing()
    {
        AetherPDTest::Network network; AetherPDInstallationRead read(network);
        for (const auto& address : {QHostAddress{}, QHostAddress("8.8.8.8"), QHostAddress("2001:4860:4860::8888")}) {
            QVERIFY(!read.begin(address, QString(64, u'a')));
        }
        for (const auto& token : {QString{}, QString(63, u'a'), QString(64, u'g'), QString(63, u'a') + u'\n'}) {
            QVERIFY(!read.begin(QHostAddress("192.168.50.100"), token));
        }
        QVERIFY(network.sent.empty());
        AetherPDInstallationRead invalidClock(network, [] { return std::numeric_limits<qint64>::max(); });
        QVERIFY(!invalidClock.begin(QHostAddress("192.168.50.100"), QString(64, u'a'))); QVERIFY(network.sent.empty());
    }
    void fragmentedReplyDoesNotExtendDeadline()
    {
        AetherPDTest::Network network; qint64 clock = 0;
        AetherPDInstallationRead read(network, [&] { return clock; });
        QVERIFY(read.begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
        const auto bytes = record();
        for (qsizetype i = 0; i < bytes.size(); ++i) { clock = i; network.last->feed(bytes.mid(i, 1)); }
        clock = 10000; network.last->finish(200);
        QCOMPARE(read.failure(), AetherPDInstallationRead::Failure::Timeout); QVERIFY(!read.observation());
        QCOMPARE(network.sent.size(), std::size_t(1));
    }
    void failuresPublishNoObservation_data()
    {
        QTest::addColumn<int>("mode");
        for (int mode = 0; mode < 12; ++mode) { QTest::newRow(qPrintable(QString::number(mode))) << mode; }
    }
    void failuresPublishNoObservation()
    {
        QFETCH(int, mode);
        AetherPDTest::Network network; qint64 clock = 0;
        AetherPDInstallationRead read(network, [&] { return clock; });
        QSignalSpy finished(&read, &AetherPDInstallationRead::finished);
        QVERIFY(read.begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
        if (mode < 5) { network.last->complete(mode == 0 ? 401 : mode == 1 ? 403 : mode == 2 ? 404 : mode == 3 ? 503 : 302, record()); }
        if (mode == 5) { network.last->complete(200, record(), QNetworkReply::RemoteHostClosedError); }
        if (mode == 6) { network.last->announceLength(2049); }
        if (mode == 7) { network.last->complete(200, QByteArray(2049, ' ')); }
        if (mode == 8) { network.last->feed(QByteArray(1100, ' ')); network.last->feed(QByteArray(1000, ' ')); }
        if (mode == 9) { network.last->complete(200, record().first(25)); }
        if (mode == 10) { clock = -1; network.last->complete(200, record()); }
        if (mode == 11) { QVERIFY(QMetaObject::invokeMethod(read.findChild<QTimer*>(), "timeout", Qt::DirectConnection)); }
        QCOMPARE(finished.size(), 1); QCOMPARE(read.state(), AetherPDInstallationRead::State::Failed); QVERIFY(!read.observation());
        if (mode < 2) { QCOMPARE(read.failure(), AetherPDInstallationRead::Failure::Unauthorized); }
        if (mode == 10) { QCOMPARE(read.failure(), AetherPDInstallationRead::Failure::InvalidClock); }
        QCOMPARE(network.sent.size(), std::size_t(1));
    }
    void cancelManagerLossAndDeletionIgnoreLateReplies()
    {
        auto network = std::make_unique<AetherPDTest::Network>();
        auto read = std::make_unique<AetherPDInstallationRead>(*network);
        QVERIFY(read->begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
        const auto old = network->last;
        read->cancel(); old->complete(200, record());
        QCOMPARE(read->state(), AetherPDInstallationRead::State::Canceled); QVERIFY(!read->observation());
        read = std::make_unique<AetherPDInstallationRead>(*network);
        QVERIFY(read->begin(QHostAddress("192.168.50.100"), QString(64, u'a'))); network.reset();
        QCOMPARE(read->state(), AetherPDInstallationRead::State::Failed); QVERIFY(!read->observation());
        AetherPDTest::Network another;
        read = std::make_unique<AetherPDInstallationRead>(another);
        connect(read.get(), &AetherPDInstallationRead::finished, &another, [&] { read.reset(); });
        QVERIFY(read->begin(QHostAddress("192.168.50.100"), QString(64, u'a')));
        another.last->complete(200, record()); QVERIFY(!read);
    }
};
QTEST_GUILESS_MAIN(InstallationReadTest)
#include "aetherpd_installation_read_test.moc"
