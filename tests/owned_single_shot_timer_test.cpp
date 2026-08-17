#include "gui/OwnedSingleShotTimer.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, line, expression);
        ++g_failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QObject dimensionOwner;
    const OwnedSingleShotTimer firstDimensionTimer = ensureOwnedSingleShotTimer(
        &dimensionOwner, QStringLiteral("dimensionTimer"), 200);
    const OwnedSingleShotTimer secondDimensionTimer = ensureOwnedSingleShotTimer(
        &dimensionOwner, QStringLiteral("dimensionTimer"), 200);
    CHECK(firstDimensionTimer.newlyCreated);
    CHECK(!secondDimensionTimer.newlyCreated);
    CHECK(firstDimensionTimer.timer == secondDimensionTimer.timer);
    CHECK(dimensionOwner.findChildren<QTimer*>(QStringLiteral("dimensionTimer"),
                                               Qt::FindDirectChildrenOnly).size() == 1);

    QSignalSpy dimensionTimeouts(firstDimensionTimer.timer, &QTimer::timeout);
    firstDimensionTimer.timer->start();
    QTest::qWait(60);
    const int dimensionRemainingBeforeRestart = firstDimensionTimer.timer->remainingTime();
    secondDimensionTimer.timer->start();
    const int dimensionRemainingAfterRestart = secondDimensionTimer.timer->remainingTime();
    CHECK(dimensionRemainingAfterRestart > dimensionRemainingBeforeRestart);
    CHECK(dimensionTimeouts.wait(500));
    CHECK(dimensionTimeouts.count() == 1);

    QObject spotOwner;
    const OwnedSingleShotTimer firstSpotTimer = ensureOwnedSingleShotTimer(
        &spotOwner, QStringLiteral("spotTimer"), 200);
    const OwnedSingleShotTimer secondSpotTimer = ensureOwnedSingleShotTimer(
        &spotOwner, QStringLiteral("spotTimer"), 200);
    CHECK(firstSpotTimer.newlyCreated);
    CHECK(!secondSpotTimer.newlyCreated);
    CHECK(firstSpotTimer.timer == secondSpotTimer.timer);

    QSignalSpy spotTimeouts(firstSpotTimer.timer, &QTimer::timeout);
    CHECK(startSingleShotTimerIfIdle(firstSpotTimer.timer));
    QTest::qWait(60);
    const int spotRemainingBeforeSchedule = firstSpotTimer.timer->remainingTime();
    CHECK(!startSingleShotTimerIfIdle(secondSpotTimer.timer));
    const int spotRemainingAfterSchedule = secondSpotTimer.timer->remainingTime();
    CHECK(spotRemainingAfterSchedule <= spotRemainingBeforeSchedule);
    CHECK(spotTimeouts.wait(500));
    CHECK(spotTimeouts.count() == 1);

    QObject independentOwner;
    const OwnedSingleShotTimer independentTimer = ensureOwnedSingleShotTimer(
        &independentOwner, QStringLiteral("dimensionTimer"), 20);
    CHECK(independentTimer.newlyCreated);
    CHECK(independentTimer.timer != firstDimensionTimer.timer);
    QSignalSpy independentTimeouts(independentTimer.timer, &QTimer::timeout);
    independentTimer.timer->start();
    CHECK(independentTimeouts.wait(500));
    CHECK(independentTimeouts.count() == 1);

    return g_failures == 0 ? 0 : 1;
}
