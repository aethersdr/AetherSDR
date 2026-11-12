// Offline unit test for the ASR model manager (RFC #4333, Phase 2).
//
// Uses file:// sources served through the real QNetworkAccessManager path, so
// the streaming-to-disk, SHA-256 verification, atomic rename, and source
// failover logic are all exercised without any network. Covers: catalog
// integrity, cache-hit, successful download+verify+rename, failover across a
// missing source, hash-mismatch rejection, failover after a corrupt mirror,
// and all-sources-failed.

#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QByteArray sha256Hex(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    f.write(bytes);
    f.close();
}

QString fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path).toString();
}

enum class Outcome { None, AlreadyPresent, Finished, Failed };

struct Result {
    Outcome outcome = Outcome::None;
    QString path;
    QString error;
};

// Drives one ensure() to completion on the event loop, capturing the terminal
// signal (works whether it is emitted synchronously or asynchronously).
Result runEnsure(AsrModelManager& mgr, const AsrModelTier& tier)
{
    Result r;
    QEventLoop loop;
    QObject::connect(&mgr, &AsrModelManager::alreadyPresent, &loop,
                     [&](const QString& p) { r.outcome = Outcome::AlreadyPresent; r.path = p; loop.quit(); });
    QObject::connect(&mgr, &AsrModelManager::finished, &loop,
                     [&](const QString& p) { r.outcome = Outcome::Finished; r.path = p; loop.quit(); });
    QObject::connect(&mgr, &AsrModelManager::failed, &loop,
                     [&](const QString& e) { r.outcome = Outcome::Failed; r.error = e; loop.quit(); });
    QTimer::singleShot(0, &mgr, [&] { mgr.ensure(tier); });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit); // safety timeout
    loop.exec();
    return r;
}

struct BundleResult : Result {
    int finishedCount = 0;
    int alreadyCount = 0;
    int failedCount = 0;
    QVector<QPair<qint64, qint64>> progress;
};

// Drives one ensureBundle() to completion, counting terminal signals and
// recording every progress() emission. Waits a beat after the first terminal
// signal so a stray second one would be counted.
BundleResult runBundle(AsrModelManager& mgr, const QVector<AsrModelTier>& files)
{
    BundleResult r;
    QEventLoop loop;
    QObject::connect(&mgr, &AsrModelManager::alreadyPresent, &loop, [&](const QString& p) {
        r.outcome = Outcome::AlreadyPresent; r.path = p; ++r.alreadyCount; });
    QObject::connect(&mgr, &AsrModelManager::finished, &loop, [&](const QString& p) {
        r.outcome = Outcome::Finished; r.path = p; ++r.finishedCount; });
    QObject::connect(&mgr, &AsrModelManager::failed, &loop, [&](const QString& e) {
        r.outcome = Outcome::Failed; r.error = e; ++r.failedCount; });
    QObject::connect(&mgr, &AsrModelManager::progress, &loop, [&](qint64 got, qint64 total) {
        r.progress.append({got, total}); });
    const auto terminal = [&] {
        QTimer::singleShot(100, &loop, &QEventLoop::quit);
    };
    QObject::connect(&mgr, &AsrModelManager::alreadyPresent, &loop, terminal);
    QObject::connect(&mgr, &AsrModelManager::finished, &loop, terminal);
    QObject::connect(&mgr, &AsrModelManager::failed, &loop, terminal);
    QTimer::singleShot(0, &mgr, [&] { mgr.ensureBundle(files); });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit); // safety timeout
    loop.exec();
    QObject::disconnect(&mgr, nullptr, &loop, nullptr);
    return r;
}

