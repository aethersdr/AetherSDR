// #6249 — the DVK panel on the AetherSDR style guide (RFC #6226), in the title
// bar's vocabulary (#6198). A real DvkPanel is driven by fw 4.2.20 status
// lines, so every state is checked without a radio and without transmitting.
#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "core/backends/flex/CommandParser.h"
#include "gui/DvkPanel.h"
#include "models/DvkModel.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const QString& label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", qPrintable(label));
    if (!ok) {
        ++g_failed;
    }
}

void feed(DvkModel& model, const QString& line)
{
    const ParsedMessage msg = CommandParser::parseLine(line);
    model.applyStatus(msg.object, msg.kvs);
}

template <typename T>
T* child(const DvkPanel& panel, const QString& name)
{
    return panel.findChild<T*>(name);
}

QString prop(const QWidget* w, const char* name)
{
    return w ? w->property(name).toString() : QStringLiteral("<missing>");
}

// Saves a PNG of each state when DVK_PANEL_GRAB_DIR is set, for review.
void grab(DvkPanel& panel, const QString& name)
{
    const QString dir = qEnvironmentVariable("DVK_PANEL_GRAB_DIR");
    if (!dir.isEmpty()) {
        QApplication::processEvents();
        panel.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dvk-panel-theme-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    auto& tm = ThemeManager::instance();

    // Every token the panel names resolves in both bundled themes, and the
    // panel carries no colour of its own.
    const QStringList tokens = ThemeManager::extractReferencedTokens(DvkPanel::styleTemplate());
    report(QStringLiteral("panel stylesheet references tokens"), !tokens.isEmpty());
    for (const QString& theme : {QStringLiteral("Default Dark"), QStringLiteral("Default Light")}) {
        report(QStringLiteral("%1 loads").arg(theme), tm.setActiveTheme(theme));
        QStringList missing;
        for (const QString& token : tokens) {
            if (!tm.color(token).isValid()) {
                missing << token;
            }
        }
        report(QStringLiteral("%1 resolves every panel token %2").arg(theme, missing.join(' ')),
               missing.isEmpty());
    }
    static const QRegularExpression kLiteral(QStringLiteral("#[0-9a-fA-F]{3,8}\\b|rgba?\\("));
    report(QStringLiteral("panel stylesheet has no colour literals"),
           !DvkPanel::styleTemplate().contains(kLiteral));
    for (const QString& token : tokens) {
        if (!token.startsWith(QLatin1String("color.canon."))
            && !token.startsWith(QLatin1String("color.titlebar."))
            && !token.startsWith(QLatin1String("color.tx.mox."))
            && token != QLatin1String("color.background.tx")
            && token != QLatin1String("color.accent.danger")) {
            report(QStringLiteral("token outside canon / title bar / MOX: %1").arg(token), false);
        }
    }

    tm.setActiveTheme(QStringLiteral("Default Dark"));
    DvkModel model;
    DvkPanel panel(&model);
    panel.resize(250, 700);
    panel.show();

    auto* slot1 = child<QFrame>(panel, QStringLiteral("dvkSlot1"));
    auto* slot3 = child<QFrame>(panel, QStringLiteral("dvkSlot3"));
    auto* fkey3 = child<QPushButton>(panel, QStringLiteral("dvkPlaySlot3"));
    auto* name3 = child<QLabel>(panel, QStringLiteral("dvkSlotName3"));
    auto* bar3 = child<QProgressBar>(panel, QStringLiteral("dvkSlotProgress3"));
    auto* rec = child<QPushButton>(panel, QStringLiteral("dvkRecord"));
    auto* stop = child<QPushButton>(panel, QStringLiteral("dvkStop"));
    auto* play = child<QPushButton>(panel, QStringLiteral("dvkPlay"));
    auto* preview = child<QPushButton>(panel, QStringLiteral("dvkPreview"));
    auto* status = child<QLabel>(panel, QStringLiteral("dvkStatus"));
    auto* dot = child<QLabel>(panel, QStringLiteral("dvkStatusDot"));
    auto* slotsScroll = child<QScrollArea>(panel, QStringLiteral("dvkSlotsScroll"));
    report(QStringLiteral("panel widgets are reachable by name"),
           slot1 && slot3 && fkey3 && name3 && bar3 && rec && stop && play && preview && status && dot);
    if (!(slot1 && slot3 && fkey3 && name3 && bar3 && rec && stop && play && preview && status && dot)) {
        return 1;
    }

    QApplication::processEvents();
    feed(model, QStringLiteral("S1|dvk status=idle enabled=1"));
    for (int id = 1; id <= 12; ++id) {
        feed(model, QStringLiteral("S1|dvk added id=%1 name=\"Recording %1\" duration=0").arg(id));
    }
    feed(model, QStringLiteral("S1|dvk id=3 name=\"CQ Contest\" duration=4210"));
    grab(panel, QStringLiteral("1-idle"));

    // Mirror the panel's production QSplitter embedding so this is a real
    // layout squeeze, rather than a standalone widget that can ignore it.
    QWidget host;
    auto* splitter = new QSplitter(Qt::Horizontal, &host);
    panel.setFixedWidth(250);
    splitter->addWidget(&panel);
    splitter->addWidget(new QWidget);
    host.resize(1024, 300);
    splitter->setGeometry(host.rect());
    host.show();
    QApplication::processEvents();
    QApplication::processEvents();
    report(QStringLiteral("short host constrains the panel height"),
           panel.height() <= host.height());
    bool rowsContainContents = true;
    bool rowsDoNotOverlap = true;
    QRect previousRow;
    for (int id = 1; id <= 12; ++id) {
        auto* slot = child<QFrame>(panel, QStringLiteral("dvkSlot%1").arg(id));
        auto* fkey = child<QPushButton>(panel, QStringLiteral("dvkPlaySlot%1").arg(id));
        auto* name = child<QLabel>(panel, QStringLiteral("dvkSlotName%1").arg(id));
        rowsContainContents = rowsContainContents && slot && fkey && name
            && slot->rect().contains(fkey->geometry())
            && slot->rect().contains(name->geometry());
        if (slot) {
            rowsDoNotOverlap = rowsDoNotOverlap
                && (id == 1 || previousRow.bottom() < slot->geometry().top());
            previousRow = slot->geometry();
        }
    }
    report(QStringLiteral("short panel keeps every F-key and name inside its row"),
           rowsContainContents);
    report(QStringLiteral("short panel keeps slot rows from overlapping"), rowsDoNotOverlap);
    report(QStringLiteral("short panel keeps transport buttons inside the panel"),
           panel.rect().contains(rec->geometry()) && panel.rect().contains(stop->geometry())
               && panel.rect().contains(play->geometry()) && panel.rect().contains(preview->geometry()));
    report(QStringLiteral("slots have a scroll area"), slotsScroll != nullptr);
    if (slotsScroll) {
        const QString longName(DvkModel::kMaxNameBytes, QLatin1Char('W'));
        feed(model, QStringLiteral("S1|dvk id=3 name=\"%1\" duration=4210").arg(longName));
        // Include LayoutRequests posted by other LayoutRequests, rather than
        // inspecting the stale geometry immediately after the name changes.
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        auto* length3 = child<QLabel>(panel, QStringLiteral("dvkSlotLength3"));
        const QRect keyRect(fkey3->mapTo(slotsScroll->viewport(), QPoint()), fkey3->size());
        const QRect lengthRect = length3
            ? QRect(length3->mapTo(slotsScroll->viewport(), QPoint()), length3->size()) : QRect();
        report(QStringLiteral("long slot names keep the F-key and duration in the viewport"),
               length3 && name3->text() == longName
                   && slotsScroll->viewport()->rect().contains(keyRect)
                   && slotsScroll->viewport()->rect().contains(lengthRect));
        report(QStringLiteral("long slot names create no hidden horizontal scroll range"),
               slotsScroll->horizontalScrollBar()->maximum() == 0);
        feed(model, QStringLiteral("S1|dvk id=3 name=\"CQ Contest\" duration=4210"));
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        report(QStringLiteral("short panel scrolls slots vertically"),
               slotsScroll->verticalScrollBar()->maximum() > 0);
        report(QStringLiteral("slot grid never scrolls horizontally"),
               slotsScroll->horizontalScrollBar()->maximum() == 0);
        slotsScroll->verticalScrollBar()->setValue(slotsScroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        auto* slot12 = child<QFrame>(panel, QStringLiteral("dvkSlot12"));
        const QPoint slot12Top = slot12 ? slot12->mapTo(slotsScroll->viewport(), QPoint(0, 0))
                                        : QPoint();
        report(QStringLiteral("slot 12 is fully reachable by scrolling"),
               slot12 && slotsScroll->viewport()->rect().contains(QRect(slot12Top, slot12->size())));
    }

    host.resize(1024, 700);
    splitter->setGeometry(host.rect());
    QApplication::processEvents();
    QApplication::processEvents();
    if (slotsScroll) {
        report(QStringLiteral("tall panel needs no slot scrollbar"),
               slotsScroll->verticalScrollBar()->maximum() == 0);
    }
    report(QStringLiteral("tall panel restores expanding slot rows"),
           slot1->height() > slot1->minimumHeight());
    const int slot1YAtStart = slot1->y();

    // ── The row's own sheet must not reach its labels (the old QFrame leak) ──
    report(QStringLiteral("rows carry no per-widget stylesheet"), slot1->styleSheet().isEmpty());
    report(QStringLiteral("labels carry no per-widget stylesheet"), name3->styleSheet().isEmpty());

    // ── Idle, slot 1 selected (empty) ──────────────────────────────────────
    report(QStringLiteral("slot 1 starts selected"), prop(slot1, "selected") == QLatin1String("true"));
    report(QStringLiteral("an empty slot's F-key reads empty"), prop(child<QPushButton>(panel, QStringLiteral("dvkPlaySlot1")), "empty") == QLatin1String("true"));
    report(QStringLiteral("a recorded slot's F-key and name are not empty"),
           prop(fkey3, "empty") == QLatin1String("false") && prop(name3, "empty") == QLatin1String("false"));
    report(QStringLiteral("idle on an empty slot: REC on; PLAY, PREV, STOP off"),
           rec->isEnabled() && !play->isEnabled() && !preview->isEnabled() && !stop->isEnabled());
    report(QStringLiteral("idle status dot is neutral"), prop(dot, "state").isEmpty());

    // ── Select the recorded slot ───────────────────────────────────────────
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), slot3->mapToGlobal(QPointF(5, 5)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slot3, &press);
    report(QStringLiteral("clicking a row selects it and only it"),
           prop(slot3, "selected") == QLatin1String("true") && prop(slot1, "selected") == QLatin1String("false"));
    report(QStringLiteral("idle on a recorded slot: PLAY and PREV on"), play->isEnabled() && preview->isEnabled());
    grab(panel, QStringLiteral("2-selected"));

    // ── Preview: live, not on air ──────────────────────────────────────────
    feed(model, QStringLiteral("S1|dvk status=preview id=3 enabled=1"));
    report(QStringLiteral("preview: F-key is live (cyan)"), prop(fkey3, "live") == QLatin1String("live"));
    report(QStringLiteral("preview: progress is live"), prop(bar3, "mode") == QLatin1String("live") && bar3->isVisible());
    report(QStringLiteral("preview: dot is live"), prop(dot, "state") == QLatin1String("live"));
    report(QStringLiteral("preview: PREV checked, STOP on, REC and PLAY off"),
           preview->isChecked() && stop->isEnabled() && !rec->isEnabled() && !play->isEnabled());
    report(QStringLiteral("preview: status says so, without a Status: prefix"),
           status->text().startsWith(QStringLiteral("Previewing · slot 3")));
    grab(panel, QStringLiteral("3-preview"));

    // ── Playback: on air, MOX amber ────────────────────────────────────────
    feed(model, QStringLiteral("S1|dvk status=playback id=3 enabled=1"));
    report(QStringLiteral("playback: F-key is on air (amber)"), prop(fkey3, "live") == QLatin1String("air"));
    report(QStringLiteral("playback: progress is on air"), prop(bar3, "mode") == QLatin1String("air"));
    report(QStringLiteral("playback: dot is on air"), prop(dot, "state") == QLatin1String("air"));
    report(QStringLiteral("playback: PLAY checked and still enabled to stop it"), play->isChecked() && play->isEnabled());
    report(QStringLiteral("playback: status says on air"), status->text().startsWith(QStringLiteral("On air · slot 3")));
    grab(panel, QStringLiteral("4-playback"));

    // ── Recording ──────────────────────────────────────────────────────────
    feed(model, QStringLiteral("S0|dvk status=recording id=3 enabled=1"));
    report(QStringLiteral("recording: status says so"),
           status->text().startsWith(QStringLiteral("Recording · slot 3")));
    report(QStringLiteral("recording: F-key live, PLAY off"),
           prop(fkey3, "live") == QLatin1String("live") && !play->isEnabled() && rec->isChecked());
    grab(panel, QStringLiteral("5-recording"));

    // ── Back to idle ───────────────────────────────────────────────────────
    feed(model, QStringLiteral("S0|dvk status=idle enabled=1"));
    report(QStringLiteral("idle again: no live state left anywhere"),
           prop(fkey3, "live").isEmpty() && prop(dot, "state").isEmpty() && !bar3->isVisible());

    // ── A refusal reads as an error, in words ──────────────────────────────
    model.handleCommandResponse(QStringLiteral("playback_start"), 3, 0x50004001u, QString());
    report(QStringLiteral("refusal: status tone is error and says failed"),
           prop(status, "tone") == QLatin1String("error")
               && status->text().startsWith(QLatin1String("Play failed (slot 3)")));
    report(QStringLiteral("a wrapped refusal keeps the slots where they were"),
           slot1->y() == slot1YAtStart);
    grab(panel, QStringLiteral("6-refused"));
    feed(model, QStringLiteral("S1|dvk status=preview id=3 enabled=1"));
    report(QStringLiteral("the next operation clears the error tone"), prop(status, "tone").isEmpty());

    // ── Light theme renders the same states ───────────────────────────────
    tm.setActiveTheme(QStringLiteral("Default Light"));
    feed(model, QStringLiteral("S1|dvk status=playback id=3 enabled=1"));
    report(QStringLiteral("light theme keeps the on-air state"), prop(fkey3, "live") == QLatin1String("air"));
    grab(panel, QStringLiteral("7-light-playback"));

    panel.setParent(nullptr);

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
