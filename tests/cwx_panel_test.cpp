// Focused CWX panel behavior tests.
// Run: ./build/cwx_panel_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/CwxPanel.h"
#include "models/CwxModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMap>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QTextEdit>
#include <cstdio>
#include <algorithm>
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

QPushButton* buttonByText(CwxPanel& panel, const QString& text)
{
    const auto buttons = panel.findChildren<QPushButton*>();
    for (auto* button : buttons) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QTextEdit* inputEdit(CwxPanel& panel)
{
    const auto edits = panel.findChildren<QTextEdit*>();
    for (auto* edit : edits) {
        if (edit->placeholderText() == QLatin1String("Type CW message..."))
            return edit;
    }
    return nullptr;
}

QTextEdit* macroEdit(CwxPanel& panel, int fKey /* 1..12 */)
{
    const QString placeholder = QString("F%1 macro...").arg(fKey);
    const auto edits = panel.findChildren<QTextEdit*>();
    for (auto* edit : edits) {
        if (edit->placeholderText() == placeholder)
            return edit;
    }
    return nullptr;
}

// The macro grid's QScrollArea specifically, walked up from a macro edit's
// own parent chain. CwxPanel has TWO QScrollAreas — m_historyScroll (send
// page, built first in the constructor) and the macro grid's (setup page,
// built second) — so an unqualified panel.findChild<QScrollArea*>() finds
// m_historyScroll first and silently passes even with the #4945 fix fully
// reverted (caught in review on #5125, credit aethersdr-agent). Anchoring
// the search at a widget actually inside the scroll area we mean is the fix.
QScrollArea* macroScrollAreaAncestor(QTextEdit* macroRow)
{
    for (QWidget* w = macroRow ? macroRow->parentWidget() : nullptr; w; w = w->parentWidget()) {
        if (auto* sa = qobject_cast<QScrollArea*>(w))
            return sa;
    }
    return nullptr;
}

struct Fixture {
    CwxModel model;
    CwxPanel panel{&model};
    QStringList commands;

    Fixture()
    {
        QObject::connect(&model, &CwxModel::commandReady,
                         [this](const QString& command) {
                             commands.push_back(command);
                         });
        // replyCommandReady carries the final cwx send of each macro block, and
        // every live-mode char, plus the drain-watch epoch + batch char count (#3949)
        QObject::connect(&model, &CwxModel::replyCommandReady,
                         [this](const QString& command, int /*epoch*/, int /*nChars*/) {
                             commands.push_back(command);
                         });
    }
};

bool requireSendWidgets(Fixture& fixture, QPushButton*& send, QPushButton*& live,
                        QPushButton*& setup, QTextEdit*& input)
{
    send = buttonByText(fixture.panel, "Send");
    live = buttonByText(fixture.panel, "Live");
    setup = buttonByText(fixture.panel, "Setup");
    input = inputEdit(fixture.panel);

    const bool ok = send && live && setup && input;
    report("CWX controls are present", ok);
    return ok;
}

QString sendCommand(const QString& text, int block)
{
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    return QString("cwx send \"%1\" %2").arg(encoded).arg(block);
}

void testLiveButtonTogglesOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    report("Live click enables live mode",
           f.model.isLive() && live->isChecked());

    live->click();
    report("second Live click disables live mode",
           !f.model.isLive() && !live->isChecked());
}

void testDisplayNameCanFollowTheRadioFamily()
{
    Fixture f;
    report("shared keyer panel defaults to the Flex CWX name",
           f.panel.displayName() == QLatin1String("CWX"));
    f.panel.setDisplayName(QStringLiteral("CWK"));
    report("Icom can relabel the shared keyer panel as CWK",
           f.panel.displayName() == QLatin1String("CWK"));

    QPushButton* live = buttonByText(f.panel, "Live");
    QPushButton* setup = buttonByText(f.panel, "Setup");
    QSpinBox* speed = f.panel.findChild<QSpinBox*>(QStringLiteral("cwxSpeedSpin"));
    f.panel.configureTextKeyer(QStringLiteral("CWK"), 6, 48, false, false);
    report("Icom CWK hides unsupported Live and stored-macro setup",
           live && setup && live->isHidden() && setup->isHidden());
    report("Icom CWK exposes the radio's honest 6..48 WPM range",
           speed && speed->minimum() == 6 && speed->maximum() == 48);

    f.panel.configureTextKeyer(QStringLiteral("CWX"), 5, 100, true, true);
    report("Flex CWX restores Live, Setup and the 5..100 WPM range",
           live && setup && !live->isHidden() && !setup->isHidden()
               && speed && speed->minimum() == 5 && speed->maximum() == 100);
}

