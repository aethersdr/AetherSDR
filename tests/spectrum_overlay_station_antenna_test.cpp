// The panadapter's RX ANT combo with a station antenna switch (#7416 review):
// a refresh asked for while the list is open is replayed when it closes, and
// a station pick does not rebuild the combo inside its own index signal.
//
// Widget only, offscreen: a real RadioModel with no backend and a hand-made
// station source; no socket, no radio, nothing keyed.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/spectrum_overlay_station_antenna_test

#include "gui/SpectrumOverlayMenu.h"
#include "models/RadioModel.h"
#include "models/StationAntenna.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static StationAntennaChoice choice(const QString& token, const QString& label)
{
    StationAntennaChoice c;
    c.token = token;
    c.label = label;
    c.enabled = true;
    return c;
}

static int rowOf(const QComboBox& combo, const QString& text)
{
    for (int i = 0; i < combo.count(); ++i) {
        if (combo.itemText(i) == text) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QWidget parent;
    parent.resize(800, 600);
    SpectrumOverlayMenu menu(&parent);
    RadioModel radio;
    menu.setRadioModel(&radio);

    StationAntennaView view;
    view.id = QStringLiteral("sw");
    view.active = true;
    view.heading = QStringLiteral("Test switch");
    view.rx = {choice(QStringLiteral("SW:1"), QStringLiteral("One")),
               choice(QStringLiteral("SW:2"), QStringLiteral("Two"))};
    view.rxCurrent = QStringLiteral("SW:1");
    view.rxLabel = QStringLiteral("One");

    QComboBox* combo = parent.findChild<QComboBox*>(QStringLiteral("panRxAntennaCombo"));
    check(combo != nullptr, "the RX ANT combo is discoverable");
    if (!combo) return 1;

    bool insidePick = false;
    bool rebuiltInsidePick = false;
    int picks = 0;
    QObject::connect(combo->model(), &QAbstractItemModel::rowsAboutToBeRemoved,
                     [&]() { if (insidePick) rebuiltInsidePick = true; });
    QObject owner;
    radio.addStationAntennaSource(&owner, view.id, [&view]() { return view; },
        [&](const QString&, bool) {
            // What a real device does when it starts a pick: say it changed.
            insidePick = true;
            ++picks;
            radio.notifyStationAntennasChanged();
            insidePick = false;
            return QString();
        });
    QCoreApplication::processEvents();
    check(rowOf(*combo, QStringLiteral("  Two")) >= 0, "the switch's ports are listed");

    // 1b: the pick's own changed() must not clear and refill the combo while
    // its currentIndexChanged is still running.
    combo->setCurrentIndex(rowOf(*combo, QStringLiteral("  Two")));
    check(picks == 1, "a station row picked: the source is asked once");
    check(!rebuiltInsidePick, "the combo is not rebuilt inside its own index signal");
    QCoreApplication::processEvents();
    check(rowOf(*combo, QStringLiteral("  Two")) >= 0 && combo->currentText() == QStringLiteral("  One"),
          "after the queued refresh: the combo shows the switch's echo, not the pick");

    // 1a: a refresh asked for while the list is open is replayed when it closes.
    parent.show();
    combo->showPopup();
    QCoreApplication::processEvents();
    check(combo->view()->isVisible(), "precondition: the list is open");
    view.rx.append(choice(QStringLiteral("SW:3"), QStringLiteral("Three")));
    radio.notifyStationAntennasChanged();
    check(rowOf(*combo, QStringLiteral("  Three")) < 0, "not rebuilt under the open list");
    combo->hidePopup();
    QCoreApplication::processEvents();
    check(!combo->view()->isVisible(), "precondition: the list is closed");
    check(rowOf(*combo, QStringLiteral("  Three")) >= 0,
          "the skipped refresh is replayed when the list closes");

    std::printf("\n%s (%d failed)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
