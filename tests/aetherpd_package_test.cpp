#include "AetherPDPackageFixture.h"
#include "core/AetherPDProtocol.h"
#include <QtTest>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace AetherSDR;
class PackageTest : public QObject {
    Q_OBJECT
private slots:
    void nativePackagesAndImmutableSnapshot_data()
    {
        QTest::addColumn<int>("arch");
        QTest::newRow("arm64") << 64;
        QTest::newRow("armv7") << 7;
    }
    void nativePackagesAndImmutableSnapshot()
    {
        QFETCH(int, arch);
        AetherPDTest::PackageFixture fixture(arch); QVERIFY(fixture.valid);
        const auto package = fixture.load(); QVERIFY(package);
        QCOMPARE(package->profile(), arch == 64 ? 1u : 2u);
        QCOMPARE(package->firmware(), QStringLiteral("4.3.16.42034"));
        const QByteArray plan = package->plan("fixture-8600");
        const auto parsed = aetherpd::parseInstallationPlan(plan); QVERIFY(parsed);
        QCOMPARE(parsed->candidateDigest.toLatin1(), QCryptographicHash::hash(fixture.bytes(1), QCryptographicHash::Blake2b_256).toHex());
        QVERIFY(package->plan(QString()).isEmpty());
        QVERIFY(package->plan(QString(64, u'x')).isEmpty());
        QVERIFY(fixture.write(1, "replaced after verification"));
        QVERIFY(!fixture.load());
        QCOMPARE(package->plan("fixture-8600"), plan);
    }
    void independentPinsAndKeys()
    {
        AetherPDTest::PackageFixture fixture; QVERIFY(fixture.load());
        for (std::size_t i = 0; i < 8; ++i) {
            const auto saved = fixture.trust.files[i];
            fixture.trust.files[i].sha256[0] ^= 1; QVERIFY(!fixture.load());
            fixture.trust.files[i] = saved;
            fixture.trust.files[i].size = 0; QVERIFY(!fixture.load());
            fixture.trust.files[i].size = 100 * 1024 * 1024; QVERIFY(!fixture.load());
            fixture.trust.files[i] = saved;
        }
        fixture.trust.candidateKey[0] ^= 1; QVERIFY(!fixture.load());
        fixture.trust.candidateKey[0] ^= 1;
        fixture.trust.previousKey[0] ^= 1; QVERIFY(!fixture.load());
    }
    void authenticatedInputsStillRequireValidEnvelopes_data()
    {
        QTest::addColumn<int>("index"); QTest::addColumn<int>("offset");
        QTest::newRow("worker-signature") << 0 << 120;
        QTest::newRow("worker-code-hash") << 0 << 190;
        QTest::newRow("candidate-signature") << 1 << 368;
        QTest::newRow("candidate-member") << 1 << 440;
        QTest::newRow("previous-signature") << 2 << 368;
        QTest::newRow("previous-member") << 2 << 440;
        for (int i = 3; i < 7; ++i) {
            QTest::newRow(qPrintable(QStringLiteral("carrier-elf-%1").arg(i))) << i << 0;
        }
    }
    void authenticatedInputsStillRequireValidEnvelopes()
    {
        QFETCH(int, index); QFETCH(int, offset);
        AetherPDTest::PackageFixture fixture;
        QByteArray bytes = fixture.bytes(index); QVERIFY(bytes.size() > offset);
        bytes[offset] ^= 1;
        QVERIFY(fixture.write(index, bytes, true)); QVERIFY(!fixture.load());
    }
    void boundedAndExactLengths()
    {
        for (std::size_t i = 0; i < 3; ++i) {
            AetherPDTest::PackageFixture fixture;
            const QByteArray original = fixture.bytes(i);
            QVERIFY(fixture.write(i, original + 'x', true)); QVERIFY(!fixture.load());
            QVERIFY(fixture.write(i, original.first(original.size() - 1), true)); QVERIFY(!fixture.load());
            QVERIFY(fixture.write(i, original.first(10), true)); QVERIFY(!fixture.load());
        }
    }
    void validSignatureCannotAuthorizeWrongWorkerBinding()
    {
        for (const QString& name : {QStringLiteral("wrong_source_worker"), QStringLiteral("wrong_release_worker")}) {
            AetherPDTest::PackageFixture fixture;
            QVERIFY(fixture.write(0, AetherPDTest::PackageFixture::decode(fixture.fixture.value(name)), true));
            QVERIFY(!fixture.load());
        }
    }
    void mixedArchitectureRefused()
    {
        AetherPDTest::PackageFixture fixture, arm(7);
        QVERIFY(fixture.write(2, arm.bytes(2), true)); QVERIFY(!fixture.load());
    }
    void nonRegularInputsRefused()
    {
        AetherPDTest::PackageFixture fixture; QVERIFY(fixture.load());
        QVERIFY(QFile::remove(fixture.path(0)));
        QVERIFY(QDir().mkdir(fixture.path(0))); QVERIFY(!fixture.load());
#ifdef Q_OS_UNIX
        QVERIFY(QDir().rmdir(fixture.path(0)));
        QVERIFY(QFile::link(fixture.path(1), fixture.path(0))); QVERIFY(!fixture.load());
        QVERIFY(QFile::remove(fixture.path(0)));
        QCOMPARE(::mkfifo(QFile::encodeName(fixture.path(0)).constData(), 0600), 0);
        QVERIFY(!fixture.load()); // O_NONBLOCK prevents opening this FIFO from hanging.
#endif
    }
};
QTEST_GUILESS_MAIN(PackageTest)
#include "aetherpd_package_test.moc"
