#include "models/CwRxModel.h"
#include <QCoreApplication>
#include <iostream>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
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
    decoder.start();
    decoder.stop();
    QCoreApplication::processEvents();
    return require(!decoder.isRunning(), "Stopped decoder restarted") ? 0 : 1;
}
