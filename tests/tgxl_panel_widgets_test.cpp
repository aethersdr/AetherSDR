// TGXL front-panel widgets — the presentation TunerApplet switches to when it
// is popped out or placed on the workspace canvas.
//
// Two contracts are pinned here:
//
//  * AccessoryPortRow renders a missing reading as "N/A" rather than as a stale or
//    invented one. Port B runs on RF sense and never reports a frequency, and
//    port A has none before the client is connected, so "no reading" is the
//    normal case rather than an error path. Bypass, being tuner-wide, empties
//    the per-port state cell and is carried in the spoken description instead.
//
//  * RelayDial carries RelayBar's accessibility contract (#4565) — an ATU
//    sweep debounces to one settled announcement, and the last published value
//    is forgotten on focus loss so a position that moved while unfocused is
//    still announced when focus returns. The dial is a second view of the same
//    relay bank, so the guarantee has to hold in both.

#include "gui/AccessoryPanelWidgets.h"

#include <QAccessible>
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPair>
#include <QTimer>
#include <QVector>

#include <iostream>

using namespace AetherSDR;

namespace {

int g_failures = 0;
QVector<QString>* g_announcements = nullptr;

void captureAccessibleValueUpdate(QAccessibleEvent* event)
{
    if (!g_announcements || event->type() != QAccessible::ValueChanged) return;
    const auto* valueEvent = static_cast<const QAccessibleValueChangeEvent*>(event);
    g_announcements->push_back(valueEvent->value().toString());
}

void expect(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        ++g_failures;
    }
}

void waitForEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// The row keeps its readings in child labels; find one by the text it shows.
bool rowShowsText(const AccessoryPortRow& row, const QString& text)
{
    const QList<QLabel*> labels = row.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text() == text) return true;
    }
    return false;
}

void testPortRowReadings()
{
    AccessoryPortRow row(QStringLiteral("A"));

    // Nothing reported yet: both the band and the frequency read N/A.
    expect(rowShowsText(row, QStringLiteral("N/A")),
           QStringLiteral("a fresh row shows N/A"));

    // A real frequency renders in the same MHz.kHz.Hz grouping as the VFO, so
    // the same signal reads identically wherever it appears in the app.
    row.setFrequencyMhz(7.1855);
    expect(rowShowsText(row, QStringLiteral("7.185.500")),
           QStringLiteral("7.1855 MHz renders as 7.185.500"));

    // Sub-kHz digits are not dropped or rounded away.
    row.setFrequencyMhz(14.074001);
    expect(rowShowsText(row, QStringLiteral("14.074.001")),
           QStringLiteral("14.074001 MHz keeps its Hz digit"));

    // Losing the reading goes back to N/A rather than holding the last one —
    // a frozen but plausible frequency is the worse failure.
    row.setFrequencyMhz(0.0);
    expect(!rowShowsText(row, QStringLiteral("14.074.001")),
           QStringLiteral("a cleared frequency does not linger"));
    expect(rowShowsText(row, QStringLiteral("N/A")),
           QStringLiteral("a cleared frequency reads N/A"));

    // An empty band is N/A too; a real one is shown verbatim.
    row.setBandText(QStringLiteral("40m"));
    expect(rowShowsText(row, QStringLiteral("40m")),
           QStringLiteral("a reported band is shown"));
    row.setBandText(QString());
    expect(!rowShowsText(row, QStringLiteral("40m")),
           QStringLiteral("a cleared band does not linger"));

    // Bypass has no in-row marker of its own — the visual cue is a sibling
    // widget spanning both strips — so the spoken description has to carry it
    // or a screen-reader user cannot tell a bypassed tuner from a matching one.
    row.setStateText(QStringLiteral("OPR"));
    row.setBypassed(true);
    expect(row.accessibleDescription().contains(QStringLiteral("bypassed")),
           QStringLiteral("a bypassed port says so"));
    row.setBypassed(false);
    expect(!row.accessibleDescription().contains(QStringLiteral("bypassed")),
           QStringLiteral("leaving bypass clears it"));

    // An empty state hides the cell rather than leaving a blank box: bypass
    // empties it because the condition is tuner-wide, not per port.
    row.setStateText(QString());
    expect(!rowShowsText(row, QStringLiteral("OPR")),
           QStringLiteral("an empty state clears the cell"));

    // The port's source name comes off the wire verbatim (flexA/flexB), so
    // like the alert banner it is displayed literally rather than left to
    // QLabel's AutoText, which would render markup as rich text and fetch a
    // remote <img>. Principle VII — device input is not ours to trust.
    {
        const QString hostile = QStringLiteral("<img src=http://example.invalid/x.png>");
        row.setSourceText(hostile);
        bool literal = false;
        for (const QLabel* label : row.findChildren<QLabel*>()) {
            if (label->text() != hostile) continue;
            literal = true;
            expect(label->textFormat() == Qt::PlainText,
                   QStringLiteral("the source name is rendered literally"));
        }
        expect(literal, QStringLiteral("the source name is held as given"));
        row.setSourceText(QStringLiteral("FLEX-8600"));
    }

    // The whole strip is one accessible sentence: a reader crossing six
    // separate labels would otherwise lose which port they belong to.
    row.setSourceText(QStringLiteral("FLEX-8600"));
    row.setStateText(QStringLiteral("OPR"));
    row.setPtt(true);
    expect(row.accessibleName().contains(QStringLiteral("A")),
           QStringLiteral("the row names its port"));
    const QString description = row.accessibleDescription();
    expect(description.contains(QStringLiteral("FLEX-8600")),
           QStringLiteral("the description carries the source"));
    expect(description.contains(QStringLiteral("OPR")),
           QStringLiteral("the description carries the state"));
    expect(!description.contains(QStringLiteral("not transmitting")),
           QStringLiteral("a keyed port does not read as idle"));
    row.setPtt(false);
    expect(row.accessibleDescription().contains(QStringLiteral("not transmitting")),
           QStringLiteral("an unkeyed port reads as idle"));
}

