// #6244 — DvkModel against the SmartSDR API wiki (TCPIP-dvk) and lines
// captured from a FLEX-8600 on fw 4.2.20. Status lines go through the real
// CommandParser, so quoted names with spaces are covered end to end.
#include "core/backends/flex/CommandParser.h"
#include "models/DvkModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

void feed(DvkModel& model, const QString& line)
{
    const ParsedMessage msg = CommandParser::parseLine(line);
    model.applyStatus(msg.object, msg.kvs);
}

QString nameOf(const DvkModel& model, int id)
{
    for (const DvkRecording& r : model.recordings()) {
        if (r.id == id) {
            return r.name;
        }
    }
    return QStringLiteral("<missing>");
}

int durationOf(const DvkModel& model, int id)
{
    for (const DvkRecording& r : model.recordings()) {
        if (r.id == id) {
            return r.durationMs;
        }
    }
    return -1;
}

struct Sent {
    QStringList commands;
};

void capture(DvkModel& model, Sent& sent)
{
    QObject::connect(&model, &DvkModel::replyCommandReady, &model,
                     [&sent](const QString& cmd, const QString&, int) {
        sent.commands << cmd;
    });
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // ── parseKVs keeps quoted values whole ──────────────────────────────────
    {
        const auto kvs = CommandParser::parseKVs(
            QStringLiteral("id=3 name=\"CQ Contest\" duration=4210"));
        report("quoted value with a space stays whole",
               kvs.value("name") == QStringLiteral("\"CQ Contest\""));
        report("keys after a quoted value still parse",
               kvs.value("duration") == QStringLiteral("4210") && kvs.size() == 3);
    }
    {
        const auto kvs = CommandParser::parseKVs(
            QStringLiteral("a=\"one two three\" b=\"x y\" c=1"));
        report("two quoted values on one line",
               kvs.value("a") == QStringLiteral("\"one two three\"")
                   && kvs.value("b") == QStringLiteral("\"x y\"")
                   && kvs.value("c") == QStringLiteral("1"));
    }
    {
        const auto kvs = CommandParser::parseKVs(
            QStringLiteral("freq=14.225000 mode=USB name=\"Solo\" removed"));
        report("unquoted, single-word quoted and bare tokens are unchanged",
               kvs.value("freq") == QStringLiteral("14.225000")
                   && kvs.value("name") == QStringLiteral("\"Solo\"")
                   && kvs.contains("removed") && kvs.value("removed").isEmpty());
    }
    {
        const auto kvs = CommandParser::parseKVs(QStringLiteral("name=\"open ended x=1"));
        report("unterminated quote splits exactly as before",
               kvs.value("name") == QStringLiteral("\"open")
                   && kvs.contains("ended") && kvs.value("x") == QStringLiteral("1"));
    }

    // ── Status lines captured from fw 4.2.20 ────────────────────────────────
    {
        DvkModel model;
        feed(model, QStringLiteral("S629BA1D4|dvk status=idle enabled=1"));
        for (int id = 1; id <= 12; ++id) {
            feed(model, QStringLiteral("S629BA1D4|dvk added id=%1 name=\"Recording %1\" duration=0")
                            .arg(id));
        }
        report("sub dvk all yields 12 default slots",
               model.recordings().size() == 12
                   && nameOf(model, 10) == QStringLiteral("Recording 10"));
        report("idle status allows a new operation",
               model.status() == DvkModel::Idle && model.canStartOperation());

        feed(model, QStringLiteral("S629BA1D4|dvk id=12 name=\"Aether Probe Test\" duration=4000"));
        report("multi-word name is kept whole",
               nameOf(model, 12) == QStringLiteral("Aether Probe Test"));
        report("update carries the duration", durationOf(model, 12) == 4000);

        feed(model, QStringLiteral("S629BA1D4|dvk id=3 name=\"Recording CQ\" duration=1500"));
        report("a user name starting with 'Recording' is not overwritten",
               nameOf(model, 3) == QStringLiteral("Recording CQ"));

        feed(model, QStringLiteral("S629BA1D4|dvk added id=3 name=\"CQ Contest\" duration=4210"));
        report("an added line for a known slot updates it",
               nameOf(model, 3) == QStringLiteral("CQ Contest")
                   && durationOf(model, 3) == 4210 && model.recordings().size() == 12);

        feed(model, QStringLiteral("S629BA1D4|dvk status=preview id=12 enabled=1"));
        report("preview status blocks a new operation",
               model.status() == DvkModel::Preview && model.activeId() == 12
                   && !model.canStartOperation());
        feed(model, QStringLiteral("S0|dvk status=idle enabled=1"));
        report("radio-originated idle (handle 0) ends the preview",
               model.status() == DvkModel::Idle && model.activeId() == -1);

        feed(model, QStringLiteral("S629BA1D4|dvk deleted id=5"));
        report("deleted removes the slot", durationOf(model, 5) == -1);

        feed(model, QStringLiteral("S629BA1D4|dvk status=idle enabled=0"));
        report("enabled=0 reads as Disabled (FlexLib DVKStatus)",
               model.status() == DvkModel::Disabled && !model.canStartOperation());
    }

    // ── Commands match the wiki ─────────────────────────────────────────────
    {
        DvkModel model;
        Sent sent;
        capture(model, sent);
        model.recStop();
        model.previewStop();
        model.playbackStop();
        report("stop verbs carry no id",
               sent.commands == QStringList{QStringLiteral("dvk rec_stop"),
                                            QStringLiteral("dvk preview_stop"),
                                            QStringLiteral("dvk playback_stop")});
    }
    {
        DvkModel model;
        Sent sent;
        capture(model, sent);
        model.clear(7);
        report("clear sends only the clear until the radio accepts it",
               sent.commands == QStringList{QStringLiteral("dvk clear id=7")});
        model.handleCommandResponse(QStringLiteral("clear"), 7, 0u, QString());
        report("an accepted clear restores the default name (fw 4.2.20 keeps it)",
               sent.commands == QStringList{QStringLiteral("dvk clear id=7"),
                                            QStringLiteral("dvk set_name name=\"Recording 7\" id=7")});
        model.clear(8);
        model.handleCommandResponse(QStringLiteral("clear"), 8, 0xE2000000u, QString());
        report("a refused clear sends no rename",
               sent.commands.size() == 3 && sent.commands.last() == QStringLiteral("dvk clear id=8"));
    }
    {
        DvkModel model;
        Sent sent;
        capture(model, sent);
        model.setName(2, QStringLiteral("  CQ | \"DX\"   'test'  "));
        report("set_name strips quote and pipe characters and collapses spaces",
               sent.commands == QStringList{QStringLiteral("dvk set_name name=\"CQ DX test\" id=2")});
        sent.commands.clear();
        model.setName(2, QStringLiteral("|\"'"));
        report("a name that sanitizes to empty sends nothing", sent.commands.isEmpty());
    }
    {
        const QString longName(80, QLatin1Char('A'));
        report("ASCII name trimmed to 61 bytes",
               DvkModel::sanitizeName(longName).toUtf8().size() == DvkModel::kMaxNameBytes);
        const QString wide(40, QChar(0x00E9));  // 2 UTF-8 bytes each
        const QByteArray utf8 = DvkModel::sanitizeName(wide).toUtf8();
        report("multi-byte name never exceeds 61 bytes",
               utf8.size() <= DvkModel::kMaxNameBytes && utf8.size() >= DvkModel::kMaxNameBytes - 1);
    }

    // ── 50004001 is the license signal ──────────────────────────────────────
    {
        DvkModel model;
        int refusedSignals = 0;
        bool lastRefused = false;
        QObject::connect(&model, &DvkModel::licenseRefusedChanged, &model,
                         [&](bool refused) { ++refusedSignals; lastRefused = refused; });
        QString failedMessage;
        QObject::connect(&model, &DvkModel::commandFailed, &model,
                         [&](const QString&, int, uint, const QString& message) {
            failedMessage = message;
        });

        model.handleCommandResponse(QStringLiteral("rec_start"), 1, 0x50004001u, QString());
        report("50004001 marks the DVK as refused", model.licenseRefused() && lastRefused);
        report("50004001 names the subscription",
               failedMessage.contains(QStringLiteral("SmartSDR+")));
        model.handleCommandResponse(QStringLiteral("rec_start"), 1, 0x50004001u, QString());
        report("a repeat refusal does not re-signal", refusedSignals == 1);

        model.reset();
        report("reset clears the refusal and the slots",
               !model.licenseRefused() && !lastRefused && refusedSignals == 2
                   && model.recordings().isEmpty() && model.status() == DvkModel::Unknown);
    }
    {
        report("busy file server has a readable message",
               DvkModel::dvkErrorString(0x50000053u).contains(QStringLiteral("busy")));
        report("E2000000 has a readable message",
               DvkModel::dvkErrorString(0xE2000000u).contains(QStringLiteral("slot")));
        report("undocumented codes stay bare hex",
               DvkModel::dvkErrorString(0x12345678u) == QStringLiteral("error 0x12345678"));
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
