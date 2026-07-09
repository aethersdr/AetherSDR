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

    std::printf("tci_protocol_test: all checks passed\n");
    return 0;
}
