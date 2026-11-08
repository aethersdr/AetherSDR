#include "core/LpMeterConnection.h"
#include "TestSettingsProfile.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QThread>

#include <chrono>
#include <cstdio>

namespace AetherSDR {

class LpMeterConnectionClockTest {
public:
    static int run()
    {
        int failed = 0;
        const auto check = [&failed](const char* description, bool ok) {
            std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", description);
            if (!ok) {
                ++failed;
            }
        };

        const auto beforeConstruction = std::chrono::steady_clock::now();
        LpMeterConnection polling;
        const qint64 origin = polling.nowMs();
        const qint64 constructionMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beforeConstruction).count();
        // Epoch milliseconds cannot satisfy this bound. It pins the production
        // clock source without changing the OS clock or interposing Qt symbols.
        check("connection time starts at a nonnegative elapsed origin",
              origin >= 0 && origin <= constructionMs + 1);

        const auto beforeSleep = std::chrono::steady_clock::now();
        const qint64 before = polling.nowMs();
        QThread::msleep(20);
        const qint64 after = polling.nowMs();
        const qint64 sleepMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - beforeSleep).count();
        check("connection time advances with steady elapsed time",
              after > before && after - before <= sleepMs + 1);

        QBuffer polls;
        QBuffer rangeBytes;
        QBuffer malformedBytes;
        LpMeterConnection range;
        LpMeterConnection malformed;
        check("memory transports open",
              polls.open(QIODevice::ReadWrite)
                  && rangeBytes.open(QIODevice::ReadWrite)
                  && malformedBytes.open(QIODevice::ReadWrite));
        attach(polling, polls);
        attach(range, rangeBytes);
        attach(malformed, malformedBytes);

        const qint64 foreignNow = polling.nowMs();
        polling.m_gate.onRecord(foreignNow - 2);
        polling.m_gate.onRecord(foreignNow - 1);
        polling.m_gate.onRecord(foreignNow);
        check("a confirmed poller suppresses the immediate poll",
              !polling.m_gate.shouldPoll(foreignNow) && polling.isRidingAlong());

        LpMeter::Reading lowRange;
        lowRange.powerRange = 2;
        range.onReading(lowRange);
        check("range contraction waits for the elapsed hold",
              range.m_range.displayedRange() == 0 && range.m_lastRecordMs >= 0);

        malformedBytes.buffer() = "not a meter record";
        malformedBytes.seek(0);
        malformed.onReadyRead();
        check("initial malformed bytes start an elapsed deadline without warning",
              malformed.m_firstBytesMs >= 0 && !malformed.m_bytesWithoutRecords);

        // No event-loop dispatch: only the connection handlers below run.
        // One wait crosses the gate, range-hold and malformed-stream deadlines.
        QThread::msleep(3100);
        polling.pollTick();
        check("polling resumes once the confirmed poller has gone quiet",
              polls.data() == QByteArray(1, LpMeter::kPollCommand)
                  && !polling.isRidingAlong());

        range.onReading(lowRange);
        check("the held range contracts after elapsed time",
              range.m_range.displayedRange() == 2);

        malformedBytes.buffer() = "still not a meter record";
        malformedBytes.seek(0);
        malformed.onReadyRead();
        check("malformed input reaches its elapsed warning deadline",
              malformed.m_bytesWithoutRecords);

        const qint64 reconnectBefore = polling.nowMs();
        polling.disconnect();
        attach(polling, polls);
        const qint64 reconnectAfter = polling.nowMs();
        check("reconnect preserves the clock epoch and resets session timestamps",
              reconnectAfter >= reconnectBefore
                  && polling.m_lastRecordMs == -1 && polling.m_firstBytesMs == -1);

        const qint64 recordBefore = polling.nowMs();
        polling.onReading(LpMeter::Reading{});
        const qint64 recordAfter = polling.nowMs();
        check("record timestamps use the connection clock",
              polling.m_lastRecordMs >= recordBefore
                  && polling.m_lastRecordMs <= recordAfter);

        polling.disconnect();
        range.disconnect();
        malformed.disconnect();
        return failed == 0 ? 0 : 1;
    }

private:
    static void attach(LpMeterConnection& connection, QBuffer& memory)
    {
        connection.m_device = &memory;
        connection.onTransportUp();
        connection.m_pollTimer.stop();
        connection.m_dataWatchdog.stop();
    }
};

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    const TestSettingsProfile profile(QStringLiteral("lp100a-clock-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    return AetherSDR::LpMeterConnectionClockTest::run();
}
