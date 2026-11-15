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

    std::printf(g_failures == 0 ? "\nASR model manager: ALL PASS\n"
                                : "\nASR model manager: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
