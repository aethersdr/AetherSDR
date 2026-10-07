// #6244 review — the DVK panel admits one operation at a time from the moment
// a start is sent, not from the radio's echo, and none during a WAV transfer.
// A real DvkPanel and DvkModel; commands are captured, nothing reaches a radio.
#include "TestSettingsProfile.h"
#include "core/TxKeyingMarker.h"
#include "core/backends/flex/CommandParser.h"
#include "gui/DvkPanel.h"
#include "models/DvkModel.h"

#include <QApplication>
#include <QPushButton>
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

void seed(DvkModel& model)
{
    feed(model, QStringLiteral("S1|dvk status=idle enabled=1"));
    for (int id = 1; id <= 12; ++id) {
        feed(model, QStringLiteral("S1|dvk added id=%1 name=\"Recording %1\" duration=%2")
                        .arg(id).arg(id == 1 ? 2000 : 0));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dvk-panel-admission-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);

    {
        // The reviewer's probe: PREV, then REC, with no status in between.
        DvkModel model;
        DvkPanel panel(&model);
        QStringList sent;
        QObject::connect(&model, &DvkModel::replyCommandReady, &model,
                         [&sent](const QString& cmd, const QString&, int) { sent << cmd; });
        seed(model);
        auto* rec = panel.findChild<QPushButton*>(QStringLiteral("dvkRecord"));
        auto* prev = panel.findChild<QPushButton*>(QStringLiteral("dvkPreview"));
        auto* f1 = panel.findChild<QPushButton*>(QStringLiteral("dvkPlaySlot1"));
        auto* stop = panel.findChild<QPushButton*>(QStringLiteral("dvkStop"));
        if (!rec || !prev || !f1 || !stop) {
            std::fprintf(stderr, "Missing panel controls\n");
            return 1;
        }

        prev->click();
        rec->click();
        f1->click();
        report("two starts before the first echo send one command",
               sent == QStringList{QStringLiteral("dvk preview_start id=1")});
        report("the refused REC press does not stay checked", !rec->isChecked());

        stop->click();
        report("STOP before the echo stops the pending start",
               sent.size() == 2 && sent.last() == QStringLiteral("dvk preview_stop"));
    }

    {
        // A running import or export blocks every start control.
        DvkModel model;
        DvkPanel panel(&model);
        QStringList sent;
        QObject::connect(&model, &DvkModel::replyCommandReady, &model,
                         [&sent](const QString& cmd, const QString&, int) { sent << cmd; });
        seed(model);
        model.setTransferActive(true);
        panel.findChild<QPushButton*>(QStringLiteral("dvkRecord"))->click();
        panel.findChild<QPushButton*>(QStringLiteral("dvkPlaySlot1"))->click();
        report("REC and an F-key during a WAV transfer send nothing", sent.isEmpty());
        model.setTransferActive(false);
        panel.findChild<QPushButton*>(QStringLiteral("dvkRecord"))->click();
        report("the transfer's end admits REC again",
               sent == QStringList{QStringLiteral("dvk rec_start id=1")});
    }

    {
        // The bridge refuses controls marked as keying TX.
        DvkModel model;
        DvkPanel panel(&model);
        bool allMarked = panel.findChild<QPushButton*>(QStringLiteral("dvkPlay"))
                             ->property(kTxKeyingProperty).toBool();
        for (int id = 1; id <= 12; ++id) {
            allMarked = allMarked
                && panel.findChild<QPushButton*>(QStringLiteral("dvkPlaySlot%1").arg(id))
                       ->property(kTxKeyingProperty).toBool();
        }
        report("PLAY and every F-key are marked as keying TX", allMarked);
        report("REC and PREV are not (they never key the radio)",
               !panel.findChild<QPushButton*>(QStringLiteral("dvkRecord"))->property(kTxKeyingProperty).toBool()
                   && !panel.findChild<QPushButton*>(QStringLiteral("dvkPreview"))->property(kTxKeyingProperty).toBool());
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
