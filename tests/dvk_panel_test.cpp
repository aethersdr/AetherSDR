// Focused DVK panel behavior tests.
// Run: ./build/dvk_panel_test

#include "gui/DvkPanel.h"
#include "models/DvkModel.h"

#include <QApplication>
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
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

struct Fixture {
    DvkModel model;
    DvkPanel panel{&model};
    QStringList commands;

    Fixture()
    {
        // Playback commands are emitted via replyCommandReady (they need
        // response correlation); capture the raw command string.
        QObject::connect(&model, &DvkModel::replyCommandReady,
                         [this](const QString& cmd, const QString&, int) {
                             commands.push_back(cmd);
                         });
    }

    // Give slot `id` a recording so durationForSlot(id) > 0 — otherwise the
    // START branch is a no-op regardless of the visibility guard.
    void addRecording(int id, int durationMs)
    {
        QMap<QString, QString> kvs;
        kvs.insert("id", QString::number(id));
        kvs.insert("duration", QString::number(durationMs));
        model.applyStatus("dvk", kvs);
    }

    // Drive the model into Playback state for slot `id`, as a radio status
    // push would (dvk status=playback id=N enabled=1).
    void setPlaying(int id)
    {
        QMap<QString, QString> kvs;
        kvs.insert("status", "playback");
        kvs.insert("id", QString::number(id));
        kvs.insert("enabled", "1");
        model.applyStatus("dvk", kvs);
    }
};

// #3514: the F1-F12 playback fire must not START a stored voice message when
// the DVK panel is hidden, even though the active slice's mode leaves the
// ApplicationShortcut enabled. The guard lives in firePlayback(), not in the
// shortcut enable state, so the "one enabled shortcut per key" invariant
// (#2464/#2582) stays intact — the shortcut still activates while hidden; the
// fire just returns early.
//
// We drive firePlaybackForTest() directly rather than synthesizing an F-key
// press, for the same reason cwx_panel_test does: the panel is unparented, so
// window() resolves to the panel itself; hiding it hides the shortcut owner
// and suppresses dispatch entirely — a key-event test would then pass whether
// or not the guard exists (a false pass). The seam exercises the guarded path.
void testPlaybackBlockedWhenPanelHidden()
{
    Fixture f;
    f.addRecording(1, 5000);
    f.panel.setShortcutsEnabled(true);  // as MainWindow does when enabled

    // Panel hidden (never shown): F1 must NOT start playback.
    f.panel.firePlaybackForTest(1);
    report("F1 does not start playback when DVK panel hidden",
           f.commands.isEmpty(),
           f.commands.isEmpty() ? "" : ("fired: " + f.commands.join(',').toStdString()));

    // Panel visible: F1 starts playback of the stored slot.
    f.panel.show();
    f.panel.firePlaybackForTest(1);
    report("F1 starts playback when DVK panel visible",
           f.commands == QStringList{"dvk playback_start id=1"},
           f.commands.join(',').toStdString());
    f.commands.clear();
}

// #3514 (review follow-up): STOPPING an in-flight transmission must stay
// reachable even with the panel hidden — otherwise a voice message started
// while visible can no longer be F-key-stopped after the panel is closed,
// and the radio keeps transmitting. This mirrors the ungated ESC/CW-abort
// exemption (#1552): abort paths must always be usable.
void testPlaybackStopReachableWhenHidden()
{
    Fixture f;
    f.addRecording(1, 5000);
    f.panel.setShortcutsEnabled(true);
    f.panel.show();

    // Start playback while visible.
    f.panel.firePlaybackForTest(1);
    report("playback started while visible",
           f.commands == QStringList{"dvk playback_start id=1"},
           f.commands.join(',').toStdString());
    f.commands.clear();

    // Radio confirms it is now playing slot 1, then the panel is hidden.
    f.setPlaying(1);
    f.panel.hide();

    // F1 while hidden must STOP the in-flight transmission, not be swallowed.
    f.panel.firePlaybackForTest(1);
    report("F1 stops in-flight playback even when DVK panel hidden",
           f.commands == QStringList{"dvk playback_stop id=1"},
           f.commands.isEmpty() ? "(nothing fired — stop was swallowed)"
                                : f.commands.join(',').toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("DVK panel behavior test harness\n\n");

    testPlaybackBlockedWhenPanelHidden();
    testPlaybackStopReachableWhenHidden();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
