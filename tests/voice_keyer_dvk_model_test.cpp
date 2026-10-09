// DvkModel as a VoiceKeyer — the radio DVK behind the keyer interface.
// Run: ./build/voice_keyer_dvk_model_test

#include "models/DvkModel.h"

#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Driven through the interface, every command keeps its exact SmartSDR
    // wire text — the split must not change what reaches the radio.
    {
        DvkModel model;
        VoiceKeyer* keyer = &model;
        QStringList sent;
        QObject::connect(&model, &DvkModel::replyCommandReady,
                         [&](const QString& cmd, const QString&, int) { sent << cmd; });
        keyer->recStart(3);
        keyer->recStop(3);
        keyer->previewStart(4);
        keyer->previewStop(4);
        keyer->playbackStart(5);
        keyer->playbackStop(5);
        keyer->clear(6);
        keyer->remove(7);
        keyer->setName(8, QStringLiteral("CQ Test"));
        // The stop verbs carry NO id: that is the radio's wire format per the
        // wiki, and dvk_model_status_test asserts it directly. The interface
        // names a slot on its stop calls because a client-side keyer's
        // operations are per-slot, but DvkModel does not put that id on the
        // wire — it reports the slot it has in flight to the reply instead.
        const QStringList expected = {
            "dvk rec_start id=3",      "dvk rec_stop",
            "dvk preview_start id=4",  "dvk preview_stop",
            "dvk playback_start id=5", "dvk playback_stop",
            "dvk clear id=6",          "dvk remove id=7",
            "dvk set_name name=\"CQ Test\" id=8",
        };
        report("commands_emit_unchanged_wire_text", sent == expected,
               sent.join(QStringLiteral(" | ")).toStdString());
    }

    // Radio status reaches a listener that only knows the interface.
    {
        DvkModel model;
        VoiceKeyer* keyer = &model;
        VoiceKeyer::Status seen = VoiceKeyer::Unknown;
        int seenId = -2;
        QList<int> changed;
        QObject::connect(keyer, &VoiceKeyer::statusChanged,
                         [&](VoiceKeyer::Status s, int id) { seen = s; seenId = id; });
        QObject::connect(keyer, &VoiceKeyer::recordingChanged,
                         [&](int id) { changed << id; });
        model.applyStatus(QStringLiteral("dvk"),
                          {{"id", "2"}, {"name", "\"CQ\""}, {"duration", "4200"}});
        model.applyStatus(QStringLiteral("dvk"),
                          {{"status", "playback"}, {"id", "2"}, {"enabled", "1"}});
        const auto& recs = keyer->recordings();
        const bool recordingOk = recs.size() == 1 && recs[0].id == 2
                                 && recs[0].name == QLatin1String("CQ")
                                 && recs[0].durationMs == 4200;
        report("status_and_recordings_reach_interface_listeners",
               seen == VoiceKeyer::Playback && seenId == 2 && keyer->activeId() == 2
                   && changed == QList<int>{2} && recordingOk);
    }

    // A refused command surfaces as commandFailed on the interface.
    {
        DvkModel model;
        VoiceKeyer* keyer = &model;
        QString verb, message;
        int id = 0;
        QObject::connect(keyer, &VoiceKeyer::commandFailed,
                         [&](const QString& v, int i, uint, const QString& m) {
                             verb = v; id = i; message = m;
                         });
        model.handleCommandResponse(QStringLiteral("rec_start"), 1, 0x500000A9u, QString());
        report("refusal_reaches_interface_listeners",
               verb == QLatin1String("rec_start") && id == 1
                   && message == QLatin1String("port already in use on radio"),
               message.toStdString());
    }

    // WAV import/export: unavailable until a transfer is wired; requests go
    // out by signal; busy follows the probe.
    {
        DvkModel model;
        VoiceKeyer* keyer = &model;
        report("no_transfer_wired_means_unavailable",
               !keyer->canTransferWav() && !keyer->isTransferring());

        int upId = 0, downId = 0;
        QString upPath, downPath;
        QObject::connect(&model, &DvkModel::wavUploadRequested,
                         [&](int i, const QString& p) { upId = i; upPath = p; });
        QObject::connect(&model, &DvkModel::wavDownloadRequested,
                         [&](int i, const QString& p) { downId = i; downPath = p; });
        bool busy = false;
        model.setWavTransferBusyProbe([&] { return busy; });

        keyer->importWav(9, QStringLiteral("/tmp/in.wav"));
        keyer->exportWav(10, QStringLiteral("/tmp/out.wav"));
        const bool idleOk = keyer->canTransferWav() && !keyer->isTransferring();
        busy = true;
        const bool busyOk = keyer->isTransferring();
        report("wav_transfer_requests_and_busy_probe",
               upId == 9 && upPath == QLatin1String("/tmp/in.wav")
                   && downId == 10 && downPath == QLatin1String("/tmp/out.wav")
                   && idleOk && busyOk);
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
