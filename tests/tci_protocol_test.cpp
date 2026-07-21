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

    // DRIVE/TUNE_DRIVE replies must carry the TRX argument. WSJT-X and
    // JTDX are put in ESDR3 mode by our `protocol:ExpertSDR3,1.5;` greeting,
    // and their handler reads args.at(1) whenever args.at(0) equals their own
    // receiver index — so a 1-element `drive:0;` segfaults them (reproduced on
    // WSJT-X 3.0.1). The TCI v2.0 spec agrees: `DRIVE:arg1,arg2;`, arg1 being
    // the transceiver ordinal.
    // This protocol instance has a null model, so both the TRX index and the
    // power resolve to 0 and each reply is fully deterministic. Assert the
    // exact string rather than "has two arguments": an empty reply has to fail
    // too. A null-model early return added to the SET path would otherwise
    // leave the notification unset and quietly retire the half of this test
    // that guards the fan-out to other clients.
    struct { const char* cmd; const char* expected; } powerCmds[] = {
        {"drive",      "drive:0,0;"},
        {"tune_drive", "tune_drive:0,0;"},
    };
    for (const auto& pc : powerCmds) {
        // GET reply — goes back to the polling client.
        const QString getReply = protocol.handleCommand(QString::fromLatin1(pc.cmd));
        if (getReply != QString::fromLatin1(pc.expected)) {
            std::fprintf(stderr,
                         "%s GET reply must be `%s` (a one-argument reply crashes ESDR3 clients); got `%s`\n",
                         pc.cmd, pc.expected, getReply.toUtf8().constData());
            return 1;
        }
        // SET notification — the path that fans out to every other client.
        protocol.handleCommand(QStringLiteral("%1:0,0").arg(QString::fromLatin1(pc.cmd)));
        const QString note = protocol.pendingNotification();
        if (note != QString::fromLatin1(pc.expected)) {
            std::fprintf(stderr,
                         "%s notification must be `%s`; got `%s`\n",
                         pc.cmd, pc.expected, note.toUtf8().constData());
            return 1;
        }
    }

    std::printf("tci_protocol_test: all checks passed\n");
    return 0;
}