void testSimpleKeyerDoesNotExpandSpeedModifiers()
{
    CwxModel model;
    QVector<CwxModel::SpeedSegment> sends;
    QObject::connect(&model, &CwxModel::transmissionRequested,
                     [&sends](const QString& text, int wpm) {
        sends.push_back({text, wpm});
    });
    model.setSpeedModifiersEnabled(false);
    model.send(QStringLiteral("+CQ TEST"));
    report("a keyer without progress receives one bounded unmodified message",
           sends == QVector<CwxModel::SpeedSegment>{{QStringLiteral("+CQ TEST"), 20}});
}

void testSendButtonSendsWhenLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    input->setPlainText("CQ TEST");
    send->click();

    report("Send button sends current input",
           f.commands == QStringList{sendCommand("CQ TEST", 1)});
    report("Send button clears input after send",
           input->toPlainText().isEmpty());
}

void testEnterStillSendsWhenLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    input->setPlainText("73");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QCoreApplication::sendEvent(input, &enter);

    report("Enter key still sends current input",
           f.commands == QStringList{sendCommand("73", 1)});
}

void testSendButtonTurnsLiveOffWithoutDuplicateSend()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    input->setPlainText("ALREADY KEYED");
    f.commands.clear();

    send->click();

    report("Send button exits live mode",
           !f.model.isLive() && !live->isChecked());
    report("Send button does not duplicate-send live text",
           f.commands.isEmpty());
    report("Send button keeps live text visible when exiting live",
           input->toPlainText() == QLatin1String("ALREADY KEYED"));
}

void testSetupTurnsLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    setup->click();

    report("Setup exits live mode",
           !f.model.isLive() && !live->isChecked() && setup->isChecked());
}

} // namespace

// Resend must re-send the raw (modifier-bearing) text, not the flattened
// display text, so per-word speed variation survives a Resend. (#272)
void testResendPreservesSpeedModifiers()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    input->setPlainText("+CQ");
    send->click();

    auto* bubble = f.panel.pendingBubble();
    const bool haveBubble = bubble != nullptr;
    report("modifier send creates an in-flight history bubble", haveBubble);
    if (!haveBubble)
        return;

    report("bubble retains raw modifier text for Resend",
           bubble->rawText() == QLatin1String("+CQ"));
    report("bubble paints modifier-stripped display text",
           bubble->text() == QLatin1String("CQ"));

    auto emitsWpmChange = [](const QStringList& cmds) {
        return std::any_of(cmds.begin(), cmds.end(), [](const QString& c) {
            return c.startsWith(QLatin1String("cwx wpm"));
        });
    };

    report("modifier send emits a per-word cwx wpm speed change",
           emitsWpmChange(f.commands));

    // Re-sending the stripped display text (the pre-fix Resend bug) drops the
    // speed change — which is exactly why the raw text is kept on the bubble.
    f.commands.clear();
    f.model.send(bubble->text());
    report("resending stripped text loses the speed change",
           !emitsWpmChange(f.commands));

    // Re-sending the raw text reproduces the speed change, so Resend is faithful.
    f.commands.clear();
    f.model.send(bubble->rawText());
    report("resending raw text preserves the speed change",
           emitsWpmChange(f.commands));
}

