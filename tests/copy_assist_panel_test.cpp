// Offline UI test for CopyAssistPanel (RFC #4333, Phase 5). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies the confidence color-coding (the
// ggmorse-style feature), tier selection, the enable signal, and clear.

#include "gui/CopyAssistPanel.h"

#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString transcriptHtml(CopyAssistPanel& panel)
{
    // The transcript is the only QTextEdit in the panel.
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toHtml() : QString();
}

QString transcriptText(CopyAssistPanel& panel)
{
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toPlainText() : QString();
}

// Character format of the first character of `needle` in the transcript.
QTextCharFormat formatAt(CopyAssistPanel& panel, const QString& needle)
{
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    const int pos = edit->toPlainText().indexOf(needle);
    QTextCursor cursor(edit->document());
    cursor.setPosition(pos + 1); // the format of the character before the cursor
    return cursor.charFormat();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    CopyAssistPanel panel;

    // ---- Confidence color-coding (green=high … red=low) -------------------
    panel.appendText(QStringLiteral("high"), 0.95f);
    panel.appendText(QStringLiteral("medium"), 0.72f);
    panel.appendText(QStringLiteral("low"), 0.50f);
    panel.appendText(QStringLiteral("verylow"), 0.10f);

    const QString html = transcriptHtml(panel).toLower();
    expect(html.contains("high") && html.contains("#00ff88"), "high confidence -> green");
    expect(html.contains("medium") && html.contains("#e0e040"), "medium confidence -> yellow");
    expect(html.contains("low") && html.contains("#ff9020"), "low confidence -> orange");
    expect(html.contains("verylow") && html.contains("#ff4040"), "very low confidence -> red");

    // ---- Empty/whitespace text is ignored ---------------------------------
    const QString before = transcriptText(panel);
    panel.appendText(QStringLiteral("   "), 0.9f);
    expect(transcriptText(panel) == before, "blank utterance is not appended");

    // ---- Clear -------------------------------------------------------------
    panel.clearText();
    expect(transcriptText(panel).trimmed().isEmpty(), "clearText empties the transcript");

    // ---- Live preview (grey provisional text) -----------------------------
    {
        CopyAssistPanel p;
        p.setPreviewText(1, QStringLiteral("hello wor"));
        expect(transcriptText(p).contains("hello wor"), "preview text appears in the transcript");

        p.setPreviewText(1, QStringLiteral("hello world"));
        expect(transcriptText(p).contains("hello world") && !transcriptText(p).contains("hello wor "),
               "a newer preview for the same id replaces the old text");

        p.setPreviewText(2, QStringLiteral("second"));
        p.commitText(1, QStringLiteral("Hello World"), 0.95f);
        const QString text = transcriptText(p);
        expect(text.contains("Hello World") && !text.contains("hello world"),
               "commitText replaces the grey block with the final");
        expect(text.indexOf("Hello World") < text.indexOf("second"),
               "a later segment's grey text stays after the committed text");

        expect(formatAt(p, QStringLiteral("Hello")).foreground().color()
                   != formatAt(p, QStringLiteral("second")).foreground().color(),
               "committed text and grey text differ in colour");
        expect(formatAt(p, QStringLiteral("second")).fontItalic()
                   && !formatAt(p, QStringLiteral("Hello")).fontItalic(),
               "grey text is italic, committed text is not");

        p.discardPreview(2);
        expect(!transcriptText(p).contains("second"), "discardPreview removes the grey block");

        p.setPreviewText(1, QStringLiteral("late"));
        p.setPreviewText(2, QStringLiteral("later"));
        expect(!transcriptText(p).contains("late"),
               "a late preview for a committed or discarded id is ignored");

        p.setPreviewText(3, QStringLiteral("pending"));
        expect(transcriptText(p).contains("pending"), "a newer id is still accepted");
        p.setPreviewText(3, QString());
        expect(!transcriptText(p).contains("pending"), "empty preview text removes the block");

        p.setPreviewText(4, QStringLiteral("gone"));
        p.clearText();
        expect(transcriptText(p).trimmed().isEmpty(), "clearText clears the preview");
        p.setPreviewText(0, QStringLiteral("fresh"));
        expect(transcriptText(p).contains("fresh") && !transcriptText(p).contains("gone"),
               "after clearText ids restart and the old grey text is gone");
    }
    for (const bool newline : {false, true}) {
        CopyAssistPanel a;
        CopyAssistPanel b;
        a.setNewlineOnSilence(newline);
        b.setNewlineOnSilence(newline);
        a.appendText(QStringLiteral("one"), 0.8f);
        b.commitText(0, QStringLiteral("one"), 0.8f);
        a.appendText(QStringLiteral("two <b>"), 0.3f);
        b.commitText(1, QStringLiteral("two <b>"), 0.3f);
        expect(transcriptHtml(a) == transcriptHtml(b) && transcriptText(a) == transcriptText(b),
               newline ? "commitText matches appendText (newline on)"
                       : "commitText matches appendText (newline off)");
    }
    {
        // The engine restarts segment ids at 0 on every reset (disable,
        // tap-point switch) without clearing the transcript.
        CopyAssistPanel p;
        p.commitText(5, QStringLiteral("kept"), 0.9f);
        p.setPreviewText(6, QStringLiteral("stale grey"));
        p.resetPreview();
        expect(transcriptText(p).contains("kept") && !transcriptText(p).contains("stale grey"),
               "resetPreview drops grey text and keeps the committed transcript");
        p.setPreviewText(0, QStringLiteral("restarted"));
        expect(transcriptText(p).contains("restarted"),
               "after resetPreview a restarted id 0 is shown again");
        for (int i = 0; i < 20; ++i) {
            p.setPreviewText(1, QStringLiteral("update %1").arg(i));
        }
        const auto* view = p.findChild<QTextEdit*>();
        expect(view != nullptr && view->document()->availableUndoSteps() == 0,
               "preview rewrites keep no undo history");
    }
    {
        CopyAssistPanel p;
        p.setNewlineOnSilence(true);
        p.commitText(0, QStringLiteral("first"), 0.9f);
        p.setPreviewText(1, QStringLiteral("grey"));
        expect(transcriptText(p).contains("\n") || transcriptText(p).contains(QChar(0x2028)),
               "a grey block starts a new line when newline-on-silence is on");
    }

    // ---- Settings (⚙) button emits its request ----------------------------
    // (Model/GPU selection now lives in CopyAssistSettingsDialog — see
    // copy_assist_settings_dialog_test.)
    QSignalSpy settingsSpy(&panel, &CopyAssistPanel::settingsRequested);
    panel.settingsButton()->click();
    expect(!settingsSpy.isEmpty(), "settingsRequested emitted on ⚙ click");

    // ---- Enable toggle emits + reflects state -----------------------------
    QSignalSpy enableSpy(&panel, &CopyAssistPanel::enableToggled);
    panel.setAsrEnabled(true);
    expect(panel.isAsrEnabled(), "setAsrEnabled reflects state");
    expect(!enableSpy.isEmpty() && enableSpy.last().at(0).toBool(),
           "enableToggled(true) emitted");

    // ---- Tuning sliders: set + emit --------------------------------------
    {
        QSignalSpy bufSpy(&panel, &CopyAssistPanel::bufferMsChanged);
        panel.setBufferMs(5000);
        expect(panel.bufferMs() == 5000, "setBufferMs sets 5 s");
        expect(!bufSpy.isEmpty() && bufSpy.last().at(0).toInt() == 5000,
               "bufferMsChanged emits milliseconds");

        QSignalSpy sensSpy(&panel, &CopyAssistPanel::sensitivityChanged);
        panel.setSensitivity(50);
        expect(panel.sensitivity() == 50, "setSensitivity sets percent");
        expect(!sensSpy.isEmpty() && sensSpy.last().at(0).toInt() == 50,
               "sensitivityChanged emits percent");

        QSignalSpy silSpy(&panel, &CopyAssistPanel::silenceMsChanged);
        panel.setSilenceMs(500);
        expect(panel.silenceMs() == 500, "setSilenceMs sets milliseconds");
        expect(!silSpy.isEmpty() && silSpy.last().at(0).toInt() == 500,
               "silenceMsChanged emits milliseconds");

        // Clamping into range.
        panel.setBufferMs(999999);
        expect(panel.bufferMs() == 20000, "buffer clamps to 20 s max");

        panel.setFontPx(20);
        expect(panel.fontPx() == 20, "setFontPx sets transcript font size");
        panel.setFontPx(99);
        expect(panel.fontPx() == 32, "font size clamps to 32 px max");
        panel.setFontPx(1);
        expect(panel.fontPx() == 8, "font size clamps to 8 px min");
    }

    std::printf(g_failures == 0 ? "\nCopy Assist panel: ALL PASS\n"
                                : "\nCopy Assist panel: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
