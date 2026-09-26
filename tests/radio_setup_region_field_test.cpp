// #5507 — Radio Setup's `Region:` field.
//
// The field used to be built as
//     new QLabel(m_model->region().isEmpty() ? "USA" : m_model->region())
// and RadioModel::m_region is written only from Flex sources (the `info` reply
// parser and FlexBackend's RadioDelta). On a Hermes-Lite 2 nothing ever writes
// it, so the ternary's "empty" arm was the ONLY arm that ever ran and the
// dialog reported a region the radio had never claimed — while
// SliceTroubleshootingDialog, rendering the same m_region out of
// troubleshootingSnapshot, said `n/a` for it in the support bundle.
//
// Three things are pinned here, one per item of the issue, and each one carries
// a positive control, because every assertion below would ALSO pass against a
// label that had been made permanently blank or permanently identical to its
// neighbour. The controls are what tell "the field reads the model" apart from
// "the field prints one string forever" — which is the exact class of defect
// the issue is about, and a test that could not see the difference would be a
// second instance of it.
//
// #5857 extends the last slot to the REST of this dialog's value labels. The
// shared value style used to be a colour literal applied with a plain
// setStyleSheet, which ThemeManager does not register for re-resolution, so
// those labels kept their dark-theme colour after View > Theme. Two of them
// went through ThemeManager::applyStyleSheet and were therefore registered —
// and still never moved, because the template they registered carried no
// {{token}} to re-resolve. That half passes a construction-time check and
// fails a post-switch one, so the slot below switches the theme and reads the
// colour back rather than only inspecting the freshly built dialog.
//
// No hardware and no transport: the model is constructed bare and never
// connected, and the region arrives the way a Flex's does — as a RadioDelta over
// IRadioBackend::radioChanged, the seam RadioModel::setupBackend wires to
// applyRadioChanges. (setBackendForTest is deliberately NOT used: it wires only
// the PCM/audio/receiver-state connections, not the status deltas, so an
// injected backend cannot deliver one.)

#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioDelta.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStringList>
#include <QToolButton>
#include <QtTest>

namespace AetherSDR {
// RadioSetupDialog declares this a friend; it is the only way to the labels
// without adding a test-only accessor to production code.
class RadioSetupDialogTestAccess {
public:
    static QLabel* regionLabel(RadioSetupDialog& d)    { return d.m_regionLabel; }
    static QLabel* hwVersionLabel(RadioSetupDialog& d) { return d.m_hwVersionLabel; }
    // The peer used as the house placeholder oracle. `Options:` is in the same
    // group, is sourced from an equally empty model field, and already answered
    // an empty value with the em-dash before this change — so comparing against
    // its text asserts "Region: answers the way this dialog answers" without
    // retyping the placeholder character into the test, where it would then
    // agree with itself no matter what the dialog did.
    static QLabel* optionsLabel(RadioSetupDialog& d)   { return d.m_optionsLabel; }
};
}
using namespace AetherSDR;

namespace {

// A theme file, read at run time from the resource the application itself
// ships and loads. Nothing here goes through ThemeManager: the point is to
// have an oracle that is not the code under test, so a resolver that quietly
// stopped re-resolving would not be able to agree with the assertion. Reading
// the file is also what keeps #00c8f0 and #0098c0 out of this source — a test
// that retypes the constant it guards passes while the code it guards is
// wrong.
struct ThemeProbe {
    QString name;
    QColor  accentBright;
};

ThemeProbe readThemeFile(const QString& path)
{
    ThemeProbe probe;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return probe;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    probe.name = root.value(QStringLiteral("name")).toString();

    // scopes.root.tokens.color["accent.bright"], then one {primitive} hop.
    QString value = root.value(QStringLiteral("scopes")).toObject()
                        .value(QStringLiteral("root")).toObject()
                        .value(QStringLiteral("tokens")).toObject()
                        .value(QStringLiteral("color")).toObject()
                        .value(QStringLiteral("accent.bright")).toString();
    if (value.startsWith(QLatin1Char('{')) && value.endsWith(QLatin1Char('}'))) {
        value = root.value(QStringLiteral("primitives")).toObject()
                    .value(value.mid(1, value.size() - 2)).toString();
    }
    probe.accentBright = QColor(value);
    return probe;
}

// The value style's SHAPE, with the colour left open. Matching on shape rather
// than on a colour is what lets the set be collected before anything is
// asserted about it: a label that kept a literal is still in the set, and is
// then caught by the colour assertion instead of quietly dropping out of it.
const QRegularExpression& valueSheetPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^QLabel \\{ color: ([^;]+); font-size: 12px; font-weight: bold; \\}$"));
    return re;
}