// #4945 fix: confirms the new QScrollArea parent between CwxPanel and the
// macro rows doesn't break the F-key edit/save/send wiring. Signal/slot
// connections aren't parent-chain-dependent in Qt, so this isn't expected
// to catch anything the layout checks above wouldn't -- but it's the one
// path this fix touches that isn't pure geometry, so it earns a direct check.
void testMacroEditAndFKeyClickStillWorkThroughScrollArea()
{
    Fixture f;
    QPushButton* setup = buttonByText(f.panel, "Setup");
    if (setup) setup->click();

    QTextEdit* f1 = macroEdit(f.panel, 1);
    QPushButton* f1Button = buttonByText(f.panel, "F1");
    const bool ok = f1 && f1Button;
    report("F1 macro row and its F-key button are both reachable through the scroll area", ok);
    if (!ok) return;

    // No spaces — emitExpandedSend() wire-encodes them as 0x7f (matching
    // sendCommand() above), which would make a literal-text contains()
    // check fail for reasons that have nothing to do with this fix.
    f1->setPlainText("TESTDEOH6NEQ");
    QCoreApplication::processEvents();
    report("typing into the macro row still saves to the model",
           f.model.macro(0) == QLatin1String("TESTDEOH6NEQ"));

    f.commands.clear();
    f1Button->click();
    const bool sent = std::any_of(f.commands.begin(), f.commands.end(),
        [](const QString& c) { return c.contains(QLatin1String("TESTDEOH6NEQ")); });
    report("clicking the F1 button still sends the saved macro text", sent);
}

// #4945 fix, zero-visual-change check: the QScrollArea wrap must not
// introduce a scrollbar or otherwise change the Setup page's look when the
// panel actually has room, which is the common case away from the app's
// minimum window height. Uses the same embedding as the minimized-height
// repro below but at a generous height.
void testSetupPageUnaffectedAtNormalWindowHeight()
{
    Fixture f;
    auto* host = new QWidget;
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(&f.panel);
    auto* filler = new QWidget;
    splitter->addWidget(filler);
    splitter->setParent(host);
    host->resize(1024, 900); // a generously tall window, not the 400px minimum
    splitter->setGeometry(host->rect());
    host->show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    QPushButton* setup = buttonByText(f.panel, "Setup");
    if (setup) setup->click();
    QCoreApplication::processEvents();

    QTextEdit* f1 = macroEdit(f.panel, 1);
    QScrollArea* macroScroll = macroScrollAreaAncestor(f1);
    report("a QScrollArea exists for the macro grid", macroScroll != nullptr);
    if (macroScroll) {
        const bool scrollbarNeeded = macroScroll->verticalScrollBar()
            && macroScroll->verticalScrollBar()->isVisible();
        report("no scrollbar appears when the panel has plenty of room",
               !scrollbarNeeded);
    }

    if (f1) {
        // Calls the SAME function buildSetupView() calls, not a copy of its
        // formula — a hardcoded `34` here is exactly how this and the
        // production constant drifted apart the first time (#5125 review,
        // credit NF0T): production moved to font metrics and this test
        // kept its own old number.
        const int floor = CwxPanel::macroRowMinimumHeight(f1->font());
        std::printf("(normal height) F1 macro row height()=%d  floor=%d\n",
                    f1->height(), floor);
        report("rows are NOT capped at the #4945 minimum-height floor when "
               "there's room to be taller",
               f1->height() > floor);
    }

    f.panel.setParent(nullptr);
    delete host;
}

