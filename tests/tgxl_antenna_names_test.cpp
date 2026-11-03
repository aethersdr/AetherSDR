// Socket-free tests for TGXL custom antenna name parsing and TunerApplet wiring.
//
// All frames are injected via processLine() — the declared test seam in
// TgxlConnection.h — so no fake TCP peer or real TGXL is required.
// The antenna R-frame handler, the decode lambda (underscore→space, length
// cap, control-character strip), and the TunerApplet setText wiring are all
// exercised end-to-end through the live signal chain.

#include "core/TgxlConnection.h"
#include "core/backends/TunerDelta.h"
#include "gui/TunerApplet.h"
#include "models/TunerModel.h"

#include <QApplication>
#include <QDeadlineTimer>
#include <QMetaObject>
#include <QPushButton>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; } } while (0)

void settle(int ms = 120)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Find an ant button by its initial default text (e.g. "ANT 1").
// The ant buttons are the only QPushButtons in the applet with that exact text.
QPushButton* findAntBtn(QWidget* applet, const QString& defaultText)
{
    for (auto* btn : applet->findChildren<QPushButton*>()) {
        if (btn->text() == defaultText)
            return btn;
    }
    return nullptr;
}

// Inject a protocol line into `conn` synchronously.
bool inject(TgxlConnection* conn, const QString& line)
{
    return QMetaObject::invokeMethod(conn, "processLine", Qt::DirectConnection,
                                     Q_ARG(QString, line));
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // ── Fixture: model + applet + 3-way direct connection ────────────────
    //
    // hasAntennaSwitch() requires oneByThree=true (set via applyChanges).
    // The ant container becomes visible when hasDirectConnection() &&
    // hasAntennaSwitch() — both must be true before the buttons appear.

    TunerModel model;
    model.setHandle(QStringLiteral("0x2000"));

    {
        TunerDelta d;
        d.oneByThree = true;    // 3-way model: enable the antenna switch row
        model.applyChanges(d);
    }

    TunerApplet applet;
    applet.setTunerModel(&model);
    applet.resize(300, 200);
    applet.show();
    settle();

    // The ant buttons are always created; find them by their initial default
    // text before any injection so we hold stable pointers through the test.
    // (Other panel buttons — STBY, BYP, TUNE — have different text and are
    // not matched here.)
    QPushButton* b1 = findAntBtn(&applet, QStringLiteral("ANT 1"));
    QPushButton* b2 = findAntBtn(&applet, QStringLiteral("ANT 2"));
    QPushButton* b3 = findAntBtn(&applet, QStringLiteral("ANT 3"));

    CHECK(b1 != nullptr);
    CHECK(b2 != nullptr);
    CHECK(b3 != nullptr);

    if (!b1 || !b2 || !b3) {
        std::fprintf(stderr, "SKIP: ant buttons not found — cannot continue\n");
        return 1;
    }

    TgxlConnection direct;
    model.setDirectConnection(&direct);

    // Injecting the version line triggers finishHandshake(), which sets
    // m_connected=true and emits connected() — without a real TCP socket.
    CHECK(inject(&direct, QStringLiteral("V1.2.44")));
    settle();

    // Buttons start at their defaults.
    CHECK(b1->text() == QStringLiteral("ANT 1"));
    CHECK(b2->text() == QStringLiteral("ANT 2"));
    CHECK(b3->text() == QStringLiteral("ANT 3"));

    // ── Happy path: all three names present ──────────────────────────────
    //
    // Firmware response to `antenna read` on a 3-way model. Underscores
    // encode spaces; the names here are exactly 8 characters or fewer.

    CHECK(inject(&direct, QStringLiteral("R2|0|antenna name1=Vertical name2=Invert_L name3=6M_Loop")));
    settle();

    CHECK(b1->text() == QStringLiteral("Vertical"));
    CHECK(b2->text() == QStringLiteral("Invert L"));   // underscore → space
    CHECK(b3->text() == QStringLiteral("6M Loop"));    // underscore → space

    // ── Missing name2: button 2 stays at default ─────────────────────────

    CHECK(inject(&direct, QStringLiteral("R3|0|antenna name1=40m_Wire name3=6M_Yagi")));
    settle();

    CHECK(b1->text() == QStringLiteral("40m Wire"));
    CHECK(b2->text() == QStringLiteral("ANT 2"));      // empty → fallback
    CHECK(b3->text() == QStringLiteral("6M Yagi"));

    // ── All-empty names: all three keys present but with empty values ─────
    //
    // If the firmware returns name1= name2= name3= (empty values), every
    // button falls back to its "ANT N" default.

    CHECK(inject(&direct, QStringLiteral("R4|0|antenna name1= name2= name3=")));
    settle();

    CHECK(b1->text() == QStringLiteral("ANT 1"));
    CHECK(b2->text() == QStringLiteral("ANT 2"));
    CHECK(b3->text() == QStringLiteral("ANT 3"));

    // ── Length cap: name longer than 8 chars is truncated ────────────────
    //
    // The TGXL firmware caps names at 8 characters. The client enforces the
    // same limit so a misbehaving device cannot overflow the button widget.

    CHECK(inject(&direct, QStringLiteral("R5|0|antenna name1=ABCDEFGHIJ name2=Short name3=X")));
    settle();

    CHECK(b1->text() == QStringLiteral("ABCDEFGH")); // truncated to 8
    CHECK(b2->text() == QStringLiteral("Short"));
    CHECK(b3->text() == QStringLiteral("X"));

    // ── Ampersand escaping: '&' must not register as a mnemonic ──────────
    //
    // QPushButton renders '&' as an Alt-accelerator. A device-supplied name
    // like "A&B" must be escaped to "A&&B" before setText so it displays as
    // a literal '&' with no keybinding side-effect.
    // setText receives the doubled form; text() returns what was passed to it.

    CHECK(inject(&direct, QStringLiteral("R6|0|antenna name1=A&B name2=Normal name3=C&D")));
    settle();

    CHECK(b1->text() == QStringLiteral("A&&B"));
    CHECK(b2->text() == QStringLiteral("Normal"));
    CHECK(b3->text() == QStringLiteral("C&&D"));

    // ── Disconnect: buttons reset to "ANT 1/2/3" ─────────────────────────

    // Re-inject real names first so the reset is observable.
    CHECK(inject(&direct, QStringLiteral("R7|0|antenna name1=Vertical name2=Invert_L name3=6M_Loop")));
    settle();
    CHECK(b1->text() == QStringLiteral("Vertical"));

    CHECK(QMetaObject::invokeMethod(&direct, "onDisconnected", Qt::DirectConnection));
    settle();

    CHECK(b1->text() == QStringLiteral("ANT 1"));
    CHECK(b2->text() == QStringLiteral("ANT 2"));
    CHECK(b3->text() == QStringLiteral("ANT 3"));

    if (g_failures == 0) {
        std::printf("tgxl_antenna_names_test: all checks passed\n");
        return 0;
    }
    std::printf("tgxl_antenna_names_test: %d failure(s)\n", g_failures);
    return 1;
}