QColor valueSheetColour(const QLabel* label)
{
    const auto m = valueSheetPattern().match(label->styleSheet());
    return m.hasMatch() ? QColor(m.captured(1).trimmed()) : QColor();
}

QList<QLabel*> collectValueLabels(QWidget& dialog)
{
    QList<QLabel*> out;
    for (QLabel* l : dialog.findChildren<QLabel*>()) {
        if (valueSheetPattern().match(l->styleSheet()).hasMatch()) out.append(l);
    }
    return out;
}

// Names a label in a failure message. The value labels are local variables in
// buildXxxTab with no object names of their own, so the identifier that
// actually reaches a reader is the field name their CopyValueButton sibling
// carries in its accessible name ("Copy Default Gateway").
QString describe(const QLabel* label)
{
    QStringList parts;
    if (!label->objectName().isEmpty()) parts << label->objectName();
    if (QWidget* field = label->parentWidget()) {
        for (const QToolButton* b : field->findChildren<QToolButton*>()) {
            if (!b->accessibleName().isEmpty()) parts << b->accessibleName();
        }
    }
    parts << QStringLiteral("text=\"%1\"").arg(label->text());
    parts << QStringLiteral("qss=%1").arg(label->styleSheet());
    return parts.join(QStringLiteral(" | "));
}

bool anyDescribes(const QList<QLabel*>& labels, const QString& needle)
{
    for (const QLabel* l : labels) {
        if (describe(l).contains(needle)) return true;
    }
    return false;
}

} // namespace

// The value labels this dialog names well enough to be asserted one by one.
// CopyValueButton's accessible name carries the field name ("Copy Default
// Gateway"); qrzCacheCount is the one that names itself. Sixteen of the
// twenty-eight labels a bare RadioModel produces are identifiable this way,
// and they include both of the sites that were tracked and still never moved.
// The remaining twelve -- buildGpsTab's ten addField values and the two audio
// gain readouts -- are local QLabels with no name of any kind, and are covered
// by the whole-set loop rather than by name.
static const QStringList kNamedValueFields = {
    // The four #5848 routed through makeValueLabel.
    QStringLiteral("Copy Radio Serial Number"),
    QStringLiteral("Copy Region"),
    QStringLiteral("Copy HW Version"),
    QStringLiteral("Copy Options"),
    // #5857's direct setStyleSheet sites.
    QStringLiteral("Copy Model"),
    QStringLiteral("Copy Subscription"),
    QStringLiteral("Copy Expiration"),
    QStringLiteral("Copy Radio ID"),
    QStringLiteral("Copy Licensed version"),
    QStringLiteral("Copy FW Version"),
    QStringLiteral("Copy IP Address"),
    QStringLiteral("Copy Subnet Mask"),
    QStringLiteral("Copy MAC Address"),
    QStringLiteral("qrzCacheCount"),
    // #5857's two applyStyleSheet sites: registered, re-visited on every
    // themeChanged, and still wrong. These are the ones the switch is for.
    QStringLiteral("Copy Default Gateway"),
    QStringLiteral("Copy Network Name"),
};

// A floor, not the count. This dialog builds twenty-eight value labels for a
// bare RadioModel here; the floor is the sixteen nameable ones, so the loops
// cannot pass vacuously and a platform where one deferred page declines to
// build does not turn a cosmetic fix red for an unrelated reason.
static constexpr int kMinValueLabels = 16;

class RadioSetupRegionFieldTest : public QObject {
    Q_OBJECT
private slots:

