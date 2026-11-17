#include "AetherPDPackageFixture.h"
#include "AetherPDTestTransport.h"
#include "core/AetherPDPackageDownload.h"
#include "core/AetherPDReleaseCatalogue.h"
#include <QtTest>
#include <memory>

using namespace AetherSDR;
namespace {
AetherPDRelease release(const AetherPDTest::PackageFixture& fixture, int arch = 64)
{
    AetherPDRelease result;
    result.version = "0.1.0-test"; result.firmware = "4.3.16.42034"; result.profile = arch == 64 ? 1 : 2;
    result.trust = fixture.trust;
    const auto names = AetherPDPackage::fileNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        result.urls[i] = QUrl(QStringLiteral("https://example.invalid/aetherpd/") + names[i]);
    }
    return result;
}
AetherPDFirstRelease firstRelease(const AetherPDTest::PackageFixture& fixture)
{
    AetherPDFirstRelease result;
    result.version = "0.1.0-test"; result.firmware = "4.3.16.42034"; result.profile = 1;
    result.trust = fixture.firstTrust();
    const auto names = AetherPDFirstPackage::fileNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        result.urls[i] = QUrl(QStringLiteral("https://example.invalid/first/") + names[i]);
    }
    return result;
}
}
class DownloadTest : public QObject {
    Q_OBJECT
private slots:
    void catalogueBindsExactModelFirmwareAndRetainsRecoveryReleases()
    {
        AetherPDTest::PackageFixture fixture;
        const auto package = fixture.loadFirst(); QVERIFY(package);
        AetherPDReleaseCatalogue::First offered{"FLEX-8600", firstRelease(fixture), package->candidateDigest(), true};
        auto retained = offered; retained.offer = false; retained.candidateDigest = QByteArray(32, 'r');
        const AetherPDReleaseCatalogue catalogue({retained, offered}); QVERIFY(catalogue.valid());
        QVERIFY(catalogue.first("FLEX-8600", "4.3.16.42034"));
        QVERIFY(!catalogue.first("FLEX-6300", "4.3.16.42034"));
        QVERIFY(!catalogue.first("FLEX-8600", "4.3.16.42035"));
        QCOMPARE(catalogue.retainedFirst("FLEX-8600", "4.3.16.42034").size(), std::size_t(2));
        const AetherPDReleaseCatalogue oldOnly({retained}); QVERIFY(oldOnly.valid());
        QVERIFY(!oldOnly.first("FLEX-8600", "4.3.16.42034"));
        QCOMPARE(oldOnly.retainedFirst("FLEX-8600", "4.3.16.42034").size(), std::size_t(1));
        QVERIFY(AetherPDReleaseCatalogue::shipping().valid()); QVERIFY(AetherPDReleaseCatalogue::shipping().empty());
    }
    void invalidOrAmbiguousCatalogueSelectsNothing()
    {
        AetherPDTest::PackageFixture fixture;
        const AetherPDReleaseCatalogue::First entry{"FLEX-8600", firstRelease(fixture), QByteArray(32, 'c'), true};
        for (int mode = 0; mode < 7; ++mode) {
            std::vector<AetherPDReleaseCatalogue::First> entries{entry};
            if (mode == 0) { entries[0].model.clear(); }
            if (mode == 1) { entries[0].candidateDigest = QByteArray(32, '\0'); }
            if (mode == 2) { entries[0].release.profile = 2; }
            if (mode == 3) { entries.push_back(entry); }
            if (mode == 4) { entries.push_back(entry); entries[1].candidateDigest = QByteArray(32, 'd'); }
            if (mode == 5) { entries[0].release.trust.publisherKey.clear(); }
            if (mode == 6) { entries.resize(33, entry); }
            const AetherPDReleaseCatalogue catalogue(entries); QVERIFY(!catalogue.valid());
            QVERIFY(!catalogue.first("FLEX-8600", "4.3.16.42034"));
            QVERIFY(catalogue.retainedFirst("FLEX-8600", "4.3.16.42034").empty());
        }
    }
    void completeFirstReleaseHasNoPredecessorAndClearsOtherResult()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy ready(&job, &AetherPDPackageDownload::ready);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        for (int mode : {0, 1, 0}) {
            if (mode == 0) { QVERIFY(job.start(release(fixture))); }
            else { QVERIFY(job.start(firstRelease(fixture))); }
            QVERIFY(!job.package()); QVERIFY(!job.firstPackage());
            const auto files = mode == 0 ? AetherPDPackage::Files{} : fixture.firstFiles();
            for (std::size_t i = 0; i < 8; ++i) {
                if (mode == 1) { QCOMPARE(network.sent.back().request.url(), firstRelease(fixture).urls[i]); }
                network.last->complete(200, mode == 0 ? fixture.bytes(i) : files[i]);
            }
            QCOMPARE(bool(job.package()), mode == 0); QCOMPARE(bool(job.firstPackage()), mode == 1);
        }
        QCOMPARE(ready.size(), 3); QVERIFY(failed.isEmpty());
        job.cancel(); QVERIFY(!job.package()); QVERIFY(!job.firstPackage());
    }
    void invalidFirstSelectionRefusedBeforeNetwork()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        const auto selection = firstRelease(fixture);
        for (const QString& url : {QStringLiteral("http://example.invalid/file"), QStringLiteral("file:///tmp/file"),
                                  QStringLiteral("https://user:secret@example.invalid/file"), QStringLiteral("https://example.invalid/file?key=x"),
                                  QStringLiteral("https://example.invalid/file#fragment")}) {
            auto bad = selection; bad.urls[0] = QUrl(url); QVERIFY(!job.start(bad));
        }
        auto bad = selection; bad.profile = 2; QVERIFY(!job.start(bad));
        bad = selection; bad.trust.publisherKey.clear(); QVERIFY(!job.start(bad));
        bad = selection; bad.trust.files[2].size = 513 * 1024; QVERIFY(!job.start(bad));
        bad = selection; bad.version.clear(); QVERIFY(!job.start(bad));
        bad = selection; bad.firmware = "unknown"; QVERIFY(!job.start(bad));
        QVERIFY(network.sent.empty());
    }
    void corruptFirstFileStopsBeforeNextRequest_data()
    {
        QTest::addColumn<int>("index");
        for (int i = 0; i < 8; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
    }
    void corruptFirstFileStopsBeforeNextRequest()
    {
        QFETCH(int, index);
        AetherPDTest::PackageFixture fixture;
        const auto files = fixture.firstFiles();
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        QVERIFY(job.start(firstRelease(fixture)));
        for (int i = 0; i < index; ++i) { network.last->complete(200, files[i]); }
        QByteArray corrupt = files[index]; corrupt[0] ^= 1;
        network.last->complete(200, corrupt);
        QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Integrity);
        QVERIFY(!job.firstPackage()); QVERIFY(!job.package()); QCOMPARE(network.sent.size(), std::size_t(index + 1));
    }
    void firstPackageSignatureAndTargetMustMatchIndependentSelection()
    {
        AetherPDTest::PackageFixture fixture;
        const auto files = fixture.firstFiles();
        for (int mode = 0; mode < 3; ++mode) {
            AetherPDTest::Network network; AetherPDPackageDownload job(network);
            QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
            auto selection = firstRelease(fixture);
            if (mode == 0) { selection.trust.publisherKey = QByteArray(32, 'x'); }
            else if (mode == 1) { selection.firmware = "4.0.0.0"; }
            else {
                // An upgrade worker signed by the same key is not a fresh-install worker.
                const QByteArray upgrade = fixture.bytes(0);
                selection.trust.files[0] = {upgrade.size(), QCryptographicHash::hash(upgrade, QCryptographicHash::Sha256)};
            }
            QVERIFY(job.start(selection));
            for (std::size_t i = 0; i < files.size(); ++i) {
                network.last->complete(200, mode == 2 && i == 0 ? fixture.bytes(0) : files[i]);
            }
            QCOMPARE(failed.size(), 1); QVERIFY(!job.firstPackage()); QVERIFY(!job.package());
            QCOMPARE(failed[0][0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Integrity);
        }
    }
    void firstTransportFailuresPublishNoPackage()
    {
        AetherPDTest::PackageFixture fixture;
        const auto files = fixture.firstFiles();
        for (int mode = 0; mode < 6; ++mode) {
            AetherPDTest::Network network; AetherPDPackageDownload job(network);
            QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
            QVERIFY(job.start(firstRelease(fixture)));
            if (mode == 0) { network.last->announceLength(files[0].size() + 1); }
            if (mode == 1) { network.last->complete(200, files[0] + 'x'); }
            if (mode == 2) { network.last->complete(200, files[0].first(5)); }
            if (mode == 3) { network.last->complete(404, {}); }
            if (mode == 4) { network.last->complete(200, {}, QNetworkReply::SslHandshakeFailedError); }
            if (mode == 5) { QVERIFY(QMetaObject::invokeMethod(job.findChild<QTimer*>(), "timeout", Qt::DirectConnection)); }
            QCOMPARE(failed.size(), 1); QVERIFY(!job.active()); QVERIFY(!job.firstPackage()); QVERIFY(!job.package());
            QCOMPARE(network.sent.size(), std::size_t(1));
        }
    }
    void cancelUpgradeAndRestartFirstFromProgressIgnoresOldCompletion()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy ready(&job, &AetherPDPackageDownload::ready);
        bool restarted = false;
        connect(&job, &AetherPDPackageDownload::progress, &job, [&] {
            if (!restarted) { restarted = true; job.cancel(); QVERIFY(job.start(firstRelease(fixture))); }
        });
        QVERIFY(job.start(release(fixture)));
        const QPointer<AetherPDTest::Reply> old = network.last;
        old->complete(200, fixture.bytes(0)); QVERIFY(old->aborted); QVERIFY(ready.isEmpty());
        for (const auto& file : fixture.firstFiles()) { network.last->complete(200, file); }
        QCOMPARE(ready.size(), 1); QVERIFY(job.firstPackage()); QVERIFY(!job.package());
    }
    void completePinnedRelease_data()
    {
        QTest::addColumn<int>("arch");
        QTest::newRow("arm64") << 64; QTest::newRow("armv7") << 7;
    }
    void completePinnedRelease()
    {
        QFETCH(int, arch);
        AetherPDTest::PackageFixture fixture(arch);
        AetherPDTest::Network network;
        AetherPDPackageDownload job(network);
        QSignalSpy ready(&job, &AetherPDPackageDownload::ready);
        QSignalSpy progress(&job, &AetherPDPackageDownload::progress);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        const auto selection = release(fixture, arch);
        QVERIFY(job.start(selection)); QVERIFY(!job.start(selection));
        for (std::size_t i = 0; i < 8; ++i) {
            QCOMPARE(network.sent.size(), i + 1);
            QCOMPARE(network.sent.back().method, QNetworkAccessManager::GetOperation);
            const auto& request = network.sent.back().request;
            QCOMPARE(request.url(), selection.urls[i]);
            QVERIFY(!request.hasRawHeader("Authorization"));
            QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
            QCOMPARE(request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), int(QNetworkRequest::Manual));
            QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::NoLessSafeRedirectPolicy));
            QCOMPARE(request.maximumRedirectsAllowed(), 5);
            network.last->complete(200, fixture.bytes(i));
        }
        QCOMPARE(ready.size(), 1); QVERIFY(failed.isEmpty()); QVERIFY(!job.active());
        QVERIFY(job.package()); QCOMPARE(job.package()->profile(), selection.profile);
        QCOMPARE(progress.last()[0], progress.last()[1]);
        const auto retained = job.package();
        job.cancel(); QVERIFY(!job.package()); QVERIFY(retained->plan("fixture-8600").size() == 352);
    }
    void invalidCatalogueRefusedBeforeNetwork()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        const auto selection = release(fixture);
        for (const QString& url : {QStringLiteral("http://example.invalid/file"), QStringLiteral("file:///tmp/file"),
                                  QStringLiteral("https://user:secret@example.invalid/file"), QStringLiteral("https://example.invalid/file?key=x"),
                                  QStringLiteral("https://example.invalid/file#fragment")}) {
            auto bad = selection; bad.urls[0] = QUrl(url); QVERIFY(!job.start(bad));
        }
        auto bad = selection; bad.trust.files[1].size = 100 * 1024 * 1024; QVERIFY(!job.start(bad));
        bad = selection; bad.trust.previousKey.clear(); QVERIFY(!job.start(bad));
        bad = selection; bad.profile = 0; QVERIFY(!job.start(bad));
        bad = selection; bad.firmware = "unknown"; QVERIFY(!job.start(bad));
        bad = selection; bad.version.clear(); QVERIFY(!job.start(bad));
        QVERIFY(network.sent.empty());
    }
    void corruptFileStopsBeforeNextRequest_data()
    {
        QTest::addColumn<int>("index");
        for (int i = 0; i < 8; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
    }
    void corruptFileStopsBeforeNextRequest()
    {
        QFETCH(int, index);
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        QVERIFY(job.start(release(fixture)));
        for (int i = 0; i < index; ++i) { network.last->complete(200, fixture.bytes(i)); }
        QByteArray bytes = fixture.bytes(index); bytes[0] ^= 1;
        network.last->complete(200, bytes);
        QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Integrity);
        QVERIFY(!job.active()); QVERIFY(!job.package()); QCOMPARE(network.sent.size(), std::size_t(index + 1));
    }
    void announcedAndUnannouncedOversize()
    {
        for (bool announced : {true, false}) {
            AetherPDTest::PackageFixture fixture;
            AetherPDTest::Network network; AetherPDPackageDownload job(network);
            QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
            QVERIFY(job.start(release(fixture)));
            if (announced) { network.last->announceLength(fixture.bytes(0).size() + 1); }
            else { network.last->complete(200, fixture.bytes(0) + 'x'); }
            QCOMPARE(failed.size(), 1); QCOMPARE(failed[0][0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Oversize);
            QVERIFY(network.last->aborted); QVERIFY(!job.package()); QCOMPARE(network.sent.size(), std::size_t(1));
        }
    }
    void truncatedAndNetworkFailures()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        QVERIFY(job.start(release(fixture))); network.last->complete(200, fixture.bytes(0).first(5));
        QCOMPARE(failed.last()[0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Integrity);
        QVERIFY(job.start(release(fixture))); network.last->complete(404, {});
        QCOMPARE(failed.last()[0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Network);
        QVERIFY(job.start(release(fixture))); network.last->complete(200, {}, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(failed.last()[0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Network);
    }
    void signedMetadataMustMatchSelection()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        auto selection = release(fixture); selection.firmware = "4.0.0.0";
        QVERIFY(job.start(selection));
        for (int i = 0; i < 8; ++i) { network.last->complete(200, fixture.bytes(i)); }
        QCOMPARE(failed.size(), 1); QVERIFY(!job.package());
    }
    void cancelAndRestartFromProgressIgnoresOldCompletion()
    {
        AetherPDTest::PackageFixture fixture;
        AetherPDTest::Network network; AetherPDPackageDownload job(network);
        QSignalSpy ready(&job, &AetherPDPackageDownload::ready);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        bool restarted = false;
        connect(&job, &AetherPDPackageDownload::progress, &job, [&] {
            if (!restarted) { restarted = true; job.cancel(); QVERIFY(job.start(release(fixture))); }
        });
        QVERIFY(job.start(release(fixture)));
        const QPointer<AetherPDTest::Reply> old = network.last;
        old->complete(200, fixture.bytes(0)); QVERIFY(restarted); QVERIFY(old->aborted);
        QVERIFY(network.last != old); QVERIFY(job.active()); QVERIFY(ready.isEmpty()); QVERIFY(failed.isEmpty());
        QCOMPARE(network.sent.size(), std::size_t(2));
        for (int i = 0; i < 8; ++i) { network.last->complete(200, fixture.bytes(i)); }
        QCOMPARE(ready.size(), 1); QVERIFY(failed.isEmpty());
    }
    void deadlineAndManagerLoss()
    {
        AetherPDTest::PackageFixture fixture;
        auto network = std::make_unique<AetherPDTest::Network>();
        AetherPDPackageDownload job(*network);
        QSignalSpy failed(&job, &AetherPDPackageDownload::failed);
        QVERIFY(job.start(release(fixture)));
        QTimer* deadline = job.findChild<QTimer*>(); QVERIFY(deadline);
        QVERIFY(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        QCOMPARE(failed.last()[0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Timeout);
        QVERIFY(job.start(release(fixture))); network.reset();
        QCOMPARE(failed.last()[0].value<AetherPDPackageDownload::Failure>(), AetherPDPackageDownload::Failure::Network);
        QVERIFY(!job.active()); QVERIFY(!job.start(release(fixture)));
    }
    void deletionFromSignals()
    {
        for (int phase = 0; phase < 3; ++phase) {
            AetherPDTest::PackageFixture fixture;
            AetherPDTest::Network network;
            auto job = std::make_unique<AetherPDPackageDownload>(network);
            if (phase == 0) { connect(job.get(), &AetherPDPackageDownload::progress, &network, [&] { job.reset(); }); }
            if (phase == 1) { connect(job.get(), &AetherPDPackageDownload::ready, &network, [&] { job.reset(); }); }
            if (phase == 2) { connect(job.get(), &AetherPDPackageDownload::failed, &network, [&] { job.reset(); }); }
            QVERIFY(job->start(release(fixture)));
            for (int i = 0; i < 8 && job; ++i) { network.last->complete(phase == 2 ? 404 : 200, fixture.bytes(i)); }
            QVERIFY(!job);
        }
    }
};
QTEST_GUILESS_MAIN(DownloadTest)
#include "aetherpd_package_download_test.moc"
