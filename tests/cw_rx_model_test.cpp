#include "models/CwRxModel.h"
#include <QCoreApplication>
#include <iostream>
#include <QPointer>
#include <QTemporaryDir>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir cache;
    qputenv("AETHER_DEEPFIST_MODEL_DIR", cache.path().toUtf8());
    CwRxModel decoder;
    auto require = [](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; }
        return condition;
    };
    if (!require(decoder.backendKey() == "ggmorse" && decoder.supportsTuning()
                 && !decoder.isRunning(), "Default RX backend changed")) { return 1; }
    decoder.start();
    if (!require(decoder.isRunning(), "ggmorse did not start")) { return 1; }
    if (!require(!decoder.selectBackend("unknown") && decoder.isRunning()
                 && decoder.backendKey() == "ggmorse", "Invalid selection interrupted RX")) { return 1; }
    decoder.reset();
    if (!require(decoder.isRunning(), "Reset disabled RX")) { return 1; }
    decoder.stop();
    decoder.reset();
    if (!require(!decoder.isRunning(), "Reset enabled stopped RX")) { return 1; }
#ifdef HAVE_DEEPFIST
    if (!require(decoder.selectBackend("deepfist") && !decoder.supportsTuning()
                 && !decoder.isRunning(), "DeepFist selection failed")) { return 1; }
    if (!require(decoder.selectBackend("ggmorse") && decoder.supportsTuning(),
                 "Cannot return to ggmorse")) { return 1; }
#else
    if (!require(!decoder.selectBackend("deepfist")
                 && CwRxModel::availableBackends() == QStringList{"ggmorse"},
                 "Unavailable backend advertised")) { return 1; }
#endif
    int neutral = 0;
    QObject::connect(&decoder, &CwRxModel::statsUpdated, &app,
                     [&](float pitch, float speed) { if (pitch == 0 && speed == 0) { ++neutral; } });
    decoder.start();
    decoder.stop();
    QCoreApplication::processEvents();
    if (!require(neutral > 0, "Stop did not publish neutral RX readings")) { return 1; }

    // A state observer can replace the operation or destroy its facade. The
    // superseded callback must not continue publishing a second notification.
    auto disposable = std::make_unique<CwRxModel>();
    int statuses = 0;
    QObject::connect(disposable.get(), &CwRxModel::statusChanged, &app, [&] { ++statuses; });
    QObject::connect(disposable.get(), &CwRxModel::statsUpdated, &app,
                     [&](float, float) { disposable.reset(); });
    disposable->reset();
    QCoreApplication::processEvents();
    if (!require(!disposable && statuses == 0, "State notification survived facade deletion")) { return 1; }
#ifdef HAVE_DEEPFIST
    CwRxModel nested;
    bool replaced = false;
    QObject::connect(&nested, &CwRxModel::statsUpdated, &app, [&](float, float) {
        if (!replaced) { replaced = true; nested.selectBackend("ggmorse"); }
    });
    QObject::connect(&nested, &CwRxModel::statusChanged, &app, [&] {
        if (nested.backendKey() != "ggmorse") { ++statuses; }
    });
    nested.selectBackend("deepfist");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    if (!require(replaced && nested.backendKey() == "ggmorse" && statuses == 0,
                 "Superseded backend state continued publishing")) { return 1; }
#endif
    return require(!decoder.isRunning(), "Stopped decoder restarted") ? 0 : 1;
}
