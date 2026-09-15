#include "core/deepfist/DeepFistModelAssets.h"
#include "DeepFistDownloadTransport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <cstdio>
using namespace AetherSDR;
namespace {
int failures = 0;
void expect(bool good, const char* name)
{
    std::fprintf(stderr, "%s %s\n", good ? "PASS" : "FAIL", name);
    if (!good) { ++failures; }
}
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
bool wait(const std::function<bool()>& done)
{
    QElapsedTimer time; time.start();
    while (!done() && time.elapsed() < 5000) { pump(5); }
    return done();
}
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) { file.write(bytes); }
}
QVector<DeepFistModelAssets::Asset> catalog(DeepFistTestNetwork& network)
{
    QVector<DeepFistModelAssets::Asset> result;
    for (const QString& name : {QStringLiteral("model"), QStringLiteral("metadata"), QStringLiteral("license")}) {
        const QByteArray bytes = name.toUtf8().repeated(20000);
        network.files[name] = {bytes};
        result.append({name, bytes.size(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()});
    }
    return result;
}
void successAndCancel()
{
    QTemporaryDir dir;
    DeepFistTestNetwork network;
    const auto assets = catalog(network);
    DeepFistModelAssets manager(dir.path(), "https://fixture.invalid/v1", assets, &network);
    int ready = 0, failed = 0;
    QObject::connect(&manager, &DeepFistModelAssets::ready, &manager, [&] { ++ready; });
    QObject::connect(&manager, &DeepFistModelAssets::failed, &manager, [&] { ++failed; });
    manager.ensure(); manager.cancel(); pump(50);
    expect(ready == 0 && failed == 0 && network.requests == 0, "cancel verification prevents downloads and completion");
    const auto cancel = QObject::connect(&manager, &DeepFistModelAssets::progress, &manager,
        [&](qint64 got, qint64) { if (got > 0) { manager.cancel(); } });
    manager.ensure(); expect(wait([&] { return !manager.busy(); }), "transfer cancellation completes");
    pump(20);
    expect(ready == 0 && failed == 0 && !QFile::exists(dir.filePath("model")), "canceled transfer never commits");
    expect(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(), "temporary files removed");
    QObject::disconnect(cancel);
    manager.ensure(); expect(wait([&] { return ready == 1 || failed; }) && ready == 1 && !failed, "retry downloads full bundle");
    for (const auto& asset : assets) { expect(read(dir.filePath(asset.name)) == network.files[asset.name].bytes, "installed exact asset bytes"); }
    const int requests = network.requests;
    manager.ensure(); expect(wait([&] { return ready == 2 || failed; }) && ready == 2, "offline cache ready");
    expect(network.requests == requests, "verified cache uses no network");
    write(dir.filePath("metadata"), "bad cache");
    manager.ensure(); expect(wait([&] { return ready == 3 || failed; }) && ready == 3, "repair corrupt cached asset");
    expect(network.requests == requests + 1, "repair downloads only invalid asset");
}
void rejection(int variant)
{
    QTemporaryDir dir;
    DeepFistTestNetwork network;
    const auto assets = catalog(network);
    auto& reply = network.files["model"];
    if (variant == 0) { reply.bytes[0] ^= 1; }
    if (variant == 1) { reply.bytes.append('!'); }
    if (variant == 2) { reply.bytes.chop(1); }
    if (variant == 3) { reply.status = 404; }
    if (variant == 4) { reply.error = QNetworkReply::RemoteHostClosedError; }
    write(dir.filePath("model"), "previous bytes");
    DeepFistModelAssets manager(dir.path(), "https://fixture.invalid/v1", assets, &network);
    bool ready = false, failed = false;
    QObject::connect(&manager, &DeepFistModelAssets::ready, &manager, [&] { ready = true; });
    QObject::connect(&manager, &DeepFistModelAssets::failed, &manager, [&](const QString&) { failed = true; });
    manager.ensure(); expect(wait([&] { return failed || ready; }) && failed && !ready, "reject invalid or incomplete HTTP transfer");
    expect(read(dir.filePath("model")) == "previous bytes", "failed replacement preserves existing file");
    expect(network.requests == 1, "failed asset prevents remainder of bundle");
}
void lifecycle()
{
    QTemporaryDir dir;
    DeepFistTestNetwork network;
    const auto assets = catalog(network);
    DeepFistModelAssets first(dir.path(), "https://fixture.invalid/v1", assets, &network);
    DeepFistModelAssets second(dir.path(), "https://fixture.invalid/v1", assets, &network);
    bool locked = false;
    QObject::connect(&second, &DeepFistModelAssets::failed, &second, [&](const QString&) { locked = true; });
    first.ensure(); second.ensure();
    expect(locked, "cache lock prevents concurrent writers");
    first.cancel();
    { DeepFistModelAssets transient(dir.path(), "https://fixture.invalid/v1", assets, &network); transient.ensure(); }
    pump(50);
    expect(network.requests == 0, "destroy while checking cannot start network work");
    DeepFistModelAssets unavailable(dir.path(), {}, assets, &network);
    bool failed = false;
    QObject::connect(&unavailable, &DeepFistModelAssets::failed, &unavailable, [&](const QString&) { failed = true; });
    unavailable.ensure(); expect(wait([&] { return failed; }), "unpublished source fails explicitly");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    successAndCancel();
    for (int i = 0; i < 5; ++i) { rejection(i); }
    lifecycle();
    return failures ? 1 : 0;
}
