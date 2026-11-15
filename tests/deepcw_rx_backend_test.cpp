#include "models/DeepCwRxBackend.h"
#include "DeepFistDownloadTransport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>

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
}

// Model preparation lifecycle on injected HTTP replies: no sockets, weights or
// inference. Every request answers 404, so the model never becomes available.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    DeepFistTestNetwork network;
    DeepCwRxBackend backend(dir.path(), QStringLiteral("https://fixture.invalid/v1"), &network);

    backend.start();
    expect(wait([&] { return backend.status().contains("unavailable"); }), "missing model reports unavailable");
    expect(backend.canRetry() && !backend.preparing() && backend.isRunning(), "failed download offers Retry");
    const int before = network.requests;
    backend.retry();
    expect(wait([&] { return network.requests > before && backend.canRetry(); }), "Retry asks the source again");
    backend.stop();
    expect(!backend.canRetry() && backend.status().isEmpty() && backend.detail().isEmpty() && !backend.isRunning(),
           "stop clears Retry, detail and status");

    backend.start();
    backend.cancelPreparation();
    expect(backend.status().contains("canceled") && backend.canRetry() && !backend.preparing(),
           "cancel during preparation offers Retry");
    backend.stop();
    expect(!backend.canRetry() && backend.status().isEmpty(), "stop after cancel clears Retry");
    pump(50);
    return failures ? 1 : 0;
}
