// The rule behind dragging a stage row up or down the AetherRX tab column.
// It cannot be exercised through the widget: the drop arrives from a nested
// drag loop that the offscreen platform plugin does not run, so this is the
// only place the arithmetic is checked at all.

#include "gui/RxStageReorder.h"

#include <QtTest>

using AetherSDR::RxStageReorder::dropped;

namespace {
// Stand-ins for RxChainStage ids; the rule never looks at what they mean.
enum { Eq = 1, Gate = 2, Comp = 3, Tube = 4, Pudu = 5 };

// Five rows, 40 px apart, first one centred at 20 — the shape the tab column
// actually has.
const QVector<int> kMids{ 20, 60, 100, 140, 180 };
const QVector<int> kOrder{ Eq, Gate, Comp, Tube, Pudu };
} // namespace

class RxStageReorderTest : public QObject {
    Q_OBJECT

private slots:
    void aboveEveryRowGoesToTheFront();
    void belowEveryRowGoesToTheEnd();
    void droppingOnItsOwnRowChangesNothing();
    void movingDownAccountsForTheRemovalShift();
    void movingUpInsertsBeforeTheRowItLandedOn();
    void anUnlistedStageIsLeftAlone();
};

void RxStageReorderTest::aboveEveryRowGoesToTheFront()
{
    QCOMPARE(dropped(kOrder, Pudu, kMids, 0),
             QVector<int>({ Pudu, Eq, Gate, Comp, Tube }));
}

void RxStageReorderTest::belowEveryRowGoesToTheEnd()
{
    QCOMPARE(dropped(kOrder, Eq, kMids, 500),
             QVector<int>({ Gate, Comp, Tube, Pudu, Eq }));
}

void RxStageReorderTest::droppingOnItsOwnRowChangesNothing()
{
    // Comp sits third, middle at 100. Anywhere inside its own row resolves to
    // the place it already holds, and the order comes back untouched.
    QCOMPARE(dropped(kOrder, Comp, kMids, 95), kOrder);
    QCOMPARE(dropped(kOrder, Comp, kMids, 105), kOrder);
}

void RxStageReorderTest::movingDownAccountsForTheRemovalShift()
{
    // Eq (index 0) dropped just past Tube's middle. Without the -1 for its own
    // removal this lands one place too far and Pudu ends up above it.
    QCOMPARE(dropped(kOrder, Eq, kMids, 145),
             QVector<int>({ Gate, Comp, Tube, Eq, Pudu }));
}

void RxStageReorderTest::movingUpInsertsBeforeTheRowItLandedOn()
{
    // Above Gate's middle, below Eq's: Pudu takes Gate's place and everything
    // from Gate down shifts one.
    QCOMPARE(dropped(kOrder, Pudu, kMids, 55),
             QVector<int>({ Eq, Pudu, Gate, Comp, Tube }));
}

void RxStageReorderTest::anUnlistedStageIsLeftAlone()
{
    // AetherNR and Out are not chain stages and never appear in the order; a
    // drop carrying something that is not in it must not rewrite the chain.
    QCOMPARE(dropped(kOrder, 99, kMids, 100), kOrder);
}

QTEST_APPLESS_MAIN(RxStageReorderTest)
#include "rx_stage_reorder_test.moc"
