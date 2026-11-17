// Station antenna switches in the RX ANT / TX ANT menus and fields
// (src/gui/StationAntennaMenu.h): the submenu per device VfoWidget and
// RxApplet add, and the text a field shows while a switch owns it. A hand-made view on a real
// RadioModel; no backend, no socket. Offscreen.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/station_antenna_menu_test

#include "StationAntennaMenu.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/RadioCapabilities.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/StationAntenna.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-72s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

StationAntennaView trioView()
{
    StationAntennaView view;
    view.active = true;
    view.heading = QStringLiteral("Trio");
    const QStringList labels = {QStringLiteral("EFHW"), QStringLiteral("Fan Dipole"),
                                QStringLiteral("Dummy")};
    for (int i = 0; i < labels.size(); ++i) {
        StationAntennaChoice choice;
        choice.token = QStringLiteral("TRIO:ant%1").arg(i + 1);
        choice.label = labels.at(i);
        choice.enabled = i != 2;
        choice.hint = QStringLiteral("TX and RX");
        choice.reason = QStringLiteral("Trio reports this button as disabled.");
        view.rx.append(choice);
        view.tx.append(choice);
    }
    view.rxCurrent = QStringLiteral("TRIO:ant2");
    view.txCurrent = QStringLiteral("TRIO:ant1");
    view.rxLabel = QStringLiteral("Fan Dipole");
    view.txLabel = QStringLiteral("EFHW");
    return view;
}

QAction* subMenuAction(const QMenu& menu, const QString& title)
{
    for (QAction* action : menu.actions()) {
        if (action->menu() != nullptr && action->text() == title) {
            return action;
        }
    }
    return nullptr;
}

void testSubmenuPerDevice()
{
    QMenu menu;
    menu.addAction(QStringLiteral("ANT"));
    addStationAntennaMenus(menu, {}, false);
    report("no device: the radio's ports only", menu.actions().size() == 1);

    StationAntennaView trio = trioView();
    trio.id = QStringLiteral("trio");
    trio.note = QStringLiteral("Split: RX on Fan Dipole, TX on EFHW.");
    StationAntennaView other = trioView();
    other.id = QStringLiteral("ag");
    other.heading = QStringLiteral("Other");
    addStationAntennaMenus(menu, {trio, other}, false);
    const QList<QAction*> top = menu.actions();
    report("the radio's port stays at the top level, then a separator and one submenu per device",
           top.size() == 4 && top.at(0)->text() == QStringLiteral("ANT") && top.at(1)->isSeparator()
               && top.at(2)->menu() && top.at(3)->menu(),
           std::to_string(top.size()));
    QAction* trioAction = subMenuAction(menu, QStringLiteral("Trio (station switch)"));
    report("the submenu title names the device as a station switch",
           trioAction != nullptr && subMenuAction(menu, QStringLiteral("Other (station switch)")));
    if (trioAction == nullptr) {
        return;
    }
    const QList<QAction*> actions = trioAction->menu()->actions();
    report("in it: the note, then one entry per port", actions.size() == 4,
           std::to_string(actions.size()));
    if (actions.size() != 4) {
        return;
    }
    report("the note is shown and cannot be picked",
           !actions.at(0)->isEnabled() && actions.at(0)->data().isNull());
    const auto pick = stationAntennaPickFrom(actions.at(1)->data());
    report("an entry carries the switch's label, its device and its token",
           actions.at(1)->text() == QStringLiteral("EFHW") && pick
               && pick->id == QStringLiteral("trio") && pick->token == QStringLiteral("TRIO:ant1"));
    report("a radio port's data is no station pick",
           !stationAntennaPickFrom(top.at(0)->data()) && !stationAntennaPickFrom(QStringLiteral("ANT")));
    report("the RX field's port is checked, as the device reports it",
           !actions.at(1)->isChecked() && actions.at(2)->isChecked() && !actions.at(3)->isChecked());
    report("a port that can be picked says what the pick does",
           actions.at(1)->isEnabled() && actions.at(1)->toolTip() == QStringLiteral("TX and RX"));
    report("a port that cannot is listed, disabled, with the reason",
           !actions.at(3)->isEnabled()
               && actions.at(3)->toolTip() == QStringLiteral("Trio reports this button as disabled."));

    QMenu tx;
    addStationAntennaMenus(tx, {trio}, true);
    QAction* txSub = subMenuAction(tx, QStringLiteral("Trio (station switch)"));
    report("the TX menu checks the TX half",
           txSub && txSub->menu()->actions().size() == 4 && txSub->menu()->actions().at(1)->isChecked()
               && !txSub->menu()->actions().at(2)->isChecked());

    QMenu kiwi;
    addStationAntennaMenus(kiwi, {trio}, false, false);
    QAction* kiwiSub = subMenuAction(kiwi, QStringLiteral("Trio (station switch)"));
    bool anyChecked = false;
    for (QAction* a : kiwiSub ? kiwiSub->menu()->actions() : QList<QAction*>{}) {
        anyChecked = anyChecked || a->isChecked();
    }
    report("a Kiwi receiver on the slice: no switch port is checked", kiwiSub && !anyChecked);
}

