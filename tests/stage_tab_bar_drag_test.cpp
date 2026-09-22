// AetherRX and AetherTX share one StageTabBar. Both columns accept drops, both
// windows can be open at once, and the drag payload is a bare stage id whose
// meaning depends entirely on which chain it came from:
//
//   RxChainStage{ Eq=1, Gate=2, Comp=3, Tube=4, Pudu=5 }
//   TxChainStage{ Gate=1, Eq=2, DeEss=3, Comp=4, Tube=5 }
//
// Every RX id is also a valid TX id. While the two columns were separate
// widgets they used different MIME types, by accident of having been written
// separately; folding them into one shared widget collapsed that into a single
// type, and a drag out of the AetherRX column then silently reordered the
// transmit chain — drag RX "Eq" (1) onto the TX bar and TX "Gate" (1) moved.
//
// The separation is now deliberate (the type carries the window prefix), which
// means it is the kind of thing a later edit can undo without noticing. This
// test is here so that edit fails.

#include "gui/StageTabBar.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QtTest>

using namespace AetherSDR;

namespace {

// Ids as the two real enums number them, so a cross-window drop lands on a
// genuinely valid id in the receiving chain rather than being rejected as
// out of range for reasons that have nothing to do with the MIME type.
enum RxId { RxEq = 1, RxGate = 2, RxComp = 3 };
enum TxId { TxGate = 1, TxEq = 2, TxDeEss = 3 };

// A bar wired to a host that records any committed reorder.
struct Bar {
    explicit Bar(const QString& prefix, std::initializer_list<int> ids)
        : bar(prefix)
    {
        order = QVector<int>(ids);
        StageTabBar::Host host;
        host.isChainStage     = [](int) { return true; };
        host.stageEnabled     = [](int) { return true; };
        host.setStageEnabled  = [](int, bool) {};
        host.chainOrder       = [this]() { return order; };
        host.commitChainOrder = [this](const QVector<int>& o) {
            order = o;
            ++commits;
        };
        bar.setHost(host);
        int n = 0;
        for (int id : ids)
            bar.addStage(id, QStringLiteral("S%1").arg(++n));
        bar.resize(160, 300);
    }

    StageTabBar bar;
    QVector<int> order;
    int commits{0};
};

// Deliver a drag-enter then a drop carrying `payload` under `mime`, aimed at
// the bottom of the column so any accepted drop would have to move something.
bool deliverDrop(StageTabBar& bar, const QString& mime, int payload)
{
    QMimeData data;
    data.setData(mime, QByteArray::number(payload));

    const QPoint pos(bar.width() / 2, bar.height() - 10);
    QDragEnterEvent enter(pos, Qt::MoveAction, &data,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &enter);

    QDropEvent drop(QPointF(pos), Qt::MoveAction, &data,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &drop);
    return drop.isAccepted();
}

} // namespace

class StageTabBarDragTest : public QObject {
    Q_OBJECT

private slots:
    void aBarAcceptsItsOwnWindowsDrag();
    void receiveColumnCannotReorderTheTransmitChain();
    void transmitColumnCannotReorderTheReceiveChain();
    void theTwoWindowsDoNotShareAMimeType();
};

void StageTabBarDragTest::aBarAcceptsItsOwnWindowsDrag()
{
    Bar tx(QStringLiteral("aetherTx"), { TxGate, TxEq, TxDeEss });
    QVERIFY(deliverDrop(tx.bar, tx.bar.dragMimeType(), TxGate));
    QCOMPARE(tx.commits, 1);
    // Dropped at the foot of the column, so the stage it carried is last.
    QCOMPARE(tx.order.last(), int(TxGate));
}

void StageTabBarDragTest::receiveColumnCannotReorderTheTransmitChain()
{
    // The exact reported gesture: RX "Eq" is id 1, and so is TX "Gate".
    Bar tx(QStringLiteral("aetherTx"), { TxGate, TxEq, TxDeEss });
    const QVector<int> before = tx.order;

    // The foreign type comes from a real RX bar, not from a copy of the
    // formula: if the two windows ever share a type again, this is the same
    // string the TX bar accepts, and the drop lands.
    Bar rx(QStringLiteral("aetherRx"), { RxEq, RxGate, RxComp });
    QVERIFY(!deliverDrop(tx.bar, rx.bar.dragMimeType(), RxEq));
    QCOMPARE(tx.commits, 0);
    QCOMPARE(tx.order, before);
}

void StageTabBarDragTest::transmitColumnCannotReorderTheReceiveChain()
{
    // The reverse direction was already rejected for ids 6 and 7, which is
    // what made the asymmetry easy to miss. Use an id that overlaps.
    Bar rx(QStringLiteral("aetherRx"), { RxEq, RxGate, RxComp });
    const QVector<int> before = rx.order;

    Bar tx(QStringLiteral("aetherTx"), { TxGate, TxEq, TxDeEss });
    QVERIFY(!deliverDrop(rx.bar, tx.bar.dragMimeType(), TxGate));
    QCOMPARE(rx.commits, 0);
    QCOMPARE(rx.order, before);
}

void StageTabBarDragTest::theTwoWindowsDoNotShareAMimeType()
{
    Bar rx(QStringLiteral("aetherRx"), { RxEq });
    Bar tx(QStringLiteral("aetherTx"), { TxGate });
    QVERIFY2(rx.bar.dragMimeType() != tx.bar.dragMimeType(),
             qPrintable(rx.bar.dragMimeType()));
}

QTEST_MAIN(StageTabBarDragTest)
#include "stage_tab_bar_drag_test.moc"