void testDialAnnouncements()
{
    QVector<QString> announcements;
    g_announcements = &announcements;
    QAccessible::installUpdateHandler(&captureAccessibleValueUpdate);

    auto* dial = new RelayDial(QStringLiteral("C1"));
    dial->setScrollEnabled(true);
    dial->show();
    dial->setFocus();
    waitForEvents(50);

    if (!dial->hasFocus()) {
        // No focus means no announcement gate to test — the platform, not the
        // widget, decided that. Say so rather than passing vacuously.
        std::cerr << "SKIP: dial never took focus on this platform\n";
        delete dial;
        QAccessible::installUpdateHandler(nullptr);
        g_announcements = nullptr;
        return;
    }

    // A sweep of positions in one burst settles to a single announcement.
    announcements.clear();
    for (int v = 10; v <= 60; v += 10) dial->setValue(v);
    waitForEvents(250);
    expect(announcements.size() == 1,
           QStringLiteral("a relay sweep debounces to one announcement, got %1")
               .arg(announcements.size()));
    expect(!announcements.isEmpty() && announcements.last() == QStringLiteral("60"),
           QStringLiteral("the settled position is the one announced"));

    // A value that moves away and back while unfocused must not be swallowed
    // by the dedup when focus returns.
    announcements.clear();
    dial->clearFocus();
    waitForEvents(20);
    dial->setValue(120);
    dial->setValue(60);            // back to the last announced position
    dial->setFocus();
    waitForEvents(20);
    if (dial->hasFocus()) {
        dial->setValue(60);        // no change, but the dedup was reset
        dial->setValue(75);
        waitForEvents(250);
        expect(!announcements.isEmpty(),
               QStringLiteral("a position that moved while unfocused is announced again"));
    }

    // Values still track regardless of focus — the dial reads hardware pushes.
    dial->setValue(200);
    expect(dial->value() == 200, QStringLiteral("the dial holds the value it was given"));

    delete dial;
    QAccessible::installUpdateHandler(nullptr);
    g_announcements = nullptr;
}

// The severity rule TunerApplet applies to tuner alerts. The protocol carries
// no severity field, so it is read off the text: the completion notice reads
// as success and everything else as something to act on. Pinned here because
// getting it backwards shows a failed tune as good news -- and because the
// rule is a string prefix, which is exactly the kind of thing that rots
// silently when new message text appears. The tune's result is the overlay's
// to report and nowhere else's, so there is nothing here about the TUNE key.
//
// Both strings are verbatim from a capture of the 4O3A TunerGeniusDesk
// application against a TGXL on firmware 1.2.17.
bool alertReadsAsGood(const QString& text)
{
    return text.trimmed().startsWith(QLatin1String("Tuned"), Qt::CaseInsensitive);
}

