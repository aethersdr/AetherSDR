#include <QTest>
#include <QDateTime>
#include <QFile>
#include <QTemporaryFile>
#include <QSignalSpy>

#include "core/EibiClient.h"

using namespace AetherSDR;

class EibiClientTest : public QObject {
    Q_OBJECT

private slots:
    void testCacheFilePath();
    void testEnabledState();
    void testCodeLookups();
    void testScheduleFormatting();
    void testParseContent();
};

void EibiClientTest::testCacheFilePath()
{
    const QString path = EibiClient::cacheFilePath();
    QVERIFY(!path.isEmpty());
    QVERIFY(path.endsWith(QStringLiteral("/eibi.txt")));
}

void EibiClientTest::testEnabledState()
{
    EibiClient client;

    QCOMPARE(client.isEnabled(), false);
    client.initialize();
    QCOMPARE(client.isEnabled(), false);
}

void EibiClientTest::testCodeLookups()
{
    // Test Country Lookups
    QCOMPARE(EibiClient::lookupCountry(QStringLiteral("IND")), QStringLiteral("India"));
    QCOMPARE(EibiClient::lookupCountry(QStringLiteral("USA")), QStringLiteral("United States of America"));
    QCOMPARE(EibiClient::lookupCountry(QStringLiteral("CUB")), QStringLiteral("Cuba"));
    QCOMPARE(EibiClient::lookupCountry(QStringLiteral("RUS")), QStringLiteral("Russian Federation"));

    // Test Language Lookups
    QCOMPARE(EibiClient::lookupLanguage(QStringLiteral("E")), QStringLiteral("English"));
    QCOMPARE(EibiClient::lookupLanguage(QStringLiteral("M")), QStringLiteral("Mandarin"));
    QCOMPARE(EibiClient::lookupLanguage(QStringLiteral("S")), QStringLiteral("Spanish/Castellano"));
    QCOMPARE(EibiClient::lookupLanguage(QStringLiteral("-CW")), QStringLiteral("Morse Station"));
    QCOMPARE(EibiClient::lookupLanguage(QStringLiteral("-TS")), QStringLiteral("Time Signal Station"));

    // Test Target Lookups
    QCOMPARE(EibiClient::lookupTarget(QStringLiteral("Eu")), QStringLiteral("Europe"));
    QCOMPARE(EibiClient::lookupTarget(QStringLiteral("EEu")), QStringLiteral("Eastern Europe"));
    QCOMPARE(EibiClient::lookupTarget(QStringLiteral("SAs")), QStringLiteral("South Asia"));
    QCOMPARE(EibiClient::lookupTarget(QStringLiteral("NAm")), QStringLiteral("North America"));
    QCOMPARE(EibiClient::lookupTarget(QStringLiteral("CLM")), QStringLiteral("Colombia"));

    // Test Site Lookups
    QCOMPARE(EibiClient::lookupSite(QStringLiteral("BUL"), QStringLiteral("s")), QStringLiteral("Sofia-Kostinbrod"));
    QCOMPARE(EibiClient::lookupSite(QStringLiteral("USA"), QStringLiteral("/BUL-s")), QStringLiteral("Relay: Bulgaria (Sofia-Kostinbrod)"));
}

void EibiClientTest::testScheduleFormatting()
{
    // Test formatDays
    QCOMPARE(EibiClient::formatDays(QStringLiteral("")), QStringLiteral("Daily"));
    QCOMPARE(EibiClient::formatDays(QStringLiteral("Mo-Fr")), QStringLiteral("Mon - Fri"));
    QCOMPARE(EibiClient::formatDays(QStringLiteral("SaSu")), QStringLiteral("Sat & Sun"));
    QCOMPARE(EibiClient::formatDays(QStringLiteral("1245")), QStringLiteral("Mon, Tue, Thu, Fri"));
    QCOMPARE(EibiClient::formatDays(QStringLiteral("1.Sa")), QStringLiteral("1st Saturday of month"));

    // Test formatTime
    QCOMPARE(EibiClient::formatTime(0, 1440), QStringLiteral("24 Hours (00:00 - 24:00 UTC)"));
    QCOMPARE(EibiClient::formatTime(300, 630), QStringLiteral("05:00 - 10:30 UTC"));
    QCOMPARE(EibiClient::formatTime(1350, 120), QStringLiteral("22:30 - 02:00 UTC (Overnight)"));
}

void EibiClientTest::testParseContent()
{
    EibiClient client;

    const QString sampleSchedule = QStringLiteral(
        "EiBi Shortwave Schedule Header\n"
        "kHz:\n"
        "====================================================================================================\n"
        "6070          0500-1030  Mo-Fr  CAN  Radio Canada Int        E      Eu       /BUL-kos\n"
        "9500          2230-0200         USA  Voice of America        E      SAs      /PHL-tin\n"
    );

    QVERIFY(client.parseContent(sampleSchedule));
}

QTEST_MAIN(EibiClientTest)
#include "eibi_client_test.moc"