// Offline, held by another client, focus on another radio: the submenu and
// its entries stay visible, every entry disabled, with the reason.
void testUnavailableDevice()
{
    StationAntennaView trio = trioView();
    trio.id = QStringLiteral("trio");
    trio.unavailable = QStringLiteral("Another client holds the Trio's session.");
    QMenu menu;
    addStationAntennaMenus(menu, {trio}, true);
    QAction* sub = subMenuAction(menu, QStringLiteral("Trio (station switch)"));
    report("an unusable device keeps its submenu, with the reason as its tooltip",
           sub && sub->isVisible() && sub->toolTip() == trio.unavailable);
    if (!sub) {
        return;
    }
    const QList<QAction*> actions = sub->menu()->actions();
    bool allDisabled = actions.size() == 4;
    for (QAction* a : actions) {
        allDisabled = allDisabled && !a->isEnabled();
    }
    report("the reason first, then every port listed and disabled", allDisabled
               && actions.at(0)->text() == trio.unavailable
               && actions.at(1)->toolTip() == trio.unavailable);

    StationAntennaView unread = trio;
    unread.rx.clear();
    unread.tx.clear();
    unread.unavailable = QStringLiteral("The Trio has not been read yet.");
    QMenu empty;
    addStationAntennaMenus(empty, {unread}, false);
    QAction* unreadSub = subMenuAction(empty, QStringLiteral("Trio (station switch)"));
    report("no ports read yet: the submenu still says why",
           unreadSub && unreadSub->menu()->actions().size() == 1
               && unreadSub->menu()->actions().at(0)->text() == unread.unavailable);
}

// One sentence for every antenna-switch refusal while transmitting, and the
// one #6237's key objection reuses (the ruling on RFC #7238, Q3).
void testSharedSentences()
{
    report("a pick while keyed",
           stationAntennaTransmitRefusal() == QStringLiteral("Antenna can't change while transmitting."));
    report("a key refused while a switch moves",
           stationAntennaChangingKeyRefusal()
               == QStringLiteral("TX failed \u2014 Antenna is changing. Key again when it has settled."));
}

void testFieldText()
{
    RadioModel radio;
    if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
        report("fixture: a slice", false);
        return;
    }
    SliceModel* slice = radio.slice(0);
    // The fixture publishes ANT1/ANT2 each way; this radio publishes none.
    SliceDelta none;
    none.rxAntennaList = QStringList{};
    none.txAntennaList = QStringList{};
    slice->applyChanges(none);
    report("the fixture now publishes no port", slice->rxAntennaList().isEmpty()
               && slice->txAntennaList().isEmpty() && radio.antennaList().isEmpty());
    report("no source: the field is the radio's",
           !stationAntennaField(&radio, slice, false) && !stationAntennaField(&radio, slice, true));

    QObject owner;
    StationAntennaView view = trioView();
    radio.addStationAntennaSource(&owner, QStringLiteral("trio"), [&view]() { return view; },
                                  [](const QString&, bool) { return QString(); });
    const auto rx = stationAntennaField(&radio, slice, false);
    const auto tx = stationAntennaField(&radio, slice, true);
    report("a bound switch and no radio port: each field shows its half",
           rx && tx && rx->text == QStringLiteral("Fan Dipole") && tx->text == QStringLiteral("EFHW"));
    report("and its tooltip names the switch",
           tx && tx->toolTip == QStringLiteral("Transmit antenna: EFHW, on the Trio (station switch)"),
           tx ? tx->toolTip.toStdString() : std::string());
    view.unavailable = QStringLiteral("The Trio's focus is on FT710, not on this radio.");
    const auto noted = stationAntennaField(&radio, slice, false);
    report("the reason the device is unusable is added to the tooltip",
           noted && noted->toolTip.endsWith(QStringLiteral("\nThe Trio's focus is on FT710, not on this radio.")));

    SliceDelta delta;
    delta.rxAntennaList = QStringList{QStringLiteral("ANT1"), QStringLiteral("ANT2")};
    slice->applyChanges(delta);
    report("the radio publishes RX ports: the RX field is the radio's again",
           !stationAntennaField(&radio, slice, false) && stationAntennaField(&radio, slice, true));
}

// The RX ANT / TX ANT restriction in VfoWidget and RxApplet. An HL2 launched
// receive-only (canTransmit false) has one RX port, "ANT": without a switch
// there is nothing to choose; with the Trio bound, its ports are, and
// receiving must not wait on transmit permission. TX ANT stays closed.
void testFieldAvailability()
{
    RadioModel radio;
    if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
        report("fixture: a slice", false);
        return;
    }
    SliceModel* slice = radio.slice(0);
    SliceDelta delta;
    delta.rxAntennaList = QStringList{QStringLiteral("ANT")};
    slice->applyChanges(delta);

    RadioCapabilities receiveOnly;
    receiveOnly.canTransmit = false;

    report("receive-only, one RX port, no switch: RX ANT restricted",
           !rxAntennaFieldAvailable(true, receiveOnly, slice, false, false));
    report("receive-only, one RX port, the Trio bound: RX ANT enabled",
           rxAntennaFieldAvailable(true, receiveOnly, slice, false, true));
    report("receive-only with the Trio bound: TX ANT still restricted",
           !txAntennaFieldAvailable(true, receiveOnly));
    RadioCapabilities transmits;
    transmits.canTransmit = true;
    report("a radio that may transmit: both fields open",
           rxAntennaFieldAvailable(true, transmits, slice, false, false)
               && txAntennaFieldAvailable(true, transmits));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("station-antenna-menu"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();

    testSubmenuPerDevice();
    testUnavailableDevice();
    testSharedSentences();
    testFieldText();
    testFieldAvailability();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