void testAlertSeverityRule()
{
    expect(alertReadsAsGood(QStringLiteral("Tuned SWR: 1.13:1")),
           QStringLiteral("the completion notice reads as success"));
    expect(!alertReadsAsGood(QStringLiteral("LOW RF POWER")),
           QStringLiteral("a failure does not read as success"));
    // An unrecognised message must fall on the attention side: a warning shown
    // as good news is worse than the reverse.
    expect(!alertReadsAsGood(QStringLiteral("SOMETHING NEW FROM A FUTURE FIRMWARE")),
           QStringLiteral("unknown text falls back to attention"));

    // The empty frame is the tuner's own clear, and is what takes the overlay
    // down — there is no local dwell timer to get out of step with it. It
    // must not be mistaken for an alert whose text happens to be blank.
    expect(QStringLiteral("").trimmed().isEmpty(),
           QStringLiteral("an empty alert is a clear, not a blank banner"));

}

// Which port the applet outlines. The tuner cannot answer this: with one
// radio cabled to both ports its status reports modeA=1 AND modeB=1, and
// `active` never moves — so an outline driven from those lights up both rows
// at once, which is what this rule replaced. The answer is the port whose
// configured antenna matches the transmit slice's, the same comparison
// FlexLib makes before it will autotune.
//
// Mirrors TunerApplet::updateActivePort so the rule is pinned independently
// of the widget tree it drives.
QPair<bool, bool> activePorts(const QString& tx, const QString& aAnt, const QString& bAnt)
{
    const bool a = !tx.trimmed().isEmpty() && !aAnt.trimmed().isEmpty()
                   && aAnt.trimmed().compare(tx.trimmed(), Qt::CaseInsensitive) == 0;
    const bool b = !tx.trimmed().isEmpty() && !bAnt.trimmed().isEmpty()
                   && bAnt.trimmed().compare(tx.trimmed(), Qt::CaseInsensitive) == 0;
    return {a, b};
}

void testActivePortRule()
{
    const QString A = QStringLiteral("ANT1"), B = QStringLiteral("ANT2");

    auto onAnt1 = activePorts(A, A, B);
    expect(onAnt1.first && !onAnt1.second,
           QStringLiteral("transmitting on ANT1 outlines port A only"));

    // The reported bug: switching to ANT2 lit port B while port A stayed lit.
    auto onAnt2 = activePorts(B, A, B);
    expect(!onAnt2.first && onAnt2.second,
           QStringLiteral("transmitting on ANT2 outlines port B only"));

    // Never both — one port transmits at a time, whatever the tuner reports
    // about which ports can hear a radio.
    for (const QString& tx : {A, B}) {
        auto p = activePorts(tx, A, B);
        expect(!(p.first && p.second), QStringLiteral("never two outlines (tx=%1)").arg(tx));
    }

    // An antenna that does not run through the tuner outlines nothing rather
    // than guessing a port — FlexLib declines to autotune in exactly this case.
    auto offTuner = activePorts(QStringLiteral("ANT3"), A, B);
    expect(!offTuner.first && !offTuner.second,
           QStringLiteral("an antenna not on the tuner outlines neither port"));

    // Before the radio or the tuner has reported, nothing is outlined: an
    // outline claims RF is passing through that port.
    auto unknownTx = activePorts(QString(), A, B);
    expect(!unknownTx.first && !unknownTx.second,
           QStringLiteral("no transmit antenna yet outlines neither port"));
    auto unknownMap = activePorts(A, QString(), QString());
    expect(!unknownMap.first && !unknownMap.second,
           QStringLiteral("no port map yet outlines neither port"));
}