    // Item 1 — the value. A radio that never reports a region must not have one
    // made up for it.
    void emptyRegionReadsAsUnknownNotAsUsa()
    {
        RadioModel model;
        // The premise, asserted rather than assumed: if something else started
        // populating region() by itself, the rest of this test would be
        // measuring nothing and should say so here first.
        QVERIFY(model.region().isEmpty());
        QVERIFY(model.radioOptions().isEmpty());

        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region  = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* options = RadioSetupDialogTestAccess::optionsLabel(dialog);
        QVERIFY(region);
        QVERIFY(options);

        // Both fields are sourced from an empty model string, so both must
        // answer with whatever this dialog's placeholder is. Before the fix
        // Region: answered "USA" and this compared "USA" against the em-dash.
        QCOMPARE(region->text(), options->text());
    }

    // POSITIVE CONTROL for the assertion above, through the constructor rather
    // than the refresh hook so that item 1 is pinned independently of item 3.
    //
    // Without this, `emptyRegionReadsAsUnknownNotAsUsa` would be satisfied by a
    // label hardcoded to the placeholder — which reports just as confidently and
    // just as wrongly as "USA" did, only in the other direction. A real region
    // must reach the label, and must be distinguishable from the placeholder.
    void aReportedRegionIsShownAndIsNotThePlaceholder()
    {
        RadioModel model;
        IRadioBackend* backend = model.backend();
        QVERIFY(backend);

        RadioDelta delta;
        delta.region = QStringLiteral("Japan");
        emit backend->radioChanged(delta);
        QCOMPARE(model.region(), QStringLiteral("Japan"));

        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region  = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* options = RadioSetupDialogTestAccess::optionsLabel(dialog);
        QVERIFY(region);
        QVERIFY(options);

        QCOMPARE(region->text(), QStringLiteral("Japan"));
        // …and the placeholder oracle is still answering the other way, so the
        // comparison in the previous test was a real comparison between two
        // reachable states and not an identity that holds unconditionally.
        QVERIFY(region->text() != options->text());
    }

    // Item 3 — the refresh. m_regionLabel had no setText anywhere in the file,
    // so it froze at construction while its three neighbours in the same
    // infoChanged lambda updated.
    void regionFollowsTheModelAfterConstruction()
    {
        RadioModel model;
        IRadioBackend* backend = model.backend();
        QVERIFY(backend);
        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region  = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* options = RadioSetupDialogTestAccess::optionsLabel(dialog);
        QVERIFY(region);
        QVERIFY(options);
        const QString placeholder = options->text();

        // One delta carrying BOTH fields. Options: was already wired into the
        // infoChanged lambda before this change, so it is the control: if the
        // delta never arrived, or infoChanged never fired, or the dialog was
        // never connected, the Options: assertion fails too and the failure is
        // about the harness rather than about Region:. Region: failing ALONE is
        // the defect.
        //
        // No processEvents() anywhere below, deliberately: every hop on this
        // path is a direct same-thread call — radioChanged is emitted from this
        // thread to a RadioModel living on it, applyRadioChanges emits
        // infoChanged inline, and the dialog's lambda runs inline off that. A
        // processEvents() here would pass while implying a queued delivery this
        // path does not have, and would keep passing if the delivery ever
        // became queued and broken.
        RadioDelta arrived;
        arrived.region       = QStringLiteral("Japan");
        arrived.radioOptions = QStringLiteral("ATU");
        emit backend->radioChanged(arrived);
        QCOMPARE(options->text(), QStringLiteral("ATU"));   // control
        QCOMPARE(region->text(),  QStringLiteral("Japan")); // the fix

        // Region: ALONE. RadioDelta is present-only (std::optional per field),
        // so leaving radioOptions unset is a delta that names region and
        // nothing else — which is the shape a real region change arrives in.
        // Without this step the Region: label is only ever observed moving in
        // company, and a lambda that refreshed Region: by reading the Options:
        // string, or one that only ran when radioOptions was present, would
        // pass. Here Options: is a NEGATIVE control: it must hold still.
        RadioDelta regionOnly;
        regionOnly.region = QStringLiteral("Europe");
        emit backend->radioChanged(regionOnly);
        QCOMPARE(region->text(),  QStringLiteral("Europe")); // the fix, alone
        QCOMPARE(options->text(), QStringLiteral("ATU"));    // negative control

        // And back the other way. RadioModel::disconnectFromRadio clears
        // m_region alongside m_callsign/m_nickname, so a label that only ever
        // moved forwards would go on showing the previous radio's region after
        // a disconnect — the same lie, told about a radio that is no longer
        // there.
        RadioDelta cleared;
        cleared.region       = QString();
        cleared.radioOptions = QString();
        emit backend->radioChanged(cleared);
        QCOMPARE(options->text(), placeholder);   // control
        QCOMPARE(region->text(),  placeholder);   // the fix
    }

