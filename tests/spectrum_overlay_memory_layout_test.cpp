// Widget geometry, searching and scrolling only: no backend, transport, or radio.
#include "TestSettingsProfile.h"
#include "gui/MemoryBrowsePanel.h"
#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QHeaderView>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QTest>
#include <QToolButton>
#include <QMap>
#include <QPointF>
#include <QRect>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableWidget>
#include <QWheelEvent>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
int checks = 0;
void check(const char* name, bool passed)
{
    ++checks;
    std::printf("%s %s\n", passed ? "[ OK ]" : "[FAIL]", name);
    if (!passed) {
        ++failures;
    }
}

void checkVisibleRows(QWidget& host, MemoryBrowsePanel& panel, QTableWidget& table)
{
    auto* add = panel.findChild<QPushButton*>(QStringLiteral("memoryAddButton"));
    check("Add Memory remains visible inside the host",
          add && add->isVisible() && host.rect().contains(
              QRect(add->mapTo(&host, QPoint()), add->size())));
    check("table header remains inside the host",
          host.rect().contains(QRect(table.horizontalHeader()->mapTo(&host, QPoint()),
                                    table.horizontalHeader()->size())));
    table.scrollToBottom();
    QApplication::processEvents();
    const QRect rowRect = table.visualItemRect(table.item(table.rowCount() - 1, 0));
    check("the last memory is fully reachable in the table viewport",
          !rowRect.isEmpty() && table.viewport()->rect().contains(rowRect));
    check("the last memory is fully inside the host",
          host.rect().contains(QRect(table.viewport()->mapTo(&host, rowRect.topLeft()),
                                    rowRect.size())));
}
} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("spectrum-overlay-memory-layout"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    QWidget host;
    host.resize(640, 300);
    SpectrumOverlayMenu menu(&host);
    menu.setTopLimit(22);
    menu.move(0, 30);
    QMap<int, MemoryEntry> memories;
    for (int index = 0; index < 14; ++index) {
        MemoryEntry memory;
        memory.index = index;
        memory.freq = 14.0 + index * 0.01;
        memory.name = QStringLiteral("Memory %1").arg(index + 1);
        memories.insert(index, memory);
    }
    menu.setMemories(memories);
    auto* button = menu.findChild<QPushButton*>(QStringLiteral("panMenuMemoryBtn"));
    auto* panel = host.findChild<MemoryBrowsePanel*>();
    auto* table = panel ? panel->findChild<QTableWidget*>() : nullptr;
    if (!button || !panel || !table) {
        std::fprintf(stderr, "Memory menu, drawer and table must exist\n");
        return 1;
    }
    host.show();
    button->click();
    app.processEvents();
    check("opening in a short host fits below the header",
          panel->isVisible() && panel->y() == 22 && panel->height() == 278);
    check("short drawer has a real scroll range", table->verticalScrollBar()->maximum() > 0);
    checkVisibleRows(host, *panel, *table);

    QSignalSpy recalled(&menu, &SpectrumOverlayMenu::memoryActivated);
    table->verticalScrollBar()->setValue(0);
    const QPointF point = table->viewport()->rect().center();
    QWheelEvent wheel(point, table->viewport()->mapToGlobal(point.toPoint()),
                      QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(table->viewport(), &wheel);
    check("the guarded table still scrolls with the wheel",
          table->verticalScrollBar()->value() > 0);
    check("scrolling does not recall a memory", recalled.isEmpty());

    host.resize(640, 150);
    app.processEvents();
    check("shrinking an open host relayouts the drawer", panel->y() == 22 && panel->height() == 128);
    checkVisibleRows(host, *panel, *table);

    host.resize(640, 700);
    app.processEvents();
    check("growing restores the 460px cap and original top anchor",
          panel->height() == 460 && panel->y() == menu.y());
    check("all fourteen rows fit without scrolling in a tall host",
          table->verticalScrollBar()->maximum() == 0);
    checkVisibleRows(host, *panel, *table);

    button->click();
    host.resize(640, 200);
    button->click();
    app.processEvents();
    check("reopening uses the current host height", panel->height() == 178 && panel->y() == 22);
    checkVisibleRows(host, *panel, *table);
    auto* search = panel->findChild<QLineEdit*>(QStringLiteral("memorySearchEdit"));
    auto* searchButton = panel->findChild<QToolButton*>(QStringLiteral("memorySearchButton"));
    auto* empty = panel->findChild<QLabel*>();
    if (!search || !searchButton || !empty) {
        std::fprintf(stderr, "Memory search and empty-state widgets must exist\n");
        return 1;
    }
    const int captureArg = app.arguments().indexOf(QStringLiteral("--capture-dir"));
    const QString captureDir = captureArg >= 0 ? app.arguments().value(captureArg + 1) : QString();
    const auto capture = [&](const QString& name) {
        if (!captureDir.isEmpty()) {
            QDir().mkpath(captureDir);
            check("save standalone widget screenshot", panel->grab().save(captureDir + '/' + name));
        }
    };
    check("search starts collapsed with accessible controls",
          !search->isVisible() && !searchButton->accessibleName().isEmpty()
          && !search->accessibleName().isEmpty());
    QSignalSpy added(panel, &MemoryBrowsePanel::quickAddRequested);
    searchButton->click();
    app.processEvents();
    check("search reveals below Add Memory and takes keyboard focus",
          search->isVisible() && search->hasFocus()
          && search->y() > panel->findChild<QPushButton*>(QStringLiteral("memoryAddButton"))->y());
    host.resize(640, 150);
    app.processEvents();
    check("expanded search stays inside a short drawer",
          panel->height() == 128 && host.rect().contains(QRect(search->mapTo(&host, QPoint()), search->size())));
    checkVisibleRows(host, *panel, *table);
    capture(QStringLiteral("search-short.png"));
    host.resize(640, 400);
    app.processEvents();
    QTest::keyClicks(search, "mEmOrY 14");
    check("typing immediately filters names without case sensitivity",
          table->rowCount() == 1 && table->item(0, 0)->data(Qt::UserRole).toInt() == 13);
    QTest::keyClick(search, Qt::Key_Return);
    check("typing and Enter do not recall or add a memory", recalled.isEmpty() && added.isEmpty());
    capture(QStringLiteral("search-filtered.png"));
    search->setText(QStringLiteral("no such memory"));
    app.processEvents();
    check("no matches has a distinct visible empty state",
          table->rowCount() == 0 && !table->isVisible() && empty->isVisible()
          && empty->text() == QStringLiteral("No matching memories."));
    capture(QStringLiteral("search-no-match.png"));
    search->clear();
    app.processEvents();
    check("clear restores the full list", table->isVisible() && table->rowCount() == 14);
    search->setText(QStringLiteral("Memory 14"));
    searchButton->click();
    app.processEvents();
    check("collapsing clears the hidden filter and returns focus",
          !search->isVisible() && search->text().isEmpty() && table->rowCount() == 14
          && searchButton->hasFocus());
    searchButton->click();
    app.processEvents();
    check("reopening search starts with all memories", search->isVisible() && table->rowCount() == 14);

    QMap<int, MemoryEntry> sparse;
    for (int index : {90, 7, 42}) {
        MemoryEntry entry;
        entry.index = index;
        entry.freq = index == 90 ? 14.3 : (index == 7 ? 14.1 : 14.2);
        entry.name = index == 90 ? QStringLiteral("Alpha North") :
                     (index == 42 ? QStringLiteral("Alpha South") : QString());
        entry.group = QStringLiteral("Bravo Group");
        sparse.insert(index, entry);
    }
    menu.setMemories(sparse);
    panel->focusClosestToFrequency(14.1);
    search->setText(QStringLiteral("  ALPHA  "));
    app.processEvents();
    check("filtering respects frequency order and original sparse IDs",
          table->rowCount() == 2 && table->item(0, 0)->data(Qt::UserRole).toInt() == 42
          && table->item(1, 0)->data(Qt::UserRole).toInt() == 90);
    check("refresh, clearing and collapsing never recall or add", recalled.isEmpty() && added.isEmpty());
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                     table->visualItemRect(table->item(0, 0)).center());
    check("explicit filtered-row click recalls its original memory ID",
          recalled.size() == 1 && recalled.at(0).at(0).toInt() == 42);
    recalled.clear();
    button->click();
    app.processEvents();
    check("reopening after recall preserves the visible search state",
          panel->isVisible() && search->isVisible() && table->rowCount() == 2);
    sparse[42].name = QStringLiteral("Changed");
    menu.setMemories(sparse);
    check("a memory refresh reapplies the active query",
          search->text() == QStringLiteral("  ALPHA  ") && table->rowCount() == 1
          && table->item(0, 0)->data(Qt::UserRole).toInt() == 90);
    search->clear();
    check("clearing restores an excluded closest memory",
          table->rowCount() == 3 && table->item(0, 0)->data(Qt::UserRole).toInt() == 7);
    search->setText(QStringLiteral("bravo"));
    check("search matches the displayed group fallback",
          table->rowCount() == 1 && table->item(0, 0)->data(Qt::UserRole).toInt() == 7);
    sparse[7].group.clear();
    menu.setMemories(sparse);
    search->setText(QStringLiteral("Memory 7"));
    check("search matches the displayed unnamed-memory fallback",
          table->rowCount() == 1 && table->item(0, 0)->data(Qt::UserRole).toInt() == 7);
    menu.setMemories({});
    app.processEvents();
    check("empty drawer remains contained", host.rect().contains(panel->geometry()));
    check("empty drawer keeps Add Memory visible",
          panel->findChild<QPushButton*>(QStringLiteral("memoryAddButton"))->isVisible());
    check("an empty bank is distinct from an unmatched query",
          empty->isVisible() && empty->text() == QStringLiteral("No memories are available yet."));
    check("refreshes and fallback filtering do not recall or add", recalled.isEmpty() && added.isEmpty());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
