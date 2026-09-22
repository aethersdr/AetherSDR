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
#include <QLabel>
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
    // the test can satisfy by agreeing with itself — the file still carries the
    // literal #00c8ff in kValueStyle for its other sites, and the control below
    // pins that the two are distinguishable.
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
        // CONTROL: the token does not resolve to the literal this file's
        // kValueStyle still carries elsewhere, so the assertion below can tell a
        // tokenised label from a hardcoded one. If these ever converge this
        // fails loudly rather than passing vacuously.
        QVERIFY(token.name().compare(QStringLiteral("#00c8ff"), Qt::CaseInsensitive) != 0);

        QVERIFY2(region->styleSheet().contains(token.name(), Qt::CaseInsensitive),
                 qPrintable(region->styleSheet()));
        QVERIFY2(hwVersion->styleSheet().contains(token.name(), Qt::CaseInsensitive),
                 qPrintable(hwVersion->styleSheet()));
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