    // Item 2 — the styling. A centre-aligned bordered accent box with
    // kToggleStyle's metrics, sitting in the column that makeToggle builds
    // Remote On: and multiFLEX: in, reads as pressable. The operator who filed
    // this clicked it. It is a QLabel with no event handling at all.
    //
    // Pinned as "matches HW Version:" rather than by quoting a stylesheet: the
    // file's own FlexControl: comment names "Region:/HW Version: above" as its
    // model of a status label, so that is the invariant, and quoting the
    // stylesheet text here would only make the test agree with itself.
    void regionIsStyledAsAStatusLabelLikeItsNeighbour()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region    = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* hwVersion = RadioSetupDialogTestAccess::hwVersionLabel(dialog);
        QVERIFY(region);
        QVERIFY(hwVersion);

        // CONTROL: the peer carries a real stylesheet, so the comparison below
        // is between two set stylesheets and not two empty strings. (The group
        // sweeps kLabelStyle onto any label left with an empty stylesheet, so
        // "empty" would not even survive construction — but an assertion that
        // depends on that sweep to be non-vacuous is not one worth having.)
        QVERIFY(!hwVersion->styleSheet().isEmpty());

        QCOMPARE(region->styleSheet(), hwVersion->styleSheet());
        // Centre alignment was the other half of the control look.
        QCOMPARE(region->alignment(), hwVersion->alignment());
    }

    // Item 2, the interaction half. Styling made Region: look like HW Version:;
    // this pins that it BEHAVES like it too. Region: was the only value in the
    // Radio Information group built with makeInfoField rather than
    // makeCopyableInfoField, so it was the one field of the four an operator
    // assembling a bug report could not select or copy.
    //
    // Asserted against the neighbour for the same reason as the stylesheet
    // above: retyping Qt::TextSelectableByMouse or the literal 1 here would let
    // the test agree with itself if makeCopyableInfoField ever changed what it
    // grants. The controls make sure neither comparison is between two nothings.
    void regionIsSelectableAndCopyableLikeItsNeighbour()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region    = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* hwVersion = RadioSetupDialogTestAccess::hwVersionLabel(dialog);
        QVERIFY(region);
        QVERIFY(hwVersion);

        // CONTROL: the neighbour really is selectable, so the compare below is
        // not NoTextInteraction against NoTextInteraction.
        QVERIFY(hwVersion->textInteractionFlags() & Qt::TextSelectableByMouse);
        QCOMPARE(region->textInteractionFlags(), hwVersion->textInteractionFlags());

        // The copy affordance. Each value label sits in the field wrapper its
        // make*InfoField built, and CopyValueButton is the only QToolButton in
        // either wrapper.
        auto copyButtons = [](QLabel* value) {
            QWidget* field = value->parentWidget();
            return field ? int(field->findChildren<QToolButton*>().size()) : -1;
        };
        // CONTROL: the neighbour has exactly one, so the compare is not 0 == 0.
        QCOMPARE(copyButtons(hwVersion), 1);
        QCOMPARE(copyButtons(region), copyButtons(hwVersion));
    }

    // The colour of these four values resolves through a ThemeManager token
    // rather than being pasted in as a literal. This is the one property the
    // old Region: stylesheet had that the rest of the group did not, and losing
    // it while fixing the box metrics would have been a silent trade: only
    // ThemeManager::applyStyleSheet registers a widget for re-resolution on
    // themeChanged, so a literal survives a switch to Default Light unchanged.
    //
    // Asserted against the token's own resolved value, which is not something
    // the test can satisfy by agreeing with itself — #00c8ff is the literal
    // this file used to carry, and the control below pins that the token and
    // that literal are distinguishable.
    void valueLabelsResolveTheirColourThroughTheThemeToken()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QLabel* region    = RadioSetupDialogTestAccess::regionLabel(dialog);
        QLabel* hwVersion = RadioSetupDialogTestAccess::hwVersionLabel(dialog);
        QVERIFY(region);
        QVERIFY(hwVersion);

        const QColor token =
            ThemeManager::instance().color(region, QStringLiteral("color.accent.bright"));
        QVERIFY(token.isValid());
        // CONTROL: the token does not resolve to the retired literal, so the
        // assertion below can tell a tokenised label from a hardcoded one. If
        // these ever converge this fails loudly rather than passing vacuously.
        QVERIFY(token.name().compare(QStringLiteral("#00c8ff"), Qt::CaseInsensitive) != 0);

        QVERIFY2(region->styleSheet().contains(token.name(), Qt::CaseInsensitive),
                 qPrintable(region->styleSheet()));
        QVERIFY2(hwVersion->styleSheet().contains(token.name(), Qt::CaseInsensitive),
                 qPrintable(hwVersion->styleSheet()));
    }

    // #5857 — the rest of this dialog's value labels, and the only assertion
    // that can see the half of the defect that looks fixed.
    //
    // The slot above is a CONSTRUCTION-TIME check on the four fields #5848
    // routed through makeValueLabel. It cannot distinguish "tracked and
    // re-resolvable" from "tracked and permanently wrong": gatewayLbl and
    // networkNameLbl were registered with ThemeManager::applyStyleSheet and
    // were therefore in m_trackedWidgets and were visited on every
    // themeChanged — and the template recorded for them was a colour literal
    // with no {{token}} in it, so resolveFor returned it verbatim and the
    // re-application re-set the same wrong colour. The audit passed, the widget
    // was tracked, the colour was still wrong. Only switching the theme and
    // reading the colour back can tell the two apart, so that is what this
    // does.
    //
    // The set is collected by the value style's SHAPE, so it does not need a
    // list of label names to stay in step with the file, and a label that kept
    // a literal is still collected and then fails the colour assertion rather
    // than silently leaving the set.
    void everyValueLabelColourFollowsAThemeSwitch()
    {
        const ThemeProbe dark  = readThemeFile(QStringLiteral(":/themes/default-dark.json"));
        const ThemeProbe light = readThemeFile(QStringLiteral(":/themes/default-light.json"));
        QVERIFY2(dark.accentBright.isValid(),  "default-dark.json: color.accent.bright unreadable");
        QVERIFY2(light.accentBright.isValid(), "default-light.json: color.accent.bright unreadable");
        QVERIFY(!dark.name.isEmpty());
        QVERIFY(!light.name.isEmpty());

        // CONTROLS on the oracle itself. If the two themes ever agreed about
        // this token, "the colour moved" would be unobservable and every
        // assertion below would pass on a label that never re-resolved; and if
        // the dark value were the old literal, a label that had not been
        // converted at all would pass the first pass.
        QVERIFY2(dark.accentBright != light.accentBright,
                 qPrintable(QStringLiteral("themes agree on color.accent.bright: %1")
                                .arg(dark.accentBright.name())));
        QVERIFY(dark.accentBright.name().compare(QStringLiteral("#00c8ff"),
                                                 Qt::CaseInsensitive) != 0);

        ThemeManager& tm = ThemeManager::instance();
        const QString restore = tm.activeTheme();
        // Restore on EVERY exit, not just the happy one: every QVERIFY2 below
        // returns from the slot, so a red assertion taken under Default Light
        // would otherwise leave the profile there for whatever runs next.
        // Harmless while this is the last slot and TestSettingsProfile isolates
        // the store -- the guard is what keeps it harmless if a slot is
        // appended.
        const auto restoreTheme = qScopeGuard([&tm, &restore] {
            if (!restore.isEmpty() && tm.activeTheme() != restore)
                tm.setActiveTheme(restore);
        });
        QVERIFY2(tm.setActiveTheme(dark.name), qPrintable(dark.name));

        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();

        // Only the Radio page is built eagerly (#1776 — deferring the rest
        // keeps QSerialPortInfo/QMediaDevices probes out of the constructor),
        // so the pages carrying the other value labels have to be opened the
        // way an operator opens them. selectTab is the dialog's own public
        // entry point and runs the deferred builder synchronously; a page a
        // bare model has no capability for simply stays unbuilt and
        // contributes nothing, which is why the coverage assertions below name
        // the pages that matter rather than a total.
        //
        // "Audio" is DELIBERATELY not in this list. buildAudioTab() is the one
        // deferred page that performs the probe #1776 moved out of the
        // constructor -- QMediaDevices::audioInputs()/audioOutputs() plus a
        // live QMediaDevices monitor parented to the group box -- and the
        // comment there names that probe as crashing on some Wayland/Qt 6.11
        // configurations. No test in this tree opens that page offscreen, and
        // this slot is not the one to start: every field in kNamedValueFields
        // lives on Radio, Network or QRZ, and kMinValueLabels is reached
        // without it. The two Audio readouts (lineoutValue, hpValue) are
        // unnamed either way, so opening the page would buy no assertion that
        // could fail for a reason worth knowing about -- only a
        // platform-conditional crash on the full-suite runners.
        for (const char* page : {"Radio", "Network", "GPS",
                                 "Antennas", "QRZ & Callsigns"}) {
            dialog.selectTab(QString::fromLatin1(page));
        }

        const QList<QLabel*> values = collectValueLabels(dialog);
        // The COUNT, not the labels. describe() is already interpolated into
        // every failure message below, so dumping all of them on a PASSING run
        // buys nothing and costs ~28 lines of stylesheet in the log. The count
        // is the one thing a green run cannot otherwise tell you, and it is
        // what says how much headroom there is above kMinValueLabels.
        qInfo() << "value labels collected:" << values.size();

        // Vacuity guard: a loop over an empty list passes everything. The
        // number is the count this dialog actually builds for a bare
        // RadioModel, not the count of call sites — addField and the antenna
        // alias row are loops, and a capability-gated group contributes none.
        QVERIFY2(values.size() >= kMinValueLabels,
                 qPrintable(QStringLiteral("only %1 value labels found").arg(values.size())));

        // COVERAGE, named field by field. Without this the slot could go on
        // passing on whichever labels happened to be reachable while a site it
        // was meant to cover quietly left the set -- and the two it would lose
        // first are the applyStyleSheet pair, since those are the ones that
        // look correct at every other level.
        for (const QString& field : kNamedValueFields) {
            QVERIFY2(anyDescribes(values, field),
                     qPrintable(QStringLiteral("no value label matching %1").arg(field)));
        }

        // Under Default Dark every one of them carries the token's value — not
        // the literal, which is one step off it in the blue channel — and is
        // registered against the token, which is what makes the switch below
        // capable of moving it.
        for (QLabel* l : values) {
            QVERIFY2(valueSheetColour(l) == dark.accentBright,
                     qPrintable(QStringLiteral("expected %1 under %2: %3")
                                    .arg(dark.accentBright.name(), dark.name, describe(l))));
            QVERIFY2(tm.tokensForWidget(l).contains(QStringLiteral("color.accent.bright")),
                     qPrintable(QStringLiteral("not registered against the token: %1")
                                    .arg(describe(l))));
        }

        // THE SWITCH. This is the assertion the construction-time one cannot
        // make: a widget that is tracked but carries no token survives it
        // unchanged.
        QVERIFY2(tm.setActiveTheme(light.name), qPrintable(light.name));
        for (QLabel* l : values) {
            QVERIFY2(valueSheetColour(l) == light.accentBright,
                     qPrintable(QStringLiteral("expected %1 under %2: %3")
                                    .arg(light.accentBright.name(), light.name, describe(l))));
        }

        // And back, so the slot pins a colour that FOLLOWS the theme rather
        // than one that changed once in one direction.
        QVERIFY2(tm.setActiveTheme(dark.name), qPrintable(dark.name));
        for (QLabel* l : values) {
            QVERIFY2(valueSheetColour(l) == dark.accentBright,
                     qPrintable(QStringLiteral("expected %1 back under %2: %3")
                                    .arg(dark.accentBright.name(), dark.name, describe(l))));
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-region-field"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupRegionFieldTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_region_field_test.moc"