AsrModelTier makeTier(const QString& fileName, const QByteArray& expectBytes,
                      const QStringList& sources)
{
    AsrModelTier t;
    t.id = QStringLiteral("test");
    t.displayName = QStringLiteral("test");
    t.fileName = fileName;
    t.sizeBytes = expectBytes.size();
    t.sha256 = QString::fromLatin1(sha256Hex(expectBytes));
    t.sources = sources;
    return t;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Catalog integrity ------------------------------------------------
    const auto& tiers = AsrModelCatalog::tiers();
    expect(!tiers.isEmpty(), "catalog exposes at least one tier");
    expect(AsrModelCatalog::tierById(AsrModelCatalog::defaultTierId()) != nullptr,
           "defaultTierId() resolves to a known tier");
    bool allWellFormed = true;
    for (const AsrModelTier& t : tiers) {
        if (t.sha256.size() != 64 || t.sizeBytes <= 0 || t.sources.isEmpty()
            || t.fileName.isEmpty()) {
            allWellFormed = false;
        }
    }
    expect(allWellFormed, "every tier has a 64-hex sha, size, filename, and >=1 source");
    expect(AsrModelCatalog::tierById(QStringLiteral("nope")) == nullptr,
           "tierById() returns nullptr for an unknown id");

    // ---- Fixtures ---------------------------------------------------------
    QTemporaryDir cacheRoot;
    QTemporaryDir srcRoot;
    expect(cacheRoot.isValid() && srcRoot.isValid(), "temp dirs created");

    const QByteArray goodBytes = QByteArray("AetherSDR ASR model fixture payload").repeated(64);
    QByteArray badBytes = goodBytes;
    badBytes[0] = badBytes[0] ^ 0xFF; // one flipped byte -> different sha, same size

    const QString goodSrc = srcRoot.filePath(QStringLiteral("good.bin"));
    const QString badSrc = srcRoot.filePath(QStringLiteral("bad.bin"));
    const QString missingSrc = srcRoot.filePath(QStringLiteral("missing.bin"));
    writeFile(goodSrc, goodBytes);
    writeFile(badSrc, badBytes);

    AsrModelManager mgr(cacheRoot.path(), nullptr);

    // ---- Cache hit: pre-placed valid file, no download --------------------
    {
        const AsrModelTier tier = makeTier(QStringLiteral("hit.bin"), goodBytes,
                                           {fileUrl(missingSrc)}); // source is unreachable
        writeFile(mgr.modelPath(tier), goodBytes);                 // but file already valid
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::AlreadyPresent, "cache hit -> alreadyPresent (no download)");
        expect(r.path == mgr.modelPath(tier), "alreadyPresent reports the cached path");
    }

    // ---- Successful download + verify + atomic rename ---------------------
    {
        const AsrModelTier tier = makeTier(QStringLiteral("dl.bin"), goodBytes,
                                           {fileUrl(goodSrc)});
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::Finished, "good source -> finished");
        expect(mgr.verify(tier, nullptr), "downloaded file passes SHA-256 verify");
        expect(!QFileInfo::exists(mgr.modelPath(tier) + QStringLiteral(".part")),
               "no .part left behind after success");
    }

    // ---- Failover: first source missing, second good ----------------------
    {
        const AsrModelTier tier = makeTier(QStringLiteral("failover.bin"), goodBytes,
                                           {fileUrl(missingSrc), fileUrl(goodSrc)});
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::Finished, "missing primary -> falls over to good source");
    }

    // ---- Hash mismatch is rejected ----------------------------------------
    {
        // Tier expects goodBytes' sha, but the only source serves badBytes.
        AsrModelTier tier = makeTier(QStringLiteral("mismatch.bin"), goodBytes,
                                     {fileUrl(badSrc)});
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::Failed, "sha mismatch -> failed");
        expect(!QFileInfo::exists(mgr.modelPath(tier)), "mismatched file is not cached");
        expect(!QFileInfo::exists(mgr.modelPath(tier) + QStringLiteral(".part")),
               "mismatched .part is removed");
    }

    // ---- Failover after a corrupt mirror ----------------------------------
    {
        const AsrModelTier tier = makeTier(QStringLiteral("corruptmirror.bin"), goodBytes,
                                           {fileUrl(badSrc), fileUrl(goodSrc)});
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::Finished, "corrupt primary -> retries next source -> finished");
    }

    // ---- All sources fail --------------------------------------------------
    {
        const AsrModelTier tier = makeTier(QStringLiteral("allfail.bin"), goodBytes,
                                           {fileUrl(missingSrc), fileUrl(missingSrc)});
        const Result r = runEnsure(mgr, tier);
        expect(r.outcome == Outcome::Failed, "all sources missing -> failed");
    }

    // ---- Bundles ------------------------------------------------------------
    {
        const QByteArray b0 = QByteArray("bundle file zero ").repeated(300);
        const QByteArray b1 = QByteArray("bundle file one, longer ").repeated(500);
        const QByteArray b2 = QByteArray("two ").repeated(40);
        const qint64 sum = b0.size() + b1.size() + b2.size();
        const QStringList names = {QStringLiteral("a.onnx"), QStringLiteral("b.onnx"),
                                   QStringLiteral("c.txt")};
        const QList<QByteArray> bytes = {b0, b1, b2};
        QStringList srcs;
        for (int i = 0; i < 3; ++i) {
            srcs << srcRoot.filePath(QStringLiteral("bundle-%1").arg(names.at(i)));
            writeFile(srcs.last(), bytes.at(i));
        }
        const auto bundleFor = [&](const QString& folder) {
            QVector<AsrModelTier> v;
            for (int i = 0; i < 3; ++i) {
                v.append(makeTier(folder + QLatin1Char('/') + names.at(i), bytes.at(i),
                                  {fileUrl(srcs.at(i))}));
            }
            return v;
        };
        const auto allPresent = [&](const QVector<AsrModelTier>& v) {
            for (int i = 0; i < 3; ++i) {
                QFile f(mgr.modelPath(v.at(i)));
                if (!f.open(QIODevice::ReadOnly) || f.readAll() != bytes.at(i)) {
                    return false;
                }
            }
            return true;
        };
        const auto noPartFiles = [&](const QVector<AsrModelTier>& v) {
            for (const AsrModelTier& t : v) {
                if (QFileInfo::exists(mgr.modelPath(t) + QStringLiteral(".part"))) {
                    return false;
                }
            }
            return true;
        };

        // a) everything fetched
        const QVector<AsrModelTier> packA = bundleFor(QStringLiteral("pack"));
        expect(!mgr.isBundlePresent(packA), "bundle not present before fetch");
        const BundleResult a = runBundle(mgr, packA);
        expect(a.finishedCount == 1 && a.failedCount == 0 && a.alreadyCount == 0,
               "bundle fetched -> finished once, no alreadyPresent/failed");
        expect(a.path == cacheRoot.filePath(QStringLiteral("pack"))
                   && a.path == mgr.bundlePath(packA),
               "finished reports the absolute bundle folder");
        expect(allPresent(packA), "all bundle files cached with the right bytes");
        expect(mgr.isBundlePresent(packA), "isBundlePresent after fetch");
        expect(noPartFiles(packA), "no .part left behind by the bundle");
        bool totalConstant = !a.progress.isEmpty();
        for (const auto& pr : a.progress) {
            totalConstant = totalConstant && pr.second == sum;
        }
        expect(totalConstant, "bundle progress total is the sum of file sizes throughout");
        expect(!a.progress.isEmpty() && a.progress.last() == qMakePair(sum, sum),
               "bundle progress ends at (sum, sum)");
        bool monotonic = true;
        for (int i = 1; i < a.progress.size(); ++i) {
            monotonic = monotonic && a.progress.at(i).first >= a.progress.at(i - 1).first;
        }
        expect(monotonic, "bundle progress never goes backwards");
        expect(!mgr.isBusy(), "manager idle after a bundle");

        // b) again, sources gone: alreadyPresent, nothing fetched
        for (const QString& src : srcs) {
            QFile::remove(src);
        }
        const BundleResult b = runBundle(mgr, packA);
        expect(b.alreadyCount == 1 && b.finishedCount == 0 && b.failedCount == 0,
               "cached bundle -> alreadyPresent once, no finished");
        expect(b.path == mgr.bundlePath(packA), "alreadyPresent reports the bundle folder");
        expect(b.progress.isEmpty(), "a cached bundle reports no download progress");

        // c) one cached, two missing: finished, the cached one is not re-fetched
        const QVector<AsrModelTier> packC = bundleFor(QStringLiteral("packc"));
        writeFile(mgr.modelPath(packC.at(1)), b1);     // middle one cached ...
        writeFile(srcs.at(0), b0);                     // ... its source is gone
        writeFile(srcs.at(2), b2);
        const BundleResult c = runBundle(mgr, packC);
        expect(c.finishedCount == 1 && c.alreadyCount == 0 && c.failedCount == 0,
               "partly cached bundle -> finished (not alreadyPresent)");
        expect(allPresent(packC), "partly cached bundle ends complete");

        // d) middle file serves wrong bytes: stop there
        writeFile(srcs.at(1), badBytes.left(b1.size()).leftJustified(b1.size(), 'x'));
        const QVector<AsrModelTier> packD = bundleFor(QStringLiteral("packd"));
        const BundleResult d = runBundle(mgr, packD);
        expect(d.failedCount == 1 && d.finishedCount == 0 && d.alreadyCount == 0,
               "bad middle file -> failed once");
        expect(QFileInfo::exists(mgr.modelPath(packD.at(0))), "first file stays cached");
        expect(!QFileInfo::exists(mgr.modelPath(packD.at(1))), "bad file is not cached");
        expect(!QFileInfo::exists(mgr.modelPath(packD.at(2)))
                   && !QFileInfo::exists(mgr.modelPath(packD.at(2)) + QStringLiteral(".part")),
               "third file was never fetched");
        expect(noPartFiles(packD), "failed bundle leaves no .part");
        expect(!mgr.isBusy(), "manager idle after a failed bundle");
        const AsrModelTier after = makeTier(QStringLiteral("after-bundle.bin"), goodBytes,
                                            {fileUrl(goodSrc)});
        expect(runEnsure(mgr, after).outcome == Outcome::Finished,
               "single-file ensure() works after a failed bundle");

        // e) files in two folders
        QVector<AsrModelTier> packE = bundleFor(QStringLiteral("packe"));
        packE[2].fileName = QStringLiteral("other/") + names.at(2);
        const BundleResult e = runBundle(mgr, packE);
        expect(e.failedCount == 1 && e.finishedCount == 0 && e.alreadyCount == 0,
               "files in different folders -> failed");
        expect(!QFileInfo::exists(mgr.modelPath(packE.at(0))), "nothing fetched for split bundle");
        expect(runBundle(mgr, {}).failedCount == 1, "empty bundle -> failed");

        // f) right size, corrupt content: re-fetched
        const QVector<AsrModelTier> packF = bundleFor(QStringLiteral("packf"));
        writeFile(srcs.at(1), b1); // restore the good source
        writeFile(mgr.modelPath(packF.at(0)), b0);
        writeFile(mgr.modelPath(packF.at(1)), QByteArray(b1.size(), 'z')); // same size, wrong
        writeFile(mgr.modelPath(packF.at(2)), b2);
        expect(mgr.isBundlePresent(packF), "corrupt bundle passes the size-only check");
        const BundleResult f = runBundle(mgr, packF);
        expect(f.finishedCount == 1 && f.alreadyCount == 0 && f.failedCount == 0,
               "corrupt cached file -> re-fetched, finished");
        expect(allPresent(packF), "corrupt file replaced with the good bytes");
    }

    std::printf(g_failures == 0 ? "\nASR model manager: ALL PASS\n"
                                : "\nASR model manager: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
