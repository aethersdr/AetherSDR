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
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioDelta.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
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
        RadioDelta arrived;
        arrived.region       = QStringLiteral("Japan");
        arrived.radioOptions = QStringLiteral("ATU");
        emit backend->radioChanged(arrived);
        QCoreApplication::processEvents();
        QCOMPARE(options->text(), QStringLiteral("ATU"));   // control
        QCOMPARE(region->text(),  QStringLiteral("Japan")); // the fix

        // And back the other way. RadioModel::disconnectFromRadio clears
        // m_region alongside m_callsign/m_nickname, so a label that only ever
        // moved forwards would go on showing the previous radio's region after
        // a disconnect — the same lie, told about a radio that is no longer
        // there.
        RadioDelta cleared;
        cleared.region       = QString();
        cleared.radioOptions = QString();
        emit backend->radioChanged(cleared);
        QCoreApplication::processEvents();
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
