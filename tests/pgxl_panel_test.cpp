// The PGXL front-panel presentation — what AmpApplet switches to when it is
// popped out or placed on the workspace canvas.
//
// The split is presentation only: both presentations read the same AmpModel,
// so what is pinned here is which controls each one shows, and that the port
// strips report what the amplifier actually said rather than a plausible
// filling-in.
//
// Driven through a stub amplifier on loopback, because the per-port block only
// exists on the direct port-9008 status — the radio-relayed object carries
// none of it.

#include "gui/AmpApplet.h"
#include "gui/AccessoryPanelWidgets.h"
#include "models/AmpModel.h"
#include "core/PgxlConnection.h"
#include "core/backends/AmpDelta.h"

#include <QApplication>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

bool spin(std::function<bool()> done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

// The status reply, verbatim off a PGXL on firmware 3.8.9, with the state
// word substituted.
QByteArray statusReply(const char* state)
{
    return QByteArray("R9|0|state=") + state +
        " bandA=40 bandB=0 bsrcA=FLEX bsrcB=FLEX flexA=FLEX-8600 flexB=FLEX-8600"
        " vac=245 vdd=51.9 id=39.0 fwd=1148.0 swr=-20.0 temp=22.4 hltemp=23.0"
        " biasA=RADIO_AAB biasB=RADIO_AB fanmode=STANDARD meffa=STANDBY\n";
}

// The strip keeps its readings in child labels; find one by the text it shows.
bool rowShows(const AccessoryPortRow* row, const QString& text)
{
    const auto labels = row->findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text() == text) return true;
    }
    return false;
}

// A cell that exists but is hidden is not on the panel.
bool rowShowsVisible(const AccessoryPortRow* row, const QString& text)
{
    const auto labels = row->findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text() == text && !label->isHidden()) return true;
    }
    return false;
}

