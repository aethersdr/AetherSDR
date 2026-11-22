// Offline UI test for CopyAssistPanel (RFC #4333, Phase 5). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies the confidence color-coding (the
// ggmorse-style feature), tier selection, the enable signal, and clear.

#include "gui/CopyAssistPanel.h"

#include <QAccessible>
#include <QApplication>
#include <QImage>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

#include <algorithm>
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

// An unmistakable draft colour, so the drawing can be found in a grab.
const QColor kDraft(255, 0, 255);

// Bounding box, in viewport pixels, of everything drawn in the draft colour.
QRect draftBounds(QTextEdit* view)
{
    const QImage image = view->viewport()->grab().toImage();
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 180 && c.blue() > 180 && c.green() < 90) {
                bounds |= QRect(x, y, 1, 1);
            }
        }
    }
    // grab() is in device pixels on a high-DPI screen.
    const qreal dpr = image.devicePixelRatio();
    return bounds.isValid() ? QRect(bounds.topLeft() / dpr, bounds.size() / dpr) : QRect();
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
    // Provisional text is drawn over the transcript, never written into its
    // document: the document is what a screen reader reads.
    {
        CopyAssistPanel p;
        p.setPreviewText(1, QStringLiteral("hello wor"), 0);
        expect(p.previewBlocks() == QStringList{QStringLiteral("hello wor")},
               "preview text is shown");
        expect(transcriptText(p).isEmpty(), "preview text never enters the transcript document");

        p.setPreviewText(1, QStringLiteral("hello world"), 0);
        expect(p.previewBlocks() == QStringList{QStringLiteral("hello world")},
               "a newer preview for the same id replaces the old text");

        p.setPreviewText(2, QStringLiteral("second"), 0);
        p.commitText(1, QStringLiteral("Hello World"), 0.95f);
        expect(transcriptText(p).contains("Hello World")
                   && p.previewBlocks() == QStringList{QStringLiteral("second")},
               "commitText replaces the grey block with the final");
        expect(!transcriptText(p).contains("second"),
               "a later segment's grey text stays out of the document");

        p.discardPreview(2, 0);
        expect(p.previewBlocks().isEmpty(), "discardPreview removes the grey block");

        p.setPreviewText(1, QStringLiteral("late"), 0);
        p.setPreviewText(2, QStringLiteral("later"), 0);
        expect(p.previewBlocks().isEmpty(),
               "a late preview for a committed or discarded id is ignored");

        p.setPreviewText(3, QStringLiteral("pending"), 0);
        expect(p.previewBlocks() == QStringList{QStringLiteral("pending")},
               "a newer id is still accepted");
        p.setPreviewText(3, QString(), 0);
        expect(p.previewBlocks().isEmpty(), "empty preview text removes the block");

        p.setPreviewText(4, QStringLiteral("gone"), 0);
        p.clearText();
        expect(transcriptText(p).trimmed().isEmpty() && p.previewBlocks().isEmpty(),
               "clearText clears the preview");
        p.setPreviewText(0, QStringLiteral("fresh"), 0);
        expect(p.previewBlocks() == QStringList{QStringLiteral("fresh")},
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
        // tap-point switch) without clearing the transcript, under a new
        // generation.
        CopyAssistPanel p;
        p.commitText(5, QStringLiteral("kept"), 0.9f);
        p.setPreviewText(6, QStringLiteral("stale grey"), 0);
        p.resetPreview(1);
        expect(transcriptText(p).contains("kept") && p.previewBlocks().isEmpty(),
               "resetPreview drops grey text and keeps the committed transcript");
        p.setPreviewText(0, QStringLiteral("restarted"), 1);
        expect(p.previewBlocks() == QStringList{QStringLiteral("restarted")},
               "after resetPreview a restarted id 0 is shown again");
        p.setPreviewText(7, QStringLiteral("old generation"), 0);
        p.discardPreview(0, 0);
        expect(p.previewBlocks() == QStringList{QStringLiteral("restarted")},
               "updates from an earlier generation are ignored");
    }

    // ---- Screen readers never see provisional text ------------------------
    {
        static int s_textEvents = 0;
        static QObject* s_watched = nullptr;
        static QAccessible::UpdateHandler s_previous = nullptr;
        CopyAssistPanel p;
        auto* view = p.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
        s_watched = view;
        QAccessible::setActive(true);
        s_previous = QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
            const auto type = event->type();
            if (event->object() == s_watched
                && (type == QAccessible::TextInserted || type == QAccessible::TextRemoved
                    || type == QAccessible::TextUpdated)) {
                ++s_textEvents;
            }
            if (s_previous != nullptr) {
                s_previous(event);
            }
        });

        p.commitText(0, QStringLiteral("CQ CQ"), 0.9f);
        const int afterCommit = s_textEvents;
        const int revision = view->document()->revision();
        for (int i = 0; i < 20; ++i) {
            p.setPreviewText(1, QStringLiteral("this is update %1").arg(i), 0);
        }
        p.setPreviewText(2, QStringLiteral("and more"), 0);
        p.discardPreview(2, 0);
        expect(afterCommit > 0, "committing text is reported to the screen reader");
        expect(s_textEvents == afterCommit,
               "provisional updates send the screen reader no text events");
        expect(view->document()->revision() == revision,
               "provisional updates leave the document untouched");

        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(view);
        QAccessibleTextInterface* text = iface != nullptr ? iface->textInterface() : nullptr;
        expect(text != nullptr
                   && text->text(0, text->characterCount()).trimmed() == QStringLiteral("CQ CQ"),
               "the transcript's accessible text is the committed text only");

        QAccessible::installUpdateHandler(s_previous);
        QAccessible::setActive(false);
    }

    // ---- Provisional text is painted after the committed text -------------
    for (const bool newline : {false, true}) {
        CopyAssistPanel p;
        p.setNewlineOnSilence(newline);
        p.setPreviewColor(kDraft);
        p.resize(400, 300);
        p.show();
        p.commitText(0, QStringLiteral("CQ"), 0.95f);
        auto* view = p.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
        const QRect committedLine = view->cursorRect(view->textCursor());
        expect(!draftBounds(view).isValid(), newline ? "nothing drawn before a preview (newline on)"
                                                     : "nothing drawn before a preview (newline off)");
        p.setPreviewText(1, QStringLiteral("this is the draft"), 0);
        const QRect drawn = draftBounds(view);
        if (newline) {
            expect(drawn.isValid() && drawn.top() >= committedLine.bottom() - 1,
                   "with newline-on-silence the draft is drawn on the next line");
        } else {
            expect(drawn.isValid() && drawn.top() < committedLine.bottom()
                       && drawn.left() >= committedLine.left() - 1,
                   "the draft is drawn on the committed line, after its text");
        }
        p.commitText(1, QStringLiteral("this is the final"), 0.95f);
        expect(!draftBounds(view).isValid(), newline ? "the final replaces the drawing (newline on)"
                                                     : "the final replaces the drawing (newline off)");
    }
    {
        // A draft longer than the view: the scroll range grows to reach it and
        // a view that was at the bottom follows it. One committed line per
        // segment, so the transcript is taller than the view whatever width the
        // panel's controls force.
        CopyAssistPanel p;
        p.setNewlineOnSilence(true);
        p.setPreviewColor(kDraft);
        p.resize(300, 200);
        p.show();
        for (int i = 0; i < 30; ++i) {
            p.commitText(quint64(i), QStringLiteral("line %1 of committed text").arg(i), 0.9f);
        }
        auto* view = p.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
        const int documentMax = view->verticalScrollBar()->maximum();
        QString longDraft;
        for (int i = 0; i < 400; ++i) {
            longDraft += QStringLiteral("draft words ");
        }
        p.setPreviewText(30, longDraft, 0);
        expect(documentMax > 0, "(setup) the committed transcript alone scrolls");
        expect(view->verticalScrollBar()->maximum() > documentMax,
               "the scroll range grows to make room for the draft");
        expect(view->verticalScrollBar()->value() == view->verticalScrollBar()->maximum(),
               "a view at the bottom follows the draft");
        const QRect drawn = draftBounds(view);
        expect(drawn.isValid() && drawn.bottom() >= view->viewport()->height() - 20,
               "the end of the draft is in view");
        p.commitText(30, QStringLiteral("short final"), 0.9f);
        CopyAssistPanel twin; // the same transcript, never shown a draft
        twin.setNewlineOnSilence(true);
        twin.resize(300, 200);
        twin.show();
        for (int i = 0; i < 30; ++i) {
            twin.commitText(quint64(i), QStringLiteral("line %1 of committed text").arg(i), 0.9f);
        }
        twin.commitText(30, QStringLiteral("short final"), 0.9f);
        auto* twinView = twin.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
        expect(view->verticalScrollBar()->maximum() == twinView->verticalScrollBar()->maximum(),
               "the extra scroll range goes when the draft does");
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
