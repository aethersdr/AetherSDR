#include "core/TciProtocol.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TciProtocol protocol(nullptr);

    const QString response = protocol.handleCommand(
        QStringLiteral("iq_samplerate:44100"));
    if (response != QStringLiteral("iq_samplerate:48000;")) {
        std::fprintf(stderr,
                     "unsupported iq_samplerate should report the current rate; got %s\n",
                     response.toUtf8().constData());
        return 1;
    }

    if (!protocol.pendingNotification().isEmpty()) {
        std::fprintf(stderr,
                     "rejected iq_samplerate must not notify other clients\n");
        return 1;
    }

    const QStringList greeting = protocol.generateInitBurst().split(
        QLatin1Char(';'), Qt::SkipEmptyParts);
    const int readyIndex = greeting.indexOf(QStringLiteral("ready"));
    const int iqRateIndex = greeting.indexOf(QStringLiteral("iq_samplerate:48000"));
    const int startIndex = greeting.indexOf(QStringLiteral("start"));
    if (readyIndex < 0 || iqRateIndex < 0 || startIndex < 0
        || iqRateIndex >= readyIndex || readyIndex >= startIndex) {
        std::fprintf(stderr,
                     "TCI greeting must order iq_samplerate before ready before start\n");
        return 1;
    }

    for (const QString& command : greeting) {
        if (command.startsWith(QStringLiteral("audio_start"))
            || command.startsWith(QStringLiteral("iq_start"))) {
            std::fprintf(stderr,
                         "TCI greeting must not emit client-owned stream command: %s\n",
                         command.toUtf8().constData());
            return 1;
        }
    }

    std::printf("tci_protocol_test: all checks passed\n");
    return 0;
}
