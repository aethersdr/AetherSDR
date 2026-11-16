// TxFilterEdges::snap: a stock preset's TX filter fitted to what a radio can
// set. Continuous radios get it as asked; a radio with fixed edges (the Icom
// lists in RadioCapabilities) gets the nearest edge on each side, and never a
// filter narrower than the minimum width when a wider edge exists.

#include "core/TxFilterEdges.h"

#include <QtTest>

using namespace AetherSDR;

class TxFilterEdgesTest : public QObject {
    Q_OBJECT

private slots:
    void continuousRadiosGetItAsAsked()
    {
        QCOMPARE(TxFilterEdges::snap(100, 3100, {}, {}, 50), std::make_pair(100, 3100));
        QCOMPARE(TxFilterEdges::snap(50, 6050, {}, {}, 50), std::make_pair(50, 6050));
    }

    void fixedEdgesSnapToTheNearest()
    {
        const QList<int> lows{100, 200, 300, 400, 500, 700};
        const QList<int> highs{2500, 2700, 2900, 3000};
        QCOMPARE(TxFilterEdges::snap(100, 3100, lows, highs, 50), std::make_pair(100, 3000));
        QCOMPARE(TxFilterEdges::snap(200, 2900, lows, highs, 50), std::make_pair(200, 2900));
        QCOMPARE(TxFilterEdges::snap(50, 6050, lows, highs, 50), std::make_pair(100, 3000));
        QCOMPARE(TxFilterEdges::snap(260, 2790, lows, highs, 50), std::make_pair(300, 2700));
    }

    void oneSideFixed()
    {
        QCOMPARE(TxFilterEdges::snap(150, 3100, {100, 200}, {}, 50), std::make_pair(100, 3100));
        QCOMPARE(TxFilterEdges::snap(150, 3100, {}, {2800, 3000}, 50), std::make_pair(150, 3000));
    }

    void keepsTheMinimumWidth()
    {
        // The nearest high edge would leave 20 Hz: take the first wide enough.
        QCOMPARE(TxFilterEdges::snap(700, 700, {700}, {720, 760, 900}, 50), std::make_pair(700, 760));
        // Continuous high side: widened to exactly the minimum.
        QCOMPARE(TxFilterEdges::snap(700, 710, {700}, {}, 50), std::make_pair(700, 750));
        // No edge is wide enough: the nearest stands rather than inventing one.
        QCOMPARE(TxFilterEdges::snap(700, 700, {700}, {720}, 50), std::make_pair(700, 720));
    }
};

QTEST_GUILESS_MAIN(TxFilterEdgesTest)
#include "tx_filter_edges_test.moc"
