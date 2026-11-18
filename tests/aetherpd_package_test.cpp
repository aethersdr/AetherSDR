#include "AetherPDPackageFixture.h"
#include "core/AetherPDProtocol.h"
#include "core/ZipArchive.h"
#include <QtTest>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace AetherSDR;
class PackageTest : public QObject {
    Q_OBJECT
private slots:
    void controlRecoveryComesFromVerifiedCandidate_data()
    {
        QTest::addColumn<int>("arch");
        QTest::addColumn<bool>("bounded");
        QTest::newRow("arm64-manual") << 64 << false;
        QTest::newRow("arm64-bounded") << 64 << true;
        QTest::newRow("armv7-manual") << 7 << false;
        QTest::newRow("armv7-bounded") << 7 << true;
    }
    void controlRecoveryComesFromVerifiedCandidate()
    {
        QFETCH(int, arch); QFETCH(bool, bounded);
        AetherPDTest::PackageFixture fixture(arch);
        QVERIFY(fixture.valid);
        if (bounded) { QVERIFY(fixture.enableControlRecovery()); }
        const auto package = fixture.load(); QVERIFY(package);
        const auto expected = bounded ? AetherPDControlRecovery::BoundedRestart : AetherPDControlRecovery::ExplicitStart;
        QCOMPARE(package->controlRecovery(), expected);
        if (arch == 64) {
            const auto first = fixture.loadFirst(); QVERIFY(first);
            QCOMPARE(first->controlRecovery(), expected);
        }
        QVERIFY(fixture.replacePrimaryUnit("unrecognized after verification"));
        QVERIFY(!fixture.load());
        QCOMPARE(package->controlRecovery(), expected);
    }
    void signedServiceCannotMisstateRecovery_data()
    {
        QTest::addColumn<QByteArray>("before"); QTest::addColumn<QByteArray>("after");
        QTest::newRow("different-delay") << QByteArray("RestartSec=5\n") << QByteArray("RestartSec=1\n");
        QTest::newRow("different-budget") << QByteArray("StartLimitBurst=3\n") << QByteArray("StartLimitBurst=0\n");
        QTest::newRow("reboot-action") << QByteArray("StartLimitAction=none\n") << QByteArray("StartLimitAction=reboot\n");
        QTest::newRow("shadowed-restart") << QByteArray("Restart=on-failure\n") << QByteArray("Restart=on-failure\nRestart=always\n");
        QTest::newRow("description-only") << QByteArray("Restart=on-failure\n") << QByteArray("# Restart=on-failure\n");
        QTest::newRow("foreign-command") << QByteArray("/pd-boot --primary\n") << QByteArray("/pd-boot --primary --unexpected\n");
    }
    void legacyManualControlEntryRemainsAccepted()
    {
        for (const int arch : {64, 7}) {
            AetherPDTest::PackageFixture fixture(arch);
            QByteArray unit = fixture.primaryUnit();
            unit.replace(arch == 64 ? "/var/lib/smartsdr/aetherpd/pd-boot --primary" : "/user/aetherpd/pd-boot --primary",
                "/run/aetherpd/bin/pd-primary-launch");
            QVERIFY(fixture.replacePrimaryUnit(unit));
            const auto package = fixture.load(); QVERIFY(package);
            QCOMPARE(package->controlRecovery(), AetherPDControlRecovery::ExplicitStart);
        }
    }
    void signedServiceCannotMisstateRecovery()
    {
        QFETCH(QByteArray, before); QFETCH(QByteArray, after);
        AetherPDTest::PackageFixture fixture; QVERIFY(fixture.enableControlRecovery()); QVERIFY(fixture.load());
        QByteArray unit = fixture.primaryUnit(); QVERIFY(unit.contains(before)); unit.replace(before, after);
        QVERIFY(fixture.replacePrimaryUnit(unit));
        QVERIFY(!fixture.load()); QVERIFY(!fixture.loadFirst());
    }
    void freshPackageHasNoPredecessor()
    {
        AetherPDTest::PackageFixture fixture; QVERIFY(fixture.valid);
        AetherPDFirstPackage::Files files;
        for (std::size_t i = 0; i < files.size(); ++i) { files[i] = fixture.bytes(i); }
        files[0] = AetherPDTest::PackageFixture::decode(fixture.fixture.value("first_worker"));
        files[2] = fixture.bytes(3);
        AetherPDFirstPackage::Trust trust;
        trust.publisherKey = fixture.trust.candidateKey;
        for (std::size_t i = 0; i < files.size(); ++i) {
            trust.files[i] = {files[i].size(), QCryptographicHash::hash(files[i], QCryptographicHash::Sha256)};
        }
        const auto package = AetherPDFirstPackage::verify(files, trust); QVERIFY(package);
        const auto uploads = package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n'));
        QVERIFY(uploads);
        QCOMPARE(uploads->plan, AetherPDTest::PackageFixture::decode(fixture.fixture.value("first_plan")));
        QCOMPARE(QCryptographicHash::hash(uploads->nativeArchive, QCryptographicHash::Sha256).toHex(),
                 fixture.fixture.value("first_native_sha256").toString().toLatin1());
        QCOMPARE(uploads->nativeName, QStringLiteral("AetherPDFirst6e6e6e6e6e6e6e6e"));
        QCOMPARE(uploads->carrier.containerName, QStringLiteral("aetherpd-first-6e6e6e6e6e6e6e6e"));
        QCOMPARE(uploads->publisherKey, trust.publisherKey);
        QString error;
        const auto entries = readZipEntries(uploads->nativeArchive, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(entries.size(), 8);
        QCOMPARE(entries.value(uploads->nativeName + QStringLiteral("/plan")), uploads->plan);
        const auto repeat = package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n'));
        QVERIFY(repeat); QCOMPARE(repeat->nativeArchive, uploads->nativeArchive); QCOMPARE(repeat->carrier.archive, uploads->carrier.archive);
        for (const QString& hostname : {QString(), QStringLiteral("-bad"), QStringLiteral("bad-"), QStringLiteral("bad/name"), QString(64, u'a')}) {
            QVERIFY(!package->prepare("fixture-8600", QByteArray(32, 't'), hostname, QByteArray(32, 'n')));
        }
        QVERIFY(!package->prepare("fixture-8600", QByteArray(32, '\0'), "fixture-radio", QByteArray(32, 'n')));
        QVERIFY(!package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, '\0')));
        QVERIFY(!package->prepare("bad\nserial", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n')));
        for (std::size_t i = 0; i < files.size(); ++i) {
            auto wrong = trust; wrong.files[i].sha256[0] ^= 1;
            QVERIFY(!AetherPDFirstPackage::verify(files, wrong));
        }
        auto wrongKey = trust; wrongKey.publisherKey[0] ^= 1;
        QVERIFY(!AetherPDFirstPackage::verify(files, wrongKey));
        for (const std::size_t i : {0u, 1u, 2u}) {
            auto damaged = files; damaged[i][i == 0 ? 120 : i == 1 ? 368 : 0] ^= 1;
            auto repinned = trust;
            repinned.files[i].sha256 = QCryptographicHash::hash(damaged[i], QCryptographicHash::Sha256);
            QVERIFY(!AetherPDFirstPackage::verify(damaged, repinned));
        }
        auto upgrade = files; upgrade[0] = fixture.bytes(0);
        auto upgradeTrust = trust; upgradeTrust.files[0] = {upgrade[0].size(), QCryptographicHash::hash(upgrade[0], QCryptographicHash::Sha256)};
        QVERIFY(!AetherPDFirstPackage::verify(upgrade, upgradeTrust));
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto names = AetherPDFirstPackage::fileNames();
        for (std::size_t i = 0; i < files.size(); ++i) {
            QFile file(QDir(directory.path()).filePath(names[i])); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(files[i]), files[i].size());
        }
        QVERIFY(AetherPDFirstPackage::load(directory.path(), trust));
        QVERIFY(QFile::remove(QDir(directory.path()).filePath(names[2])));
        QVERIFY(!AetherPDFirstPackage::load(directory.path(), trust));
        QVERIFY(package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n')));
    }
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