// #4945 reproduction — NOT yet a fix verification. Mirrors the real embedding
// (MainWindow.cpp:4766: CwxPanel added straight into the main horizontal
// QSplitter) and squeezes it to the app's actual minimum window height
// (MainWindow.cpp:1126: setMinimumSize(1024, 400)), minus a rough allowance
// for the title bar and status bar the splitter doesn't own.
//
// The reporter's actual screenshot (fetched directly from the issue, which
// the bot's triage said it couldn't do) shows the SETUP page — F1/F2 macro
// row TEXT with the tops of glyphs clipped off — not the send-page input
// box the bot's item 3 focused on. That distinction matters mechanically:
// m_textEdit (the send box) is setFixedHeight(60), which Qt's layout engine
// protects; m_macroEdits[i] are QSizePolicy::Expanding, which is what
// actually absorbs a squeeze. First pass of this repro (measuring the send
// page) found m_textEdit stayed at its full 60px under this same squeeze —
// consistent with that distinction, and a real gap in the bot's diagnosis.
void reproduceIssue4945MinimizedHeight()
{
    std::printf("\n--- #4945 repro: CWX Setup page at the app's minimum window height ---\n");

    Fixture f;
    auto* host = new QWidget;
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(&f.panel);
    auto* filler = new QWidget; // stands in for the panadapter stack + RX applet column
    splitter->addWidget(filler);
    splitter->setParent(host);
    // 400px app minimum height minus the title/status bar. NF0T measured
    // the real panel at 322px against a live FLEX-8400 at the app's actual
    // minimum (review on #5125) -- 330 was an estimate off by ~8px; using
    // the measured figure instead of guessing again.
    host->resize(1024, 322);
    splitter->setGeometry(host->rect());
    host->show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    QPushButton* setup = buttonByText(f.panel, "Setup");
    QTextEdit* f1 = macroEdit(f.panel, 1);
    if (f1)
        f1->setPlainText("KN7K"); // match the reporter's screenshot content
    if (setup)
        setup->click();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    std::printf("panel height()=%d\n", f.panel.height());

    // Structural check, not just a height reading: a mutation pass on this
    // fix found that setMinimumHeight() alone can report a passing height()
    // on ONE queried row while the panel still renders visibly squeezed —
    // the QScrollArea is what actually does the work (verified by removing
    // each half independently: scroll-area-alone passed both this check and
    // a visual grab; setMinimumHeight-alone passed a single row's height()
    // reading but still looked squeezed in the saved screenshot). So this
    // pins the wiring directly: an ancestor of the macro edits must be a
    // QScrollArea, not just "some row happens to measure tall enough".
    QScrollArea* macroScroll = macroScrollAreaAncestor(f1);
    report("the macro grid is wrapped in a QScrollArea", macroScroll != nullptr);

    // PITCH, not height() alone (review on #5125, credit NF0T). Checking
    // every row's height() instead of just F1's was still not enough:
    // setMinimumHeight() clamps QWidget::height() to the floor even when
    // the LAYOUT doesn't actually give the widget that much room, so under
    // a squeeze the rows overlap -- each painting over the one above --
    // while every individual height() keeps reporting the floor value.
    // Verified directly: reverting to the pre-#5125 code (scroll area
    // removed, floor kept) still reports every row's height() at the
    // floor, while the actual pitch -- the real vertical distance between
    // consecutive rows -- collapses from ~36px to ~14px, a 20px overlap
    // per row. Pitch is what actually reveals that; height() cannot.
    const int oneLine = CwxPanel::macroRowMinimumHeight(f1 ? f1->font() : f.panel.font());
    int shortRows = 0;
    int previousTop = -1;
    for (int i = 1; i <= 12; ++i) {
        QTextEdit* row = macroEdit(f.panel, i);
        if (!row) { ++shortRows; continue; }
        const int top = row->mapTo(&f.panel, QPoint(0, 0)).y();
        if (previousTop >= 0 && (top - previousTop) < oneLine)
            ++shortRows;
        previousTop = top;
    }
    std::printf("rows whose pitch to the next row is below one readable "
               "line (%dpx): %d/11 gaps checked\n", oneLine, shortRows);
    report("every macro row's pitch to the next stays clear of one "
           "readable line at min window height (no overlap)",
           shortRows == 0);

    // Visual side-by-side: render the live Setup page next to the
    // reporter's actual screenshot so a human can eyeball the match rather
    // than trust the height arithmetic alone.
    const QPixmap grabbed = f.panel.grab();
    const QString outPath = QDir(QDir::tempPath()).filePath("cwx_4945_repro_setup_page.png");
    if (grabbed.save(outPath))
        std::printf("saved repro screenshot to %s (%dx%d)\n",
                    qPrintable(outPath), grabbed.width(), grabbed.height());
    else
        std::printf("failed to save repro screenshot to %s\n", qPrintable(outPath));

    // f.panel is a stack member of Fixture, not heap-owned — detach it from
    // the splitter before host's children get destroyed, or Qt tries to
    // `delete` a non-heap pointer when the splitter is torn down.
    f.panel.setParent(nullptr);
    delete host;
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-cwx-panel-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::printf("CWX panel behavior test harness\n\n");

    testLiveButtonTogglesOff();
    testDisplayNameCanFollowTheRadioFamily();
    testSimpleKeyerDoesNotExpandSpeedModifiers();
    testSendButtonSendsWhenLiveOff();
    testEnterStillSendsWhenLiveOff();
    testSendButtonTurnsLiveOffWithoutDuplicateSend();
    testSetupTurnsLiveOff();
    testResendPreservesSpeedModifiers();
    testSetupPageUnaffectedAtNormalWindowHeight();
    testMacroEditAndFKeyClickStillWorkThroughScrollArea();
    reproduceIssue4945MinimizedHeight();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
