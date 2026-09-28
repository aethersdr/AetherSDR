// TGXL applet: what the port strips claim, and the order the STANDBY
// transition is commanded in.
//
//  * The relay-path status carries no per-port block and no trigger mode, so
//    port B's source cell is left out rather than labelled "RF SENSE" — a
//    guess that is wrong on a PTT-keyed port. Port A names the connected
//    radio while there is one and shows nothing when there is not.
//
//  * BYPASS → STANDBY is two commands. Operate goes first so every status the
//    radio reports in between already reads STANDBY (operate=0); bypass first
//    would report operate=1 bypass=0 and flash OPERATE. The model applies each
//    status exactly as reported (Principle II) — the order is what keeps the
//    display steady, not any filtering of what the radio says.
//
// Relay path only: the direct port-9010 path needs a live socket, and its
// source cell goes through the same setSourceVisible pinned in
// tgxl_panel_widgets_test.

#include "gui/AccessoryPanelWidgets.h"
#include "gui/TunerApplet.h"
#include "models/TunerModel.h"
#include "core/backends/TunerDelta.h"
#include <QLabel>

#include <QApplication>
#include <QDeadlineTimer>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

void settle(int ms = 50)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// A cell that exists but is hidden is not on the panel.
bool rowShowsVisible(const AccessoryPortRow* row, const QString& text)
{
    for (const QLabel* label : row->findChildren<QLabel*>()) {
        if (label->text() == text && !label->isHidden()) return true;
    }
    return false;
}

QPushButton* keyReading(QWidget* applet, const QString& caption)
{
    for (auto* btn : applet->findChildren<QPushButton*>()) {
        if (btn->text() == caption) return btn;
    }
    return nullptr;
}

void seed(TunerModel& model, bool operate, bool bypass)
{
    TunerDelta d;
    d.operate = operate;
    d.bypass = bypass;
    model.applyChanges(d);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    TunerModel model;
    model.setHandle(QStringLiteral("0x2000"));
    seed(model, true, false);

    TunerApplet applet;
    applet.setTunerModel(&model);
    applet.setFloating(true);           // the port strips are the expanded panel's
    applet.resize(420, 360);
    applet.show();
    settle();

    QLabel* sourceIndicator = applet.findChild<QLabel*>(QStringLiteral("tunerConnectionSource"));
    CHECK(sourceIndicator != nullptr);
    if (sourceIndicator) {
        const QPoint at = sourceIndicator->mapTo(&applet, QPoint(0, 0));
        const int bottomInset = applet.height() - at.y() - sourceIndicator->height();
        CHECK(bottomInset >= 6);
        CHECK(bottomInset < 20);
    }

    const auto rows = applet.findChildren<AccessoryPortRow*>();
    CHECK(rows.size() == 2);
    if (rows.size() != 2) return 1;
    AccessoryPortRow* portA = rows.at(0);
    AccessoryPortRow* portB = rows.at(1);

    // ── Relay-path source cells ──────────────────────────────────────────
    applet.setRadioModelName(QStringLiteral("FLEX-8600"));
    applet.setRadioConnected(true);
    settle();
    CHECK(rowShowsVisible(portA, QStringLiteral("FLEX-8600")));
    CHECK(portA->accessibleDescription().contains(QStringLiteral("FLEX-8600")));
    for (const AccessoryPortRow* row : {portA, portB}) {
        CHECK(!rowShowsVisible(row, QStringLiteral("RF SENSE")));
        CHECK(!row->accessibleDescription().contains(QStringLiteral("RF SENSE")));
    }
    // Port B's trigger mode and source are not knowable here: no cell at all,
    // not the placeholder dash and not the connected radio's name.
    CHECK(!rowShowsVisible(portB, QStringLiteral("—")));
    CHECK(!rowShowsVisible(portB, QStringLiteral("FLEX-8600")));
    CHECK(!portB->accessibleDescription().contains(QStringLiteral("FLEX-8600")));

    // Disconnected: port A no longer names a radio, visually or spoken.
    applet.setRadioConnected(false);
    settle();
    CHECK(!rowShowsVisible(portA, QStringLiteral("FLEX-8600")));
    CHECK(!rowShowsVisible(portA, QStringLiteral("NO RADIO")));
    CHECK(!portA->accessibleDescription().contains(QStringLiteral("FLEX-8600")));
    applet.setRadioConnected(true);
    settle();
    CHECK(rowShowsVisible(portA, QStringLiteral("FLEX-8600")));

    // ── BYPASS → STANDBY: operate is commanded first ─────────────────────
    QStringList wire;
    QObject::connect(&model, &TunerModel::operateRequested, &model, [&wire](bool on) {
        wire << QStringLiteral("operate=%1").arg(on ? 1 : 0);
    });
    QObject::connect(&model, &TunerModel::bypassRequested, &model, [&wire](bool on) {
        wire << QStringLiteral("bypass=%1").arg(on ? 1 : 0);
    });

    // Expanded STBY key.
    seed(model, true, true);
    settle();
    QPushButton* stby = keyReading(&applet, QStringLiteral("STBY"));
    CHECK(stby != nullptr);
    if (!stby) return 1;
    wire.clear();
    stby->click();
    CHECK(wire.size() == 2 && wire.value(0) == QStringLiteral("operate=0")
          && wire.value(1) == QStringLiteral("bypass=0"));
    if (wire.value(0) != QStringLiteral("operate=0")) {
        std::fprintf(stderr, "  wire: %s\n", qPrintable(wire.join(QStringLiteral(", "))));
    }

    // The radio's status after the first command. It is applied as reported
    // — the model is not holding bypass against it — and it already reads
    // STANDBY, so nothing in between says OPERATE.
    seed(model, false, true);
    settle();
    CHECK(!model.isOperate() && model.isBypass());
    CHECK(keyReading(&applet, QStringLiteral("STANDBY")) != nullptr);
    CHECK(keyReading(&applet, QStringLiteral("OPERATE")) == nullptr);
    seed(model, false, false);
    settle();
    CHECK(!model.isOperate() && !model.isBypass());
    CHECK(keyReading(&applet, QStringLiteral("STANDBY")) != nullptr);

    // Docked rail: the same transition through cycleOperateState.
    applet.setFloating(false);
    seed(model, true, true);
    settle();
    QPushButton* rail = keyReading(&applet, QStringLiteral("BYPASS"));
    CHECK(rail != nullptr);
    if (!rail) return 1;
    wire.clear();
    rail->click();
    CHECK(wire.size() == 2 && wire.value(0) == QStringLiteral("operate=0")
          && wire.value(1) == QStringLiteral("bypass=0"));
    if (wire.value(0) != QStringLiteral("operate=0")) {
        std::fprintf(stderr, "  wire: %s\n", qPrintable(wire.join(QStringLiteral(", "))));
    }

    if (g_failures == 0) {
        std::printf("tgxl_applet_ports_test: all checks passed\n");
        return 0;
    }
    std::printf("tgxl_applet_ports_test: %d failure(s)\n", g_failures);
    return 1;
}