QLabel* standbyBanner(AmpApplet& applet)
{
    const auto labels = applet.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == QStringLiteral("STANDBY")) return label;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "No loopback bind available — skipping\n");
        return 77;   // tests.cmake maps 77 to SKIP
    }

    PgxlConnection conn;
    AmpModel model;
    model.setDirectConnection(&conn);

    // The relayed object supplies the antenna → output map; the direct
    // connection supplies the per-port block. The panel needs both.
    AmpDelta d;
    d.handle = QStringLiteral("0x6F21EAB0");
    d.detectedModel = QStringLiteral("PowerGeniusXL");
    d.ip = QStringLiteral("127.0.0.1");
    d.operate = true;
    d.telemetry.insert(QStringLiteral("ant"), QStringLiteral("ANT1:PORTA,ANT2:PORTB"));
    model.applyChanges(d);

    conn.connectToPgxl(QStringLiteral("127.0.0.1"), server.serverPort());
    CHECK(spin([&] { return server.hasPendingConnections(); }));
    QTcpSocket* peer = server.nextPendingConnection();
    if (!peer) return 1;
    peer->write("V3.8.9\n");
    peer->flush();
    CHECK(spin([&] { return conn.isConnected(); }));
    peer->write(statusReply("IDLE"));
    peer->flush();
    CHECK(spin([&] { return model.hasPortInfo(); }));

    AmpApplet applet;
    applet.setAmpModel(&model);
    applet.setDirectConnected(true);
    applet.resize(560, 380);
    applet.show();
    QCoreApplication::processEvents();

    const auto rows = applet.findChildren<AccessoryPortRow*>();
    CHECK(rows.size() == 2);
    if (rows.size() != 2) return 1;
    AccessoryPortRow* portA = rows.at(0);
    AccessoryPortRow* portB = rows.at(1);

    // ── Docked: the compact tile ──────────────────────────────────────
    //
    // The rail stacks every applet at one width, so the strips are not on it
    // — there is no room for them without squeezing the gauges that are the
    // reason the tile exists.
    // isVisible(), not isHidden(): the strips are taken off the panel by
    // hiding the box that holds them, so their own hidden flag never moves.
    CHECK(!applet.isFloating());
    CHECK(!portA->isVisible());
    CHECK(!portB->isVisible());

    // Fan speed only exists on the direct connection. Until the amplifier has
    // reported a mode, neither fan control is up: one that cannot say what it
    // is set to is worse than none.
    QComboBox* fanCombo = applet.findChild<QComboBox*>(QStringLiteral("ampFanModeCombo"));
    CHECK(fanCombo != nullptr);
    if (fanCombo) CHECK(!fanCombo->isVisible());

    // ── Expanded ──────────────────────────────────────────────────────
    applet.setFloating(true);
    QCoreApplication::processEvents();
    CHECK(portA->isVisible());
    CHECK(portB->isVisible());

    // Exactly one operate control is up at a time, and it is the one that
    // belongs to the presentation.
    {
        QPushButton* rail = nullptr;
        PanelKey* key = nullptr;
        for (QPushButton* btn : applet.findChildren<QPushButton*>()) {
            if (auto* panelKey = qobject_cast<PanelKey*>(btn)) key = panelKey;
            else if (btn->text() == QStringLiteral("OPERATE")
                     || btn->text() == QStringLiteral("STANDBY")) rail = btn;
        }
        CHECK(key != nullptr);
        CHECK(rail != nullptr);
        if (key && rail) {
            CHECK(!key->isHidden());
            CHECK(rail->isHidden());

            // The key commands the state the amplifier is NOT in. Operating,
            // it asks for standby.
            QSignalSpy operate(&applet, &AmpApplet::operateToggled);
            key->click();
            CHECK(operate.count() == 1);
            if (operate.count() == 1) {
                CHECK(operate.takeFirst().at(0).toBool() == false);
            }
        }
    }

    // ── Fan speed ─────────────────────────────────────────────────────
    //
    // The rail keeps the pull-down (#3905 — three modes listed rather than
    // clicked through blind); the panel gets a one-letter key, because the
    // control row there is keys. Exactly one is up at a time.
    {
        PanelKey* fanKey = nullptr;
        PanelKey* stbyKey = nullptr;
        for (PanelKey* k : applet.findChildren<PanelKey*>()) {
            if (k->accessibleName().contains(QStringLiteral("Fan"))) fanKey = k;
            else stbyKey = k;
        }
        CHECK(fanKey != nullptr);
        CHECK(stbyKey != nullptr);
        if (fanKey && stbyKey && fanCombo) {
            // No mode reported yet — still nothing up, in either presentation.
            CHECK(!fanKey->isVisible());

            applet.setFanMode(QStringLiteral("STANDARD"));
            QCoreApplication::processEvents();
            CHECK(fanKey->isVisible());
            CHECK(!fanCombo->isVisible());   // the rail's control, not the panel's
            CHECK(fanKey->text() == QStringLiteral("S"));
            // The letter is the caption, not the whole story: the mode's name
            // is on the tooltip and in the accessible name, so nothing is
            // available only as an initial.
            CHECK(fanKey->accessibleName().contains(QStringLiteral("STANDARD")));
            CHECK(fanKey->toolTip().contains(QStringLiteral("STANDARD")));

            // Square, and exactly as tall as the key beside it.
            CHECK(fanKey->sizeHint().width() == fanKey->sizeHint().height());
            CHECK(fanKey->sizeHint().height() == stbyKey->sizeHint().height());

            // One press cycles to the next mode and commands it once.
            QSignalSpy fan(&applet, &AmpApplet::fanModeChanged);
            fanKey->click();
            QCoreApplication::processEvents();
            CHECK(fan.count() == 1);
            if (fan.count() == 1) {
                CHECK(fan.takeFirst().at(0).toString() == QStringLiteral("CONTEST"));
            }
            CHECK(fanKey->text() == QStringLiteral("C"));
            fanKey->click();
            CHECK(fanKey->text() == QStringLiteral("B"));
            // And wraps, so every mode is reachable from every other.
            fanKey->click();
            CHECK(fanKey->text() == QStringLiteral("S"));

            // The key and the pull-down are two faces of one mode, so a status
            // from the amplifier moves both — and must not echo a command back.
            QSignalSpy echo(&applet, &AmpApplet::fanModeChanged);
            applet.setFanMode(QStringLiteral("BROADCAST"));
            CHECK(echo.count() == 0);
            CHECK(fanKey->text() == QStringLiteral("B"));
            CHECK(fanCombo->currentData().toString() == QStringLiteral("BROADCAST"));
            applet.setFanMode(QStringLiteral("STANDARD"));
        }
    }

    // ── What the strips show ──────────────────────────────────────────
    //
    // Port A is on 40m with the AAB bias profile, fed by a FLEX-8600. Port B
    // has no band, which is how the amplifier reports a port nothing is
    // driving — so the band cell reads N/A while the configuration cells
    // still describe how the port is set up.
    CHECK(rowShows(portA, QStringLiteral("40")));
    CHECK(rowShows(portA, QStringLiteral("AAB")));
    CHECK(rowShows(portA, QStringLiteral("FLEX-8600")));
    CHECK(rowShows(portB, QStringLiteral("N/A")));
    CHECK(rowShows(portB, QStringLiteral("AB")));
    CHECK(rowShows(portB, QStringLiteral("FLEX-8600")));

    // The frequency cell is not on an amplifier's strip at all. The PGXL
    // reports no frequency per port, and a cell standing at N/A forever would
    // say a reading is missing rather than that there is none to take. The
    // tuner's strips, which do have one, keep it.
    for (const AccessoryPortRow* row : {portA, portB}) {
        const auto labels = row->findChildren<QLabel*>();
        int visibleNa = 0;
        for (const QLabel* label : labels) {
            if (label->text() == QStringLiteral("N/A") && !label->isHidden()) ++visibleNa;
        }
        // Port B's band is the only N/A that may be showing; port A has none.
        CHECK(visibleNa <= 1);
    }
    CHECK(!rowShowsVisible(portA, QStringLiteral("N/A")));

    // ── Which port transmits ──────────────────────────────────────────
    //
    // From the amplifier's antenna → output map, not from the state word:
    // the word only distinguishes the ports once RF is already flowing.
    applet.setTxAntenna(QStringLiteral("ANT1"));
    QCoreApplication::processEvents();
    CHECK(portA->accessibleDescription().contains(QStringLiteral("transmit port")));
    CHECK(!portB->accessibleDescription().contains(QStringLiteral("transmit port")));

    applet.setTxAntenna(QStringLiteral("ANT2"));
    QCoreApplication::processEvents();
    CHECK(!portA->accessibleDescription().contains(QStringLiteral("transmit port")));
    CHECK(portB->accessibleDescription().contains(QStringLiteral("transmit port")));

    // An antenna that does not run through the amplifier outlines neither
    // port. Outlining one would claim RF passes through it.
    applet.setTxAntenna(QStringLiteral("XVTR"));
    QCoreApplication::processEvents();
    CHECK(!portA->accessibleDescription().contains(QStringLiteral("transmit port")));
    CHECK(!portB->accessibleDescription().contains(QStringLiteral("transmit port")));

    // ── The state cell speaks only when it has something to say ───────
    //
    // Operating is the normal condition and carries no word for it — the
    // amplifier's own panel has none — and keying is already on the PTT lamp,
    // so neither puts anything in the cell.
    CHECK(!rowShowsVisible(portA, QStringLiteral("OPR")));
    peer->write(statusReply("TRANSMIT_A"));
    peer->flush();
    CHECK(spin([&] { return model.portA().ptt; }));
    QCoreApplication::processEvents();
    CHECK(!rowShowsVisible(portA, QStringLiteral("TX")));
    CHECK(!rowShowsVisible(portB, QStringLiteral("OPR")));

    // A fault does: it is the one thing on this strip an operator has to act
    // on, and it is named rather than left to the absence of a word.
    peer->write(statusReply("FAULT"));
    peer->flush();
    CHECK(spin([&] { return model.stateText() == QLatin1String("FAULT"); }));
    QCoreApplication::processEvents();
    CHECK(rowShowsVisible(portA, QStringLiteral("FAULT")));
    // And a fault is NOT standby. Read as one, the banner covers the strips
    // and tells the operator the amplifier is out of circuit by choice at the
    // moment it has tripped — hiding the cell that just said FAULT.
    CHECK(portA->isVisible());
    {
        QLabel* banner = standbyBanner(applet);
        if (banner) CHECK(!banner->isVisible());
    }

    // ── Standby ───────────────────────────────────────────────────────
    //
    // Out of circuit there is no per-port reading left, so the banner takes
    // the whole area rather than leaving two strips of stale cells up.
    peer->write(statusReply("STANDBY"));
    peer->flush();
    CHECK(spin([&] { return !rows.at(0)->isVisible(); }));
    QLabel* banner = standbyBanner(applet);
    CHECK(banner != nullptr);
    if (banner) CHECK(banner->isVisible());

    // The banner replaces the strips beside the keys, never the keys: pressing
    // STBY is how the amplifier comes back out of standby, so it has to
    // survive the state it is the exit from.
    for (PanelKey* k : applet.findChildren<PanelKey*>()) {
        CHECK(k->isVisible());
    }

    {
        // In standby the key asks for operate — the opposite of what it asked
        // for a moment ago, from the same press.
        PanelKey* key = nullptr;
        for (PanelKey* k : applet.findChildren<PanelKey*>()) {
            if (!k->accessibleName().contains(QStringLiteral("Fan"))) key = k;
        }
        CHECK(key != nullptr);
        if (key) {
            QSignalSpy operate(&applet, &AmpApplet::operateToggled);
            key->click();
            CHECK(operate.count() == 1);
            if (operate.count() == 1) {
                CHECK(operate.takeFirst().at(0).toBool() == true);
            }
        }
    }

    // ── The readouts reflow with the presentation ─────────────────────
    //
    // One row along the bottom on the panel, stacked in the rail — which is
    // one tile wide and has nowhere to put four readings abreast. Same
    // widgets either way; nothing is reparented between presentations.
    {
        QPushButton* temp = applet.findChild<QPushButton*>(QStringLiteral("ampTempUnitButton"));
        QLabel* vdd = nullptr;
        for (QLabel* l : applet.findChildren<QLabel*>()) {
            if (l->text().startsWith(QStringLiteral("Vdd"))) vdd = l;
        }
        CHECK(temp != nullptr);
        CHECK(vdd != nullptr);
        if (temp && vdd) {
            CHECK(applet.isFloating());
            CHECK(temp->y() == vdd->y());        // abreast
            CHECK(temp->x() < vdd->x());

            applet.setFloating(false);
            QCoreApplication::processEvents();
            CHECK(temp->y() < vdd->y());         // stacked
            CHECK(temp->x() == vdd->x());
            // Still the same widgets, still inside the applet.
            CHECK(temp->parentWidget() == vdd->parentWidget());

            applet.setFloating(true);
            QCoreApplication::processEvents();
            CHECK(temp->y() == vdd->y());
        }
    }

    // ── The floor does not ratchet ────────────────────────────────────
    //
    // The minimum comes from the minimum scale, not from the children the
    // current scale has just sized. Derived from the layout instead, a panel
    // enlarged once could never be made small again.
    const QSize floorBefore = applet.minimumSizeHint();
    applet.resize(900, 700);
    QCoreApplication::processEvents();
    CHECK(applet.minimumSizeHint() == floorBefore);
    applet.resize(360, 260);
    QCoreApplication::processEvents();
    CHECK(applet.minimumSizeHint() == floorBefore);
    CHECK(applet.size().width() == 360 && applet.size().height() == 260);

    // ── The alert banner ──────────────────────────────────────────────
    peer->write("M|PA OVERTEMP\n");
    peer->flush();
    CHECK(spin([&] { return !model.alert().isEmpty(); }));
    QCoreApplication::processEvents();
    {
        QLabel* overlay = nullptr;
        for (QLabel* label : applet.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("PA OVERTEMP")) overlay = label;
        }
        CHECK(overlay != nullptr);
        if (overlay) {
            CHECK(!overlay->isHidden());
            // It covers the applet rather than sharing a row with the
            // readings: a fault is missable tucked above them.
            CHECK(overlay->geometry() == applet.rect());
            // Device text, rendered literally. AutoText would treat anything
            // markup-shaped as rich text and fetch a remote <img> from it.
            CHECK(overlay->textFormat() == Qt::PlainText);
        }
    }

    // The amplifier clears it on its own schedule; there is no local timer to
    // get out of step with the device.
    peer->write("M|\n");
    peer->flush();
    CHECK(spin([&] { return model.alert().isEmpty(); }));
    QCoreApplication::processEvents();
    for (QLabel* label : applet.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("PA OVERTEMP")) CHECK(label->isHidden());
    }

    // ── Losing the amplifier ──────────────────────────────────────────
    //
    // The strips stop being refreshed, so they are emptied rather than left
    // claiming a band the amplifier may have moved off.
    peer->close();
    CHECK(spin([&] { return !model.hasPortInfo(); }));
    QCoreApplication::processEvents();
    CHECK(!rowShows(portA, QStringLiteral("40")));
    CHECK(!rowShows(portA, QStringLiteral("AAB")));

    // Fan speed goes with it: it is commandable only over the direct
    // connection, so the key comes down rather than standing there unable to
    // do anything.
    applet.setDirectConnected(false);
    QCoreApplication::processEvents();
    for (PanelKey* k : applet.findChildren<PanelKey*>()) {
        if (k->accessibleName().contains(QStringLiteral("Fan")))
            CHECK(!k->isVisible());
    }

    conn.disconnect();

    if (g_failures == 0) {
        std::printf("pgxl_panel_test: all checks passed\n");
        return 0;
    }
    std::printf("pgxl_panel_test: %d failure(s)\n", g_failures);
    return 1;
}