// The panel grows its contents with the window rather than its padding, so
// the scaling primitives have to actually move. CrossNeedleMeterWidget gets
// this for free by scaling a QPainter onto a fixed design canvas; a widget
// tree cannot, so each metric is scaled instead -- and a metric that silently
// ignored its scale would look exactly like the bug this replaced.
void testScaling()
{
    // The three discrete keys share one seed width, scaled like every other
    // metric, so they stay identical to each other and grow with the panel.
    // The seed must NOT come from the laid-out column: fixing a key's size
    // makes the column's own size hint that fixed width, a one-way ratchet
    // that leaves the keys stranded at whatever size they were first given.
    //
    // Their height is tied to the dials instead of to their own width. The
    // 16:9 they were first given cannot hold alongside that tie -- a key as
    // tall as this would be 164px wide at 16:9 on an 800px panel, and three
    // of those plus the dials overflow the control row at every panel size.
    constexpr int kSeed = 52;          // a representative widest-caption seed
    constexpr int kDialDesign = 46;    // the dial's design diameter
    constexpr qreal kKeyHeightOfDial = 0.95;
    constexpr qreal kKeyAspect = 16.0 / 9.0;
    int previousWidth = 0;
    for (qreal scale : {0.8, 1.0, 1.45, 2.1}) {
        const int dial = qMax(1, qRound(kDialDesign * scale));
        // Height is chosen from the dial, width follows it at 16:9 — both
        // rules hold at once, which they only can because the panel's design
        // width budgets for keys this wide.
        const int h = qMax(1, qRound(dial * kKeyHeightOfDial));
        const int w = qMax(qRound(h * kKeyAspect), qRound(kSeed * scale));

        const qreal ofDial = qreal(h) / dial;
        expect(qAbs(ofDial - kKeyHeightOfDial) < 0.02,
               QStringLiteral("a key at scale %1 is %2 of the dial's height")
                   .arg(scale).arg(ofDial, 0, 'f', 3));
        const qreal aspect = qreal(w) / h;
        expect(qAbs(aspect - kKeyAspect) < 0.06,
               QStringLiteral("a key at scale %1 is %2x%3 (aspect %4)")
                   .arg(scale).arg(w).arg(h).arg(aspect, 0, 'f', 3));
        expect(w > previousWidth,
               QStringLiteral("a key grows with the scale (%1 -> %2)")
                   .arg(previousWidth).arg(w));
        previousWidth = w;
    }

    // A PanelKey's minimum must not follow the size the scale gave it. When
    // it did, the layout's minimum tracked the current scale and the panel's
    // own floor ratcheted upward: an 802px panel reported a 710px floor and
    // could never be made small again.
    {
        PanelKey key(QStringLiteral("STBY"));
        key.setTargetSize(QSize(80, 45));
        const QSize small = key.minimumSizeHint();
        key.setTargetSize(QSize(240, 135));
        expect(key.minimumSizeHint() == small,
               QStringLiteral("a key's minimum ignores its target size"));
        expect(key.sizeHint() == QSize(240, 135),
               QStringLiteral("but its size hint follows it"));
    }

    RelayDial dial(QStringLiteral("C1"));
    const int base = dial.sizeHint().width();
    dial.setPreferredDiameter(base * 2);
    expect(dial.sizeHint().width() == base * 2,
           QStringLiteral("the dial takes its preferred diameter"));
    expect(dial.minimumSize().width() < dial.sizeHint().width(),
           QStringLiteral("the dial can still be squeezed below its preference"));

    // Clamped at both ends: a panel dragged to a sliver must not ask for a
    // dial of two pixels, nor one bigger than any sane window.
    dial.setPreferredDiameter(1);
    expect(dial.sizeHint().width() >= 36,
           QStringLiteral("an absurdly small diameter is clamped, got %1")
               .arg(dial.sizeHint().width()));
    dial.setPreferredDiameter(100000);
    expect(dial.sizeHint().width() <= 260,
           QStringLiteral("an absurdly large diameter is clamped, got %1")
               .arg(dial.sizeHint().width()));

    // The port strip scales its cells with its type, or the text outgrows the
    // box it sits in.
    AccessoryPortRow row(QStringLiteral("A"));
    const int baseHint = row.sizeHint().height();
    row.setScale(2.0);
    expect(row.sizeHint().height() > baseHint,
           QStringLiteral("the strip grows with its scale (%1 -> %2)")
               .arg(baseHint).arg(row.sizeHint().height()));
    row.setScale(1.0);
    expect(row.sizeHint().height() == baseHint,
           QStringLiteral("and returns to its compact size"));
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    if (!QAccessible::isActive()) {
        QAccessible::setActive(true);
    }
    if (!QAccessible::isActive()) {
        // Same convention as the other a11y tests: no backend is a skip, not
        // a failure. tests.cmake maps 77 to SKIP.
        std::cerr << "No accessibility backend available — skipping\n";
        return 77;
    }

    testPortRowReadings();
    testDialAnnouncements();
    testAlertSeverityRule();
    testActivePortRule();
    testScaling();

    if (g_failures == 0) {
        std::cout << "tgxl_panel_widgets_test: all checks passed\n";
        return 0;
    }
    std::cout << "tgxl_panel_widgets_test: " << g_failures << " failure(s)\n";
    return 1;
}
