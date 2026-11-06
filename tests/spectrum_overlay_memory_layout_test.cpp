// Widget geometry and scrolling only: no backend, transport, or radio.
#include "TestSettingsProfile.h"
#include "gui/MemoryBrowsePanel.h"
#include "gui/SpectrumOverlayMenu.h"

#include <QApplication>
#include <QHeaderView>
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
    menu.setMemories({});
    app.processEvents();
    check("empty drawer remains contained", host.rect().contains(panel->geometry()));
    check("empty drawer keeps Add Memory visible",
          panel->findChild<QPushButton*>(QStringLiteral("memoryAddButton"))->isVisible());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
