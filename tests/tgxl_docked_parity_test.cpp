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
#include <QFontMetrics>
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

// Spins until done() or the deadline, returning done()'s last answer.
bool spin(std::function<bool()> done, int timeoutMs = 3000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
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

    // Ending a tune on this model raises the relay-path completion notice
    // (asserted properly further down). Drain it — waiting only for "not
    // visible" would return instantly, before the notice has even been
    // raised, and it would then appear in the middle of the next section.
    QLabel* overlay = alertOverlay(&applet);
    CHECK(overlay != nullptr);
    if (!overlay) return 1;
    CHECK(spin([&] { return overlay->isVisible(); }, 3000));
    CHECK(spin([&] { return !overlay->isVisible(); }, 5000));

    // ── Every state word fits the rail's button, at every rail width ─────
    //
    // The rail's captions are the tuner's own state words and the rail's width
    // belongs to the applet panel, not to this applet — so the caption is
    // shrunk to fit the button it actually gets rather than sized for the rail
    // widths that happen to be tested. "OPERATE" was drawn as "OPERATI" on a
    // narrower rail than the one it was checked against.
    //
    // Measured bold, because bold is what the style sheet draws: the widget's
    // own font is not, and measuring that reports a caption several pixels
    // narrower than the one on screen.
    {
        const char* words[] = {"OPERATE", "BYPASS", "STANDBY"};
        const bool operateFlag[] = {true, true, false};
        const bool bypassFlag[] = {false, true, false};

        for (int railWidth : {150, 180, 240, 300}) {
            applet.resize(railWidth, 150);
            settle();
            for (int i = 0; i < 3; ++i) {
                TunerDelta d;
                d.operate = operateFlag[i];
                d.bypass = bypassFlag[i];
                model.applyChanges(d);
                settle();

                bool found = false;
                for (auto* btn : applet.findChildren<QPushButton*>()) {
                    if (!btn->isVisible() || btn->text() != QLatin1String(words[i])) continue;
                    found = true;

                    // Measured off the size in the style sheet, not off
                    // btn->font(): the fitted size is applied through the
                    // sheet because a sheet's font-size beats setFont, so the
                    // widget's font no longer reports what is drawn. Reading
                    // it here would test the wrong number and pass while the
                    // caption clipped.
                    const QString sheet = btn->styleSheet();
                    const int at = sheet.indexOf(QStringLiteral("font-size:"));
                    CHECK(at >= 0);
                    if (at < 0) continue;
                    const int pixels = sheet.mid(at + 10).trimmed()
                                            .split(QLatin1Char('p')).first().toInt();
                    CHECK(pixels > 0);

                    // WHAT THIS GUARDS, AND WHY IT IS NOT "THE CAPTION FITS".
                    //
                    // The regression was "OPERATE" drawn as "OPERATI":
                    // fittedRailFontPx() left a size on the button that was too
                    // large for it. What catches that is the FIT half below,
                    // qualified by the clamp. MAXIMALITY catches the opposite
                    // defect — a caption shrunk further than it needed to be —
                    // which the old assertion could not see at all. Both
                    // re-evaluate the fitter's own predicate rather than an
                    // absolute width, so they hold whatever fonts the machine
                    // has.
                    //
                    // Asserting the caption fits OUTRIGHT does not hold
                    // everywhere, and asserting it turned main's full suite red
                    // on every run from #5676 until this. fittedRailFontPx()
                    // searches [kRailCaptionMinPx, kRailCaptionMaxPx] and
                    // CLAMPS to the minimum when nothing in range fits —
                    // deliberately, because a caption shrunk past 7px is not
                    // readable. With the project's fonts present the floor is
                    // never reached; in a container falling back to whatever is
                    // installed it is, and the button then legitimately
                    // overflows. CI reported exactly that, one line above the
                    // failure: "ThemeManager: Default Dark also failed to load
                    // — UI will render with compiled-in defaults". That is a
                    // rendering-environment fact, not a defect in the fitter,
                    // and a test that cannot tell them apart reports the wrong
                    // one.
                    //
                    // Mirrors of TunerApplet.cpp's own constants — they live
                    // in an anonymous namespace there, so there is nothing to
                    // include. The padding is the fitter's, not a looser
                    // number, so "fits" here and "fits" there are the same
                    // predicate. Drift is only loud in one direction: if
                    // kRailCaptionPadding DECREASES without a matching edit
                    // here, this copy is tighter than the fitter and the checks
                    // below fail. An INCREASE goes the quiet way — this copy
                    // becomes looser, and drift is then caught only if some
                    // caption happens to land in the gap.
                    constexpr int kFitMinPx = 7;     // kRailCaptionMinPx
                    constexpr int kFitMaxPx = 10;    // kRailCaptionMaxPx
                    constexpr int kFitPadding = 8;   // kRailCaptionPadding
                    CHECK(pixels >= kFitMinPx && pixels <= kFitMaxPx);

                    QFont drawn = btn->font();
                    drawn.setBold(true);      // the sheet draws these bold
                    drawn.setPixelSize(pixels);
                    const int available = btn->width() - kFitPadding;
                    const bool fits =
                        QFontMetrics(drawn).horizontalAdvance(btn->text())
                        <= available;

                    // Both checks re-derive the fitter's own arithmetic on the
                    // same button at the same width, so what they pin is its
                    // CONTRACT, not the rendered pixels. They catch a sheet
                    // that disagrees with fittedRailFontPx() and a fitter that
                    // stops honouring its own search; they are not an
                    // independent "nothing clips on screen" check. That check
                    // is exactly what is unavailable in a container without the
                    // project's fonts — which is what made the outright-fit
                    // assertion red.
                    //
                    // Either it fits, or the fitter exhausted its range and
                    // clamped. Never a size it could still have shrunk.
                    CHECK(fits || pixels == kFitMinPx);

                    // MAXIMALITY, which catches the opposite defect: the
                    // fitter shrinking the caption further than it needed to.
                    // (The clipping regression this test exists for — a caption
                    // too LARGE for its button — is caught above, by `fits`.)
                    // One size larger must NOT fit, or the fitter stopped
                    // short.
                    if (pixels < kFitMaxPx) {
                        QFont bigger = drawn;
                        bigger.setPixelSize(pixels + 1);
                        CHECK(QFontMetrics(bigger).horizontalAdvance(btn->text())
                              > available);
                    }
                }
                CHECK(found);   // the full word, not an abbreviation
            }
        }
        applet.resize(300, 150);
        TunerDelta d; d.operate = true; d.bypass = false;
        model.applyChanges(d);
        settle();
    }

    // ── Tuner alerts reach the rail, full width ───────────────────────────
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

    // ── Device text is never markup ──────────────────────────────────────
    //
    // The banner shows the body of an M| frame verbatim. Under QLabel's
    // AutoText a tuner sending something markup-shaped would have it rendered
    // as rich text — and a remote <img> would be fetched. The frame is device
    // input, so it is displayed literally (Principle VII).
    {
        CHECK(overlay->textFormat() == Qt::PlainText);
        const QString hostile =
            QStringLiteral("<img src=http://example.invalid/x.png> LOW RF POWER");
        emit model.alertChanged(hostile);
        settle();
        CHECK(overlay->isVisible());
        // Held as given, not parsed into an element.
        CHECK(overlay->text() == hostile);
        CHECK(overlay->textFormat() == Qt::PlainText);
        emit model.alertChanged(QString());
        settle();
        CHECK(!overlay->isVisible());
    }

    // ── A relay-only station still gets the tune result ──────────────────
    //
    // The tuner's alert channel exists only on the direct connection: the
    // relayed object carries no message, result or SWR field, and the radio's
    // own atu status stays TUNE_MANUAL_BYPASS through a TGXL tune because the
    // TGXL is the one tuning. Reporting the result only through that channel
    // would leave every relay-only station with nothing after a tune, where
    // before it had the figure on the key.
    //
    // This model has no direct connection, which is exactly that station.
    {
        // Through the relay entry point, not the blind one: updateMeters is
        // private now precisely so the wiring cannot reach it, and this
        // relay-only station is the case setRadioMeters names.
        applet.setRadioMeters(60.0f, 1.42f);    // a settled reading
        {
            TunerDelta d; d.tuning = true;
            model.applyChanges(d);
            settle();
        }
        // Nothing yet — the tune is still running.
        CHECK(!overlay->isVisible());
        {
            TunerDelta d; d.tuning = false;
            model.applyChanges(d);
        }
        // The notice waits for the settled SWR rather than reading the meters
        // at the instant the tune ended.
        CHECK(spin([&] { return overlay->isVisible(); }, 3000));
        CHECK(overlay->text().startsWith(QLatin1String("Tuned SWR:")));
        CHECK(overlay->text().contains(QLatin1String("1.42")));
        // Worded like the tuner's own notice so the severity rule reads it the
        // same way — a result is a result wherever it came from.
        CHECK(overlay->geometry() == applet.rect());

        // And it takes itself down: there is no device clear on this path.
        CHECK(spin([&] { return !overlay->isVisible(); }, 5000));
    }

    if (g_failures == 0) {
        std::printf("tgxl_docked_parity_test: all checks passed\n");
        return 0;
    }
    std::printf("tgxl_docked_parity_test: %d failure(s)\n", g_failures);
    return 1;
}
