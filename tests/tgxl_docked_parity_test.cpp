// Docked/expanded parity for the TGXL applet.
//
// The panel work in this area was all done against the expanded presentation,
// which makes it easy for a behaviour to end up gated on m_floating by
// accident. The split is meant to be presentation only: the rail tile shows
// fewer things, but everything it does show behaves the same way.
//
// So this pins the docked side of the behaviours that are NOT presentation —
// the ones a reader of TunerApplet.cpp would have to check `f` for to be sure
// about. What the rail deliberately omits (the port strips, the relay dials,
// the discrete STBY/BYP keys, content scaling) is not asserted here; those are
// the UI surface and are expected to differ.

#include "gui/TunerApplet.h"
#include "models/TunerModel.h"
#include "core/backends/TunerDelta.h"

#include <QApplication>
#include <QDeadlineTimer>
#include <QLabel>
#include <QPushButton>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

void settle(int ms = 120)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// The alert banner, found the way a screen reader would.
QLabel* alertOverlay(QWidget* applet)
{
    for (auto* label : applet->findChildren<QLabel*>()) {
        if (!label->accessibleName().isEmpty()
            && label->accessibleName().contains(QStringLiteral("alert"), Qt::CaseInsensitive)) {
            return label;
        }
    }
    return nullptr;
}

bool aVisibleKeyReads(QWidget* applet, const QString& caption)
{
    for (auto* btn : applet->findChildren<QPushButton*>()) {
        if (btn->isVisible() && btn->text() == caption) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    TunerModel model;
    model.setHandle(QStringLiteral("0x2000"));

    TunerApplet applet;                 // starts docked — the rail's default
    applet.setTunerModel(&model);
    applet.resize(300, 150);
    applet.show();
    settle();

    CHECK(!applet.isFloating());

    // ── The TUNE key becomes STOP while tuning ────────────────────────────
    // The caption and the action are driven by one flag, so a rail key that
    // still said TUNE would also still start a tune — on a tuner already
    // tuning, with the transmitter keyed.
    CHECK(aVisibleKeyReads(&applet, QStringLiteral("TUNE")));
    {
        TunerDelta d; d.tuning = true;
        model.applyChanges(d);
        settle();
    }
    CHECK(aVisibleKeyReads(&applet, QStringLiteral("STOP")));
    CHECK(!aVisibleKeyReads(&applet, QStringLiteral("TUNE")));
    {
        TunerDelta d; d.tuning = false;
        model.applyChanges(d);
        settle();
    }
    CHECK(aVisibleKeyReads(&applet, QStringLiteral("TUNE")));

    // ── Tuner alerts reach the rail, full width ───────────────────────────
    QLabel* overlay = alertOverlay(&applet);
    CHECK(overlay != nullptr);
    if (!overlay) return 1;
    CHECK(!overlay->isVisible());

    emit model.alertChanged(QStringLiteral("LOW RF POWER"));
    settle();
    CHECK(overlay->isVisible());
    CHECK(overlay->text() == QLatin1String("LOW RF POWER"));
    // A banner, not a strip: it covers the tile rather than taking a row of
    // it, so a failed tune cannot be missed on a rail full of applets.
    CHECK(overlay->geometry() == applet.rect());
    const QString failureStyle = overlay->styleSheet();

    // The tuner clears it on its own schedule; nothing here second-guesses
    // that with a local timer.
    emit model.alertChanged(QString());
    settle();
    CHECK(!overlay->isVisible());

    // ── And the completion notice, in a different colour ──────────────────
    emit model.alertChanged(QStringLiteral("Tuned SWR: 1.14:1"));
    settle();
    CHECK(overlay->isVisible());
    CHECK(overlay->text() == QLatin1String("Tuned SWR: 1.14:1"));
    CHECK(overlay->geometry() == applet.rect());
    // Success must not be painted in the failure colour. The two are told
    // apart by the text, so this is the assertion that catches the rule being
    // inverted or the styling being applied before the text is classified.
    CHECK(overlay->styleSheet() != failureStyle);

    emit model.alertChanged(QString());
    settle();
    CHECK(!overlay->isVisible());

    if (g_failures == 0) {
        std::printf("tgxl_docked_parity_test: all checks passed\n");
        return 0;
    }
    std::printf("tgxl_docked_parity_test: %d failure(s)\n", g_failures);
    return 1;
}
