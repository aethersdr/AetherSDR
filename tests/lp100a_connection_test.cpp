#include "core/LpMeterConnection.h"
#include "core/LogManager.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QIODevice>
#include <QList>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cstdio>

namespace {

QStringList warnings;

void captureWarning(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg && QString::fromLatin1(context.category) == QStringLiteral("aether.tuner")) {
        warnings.append(message);
    }
}

// Byte sink only: no firmware model, peer, socket or serial device.
class PollSink final : public QIODevice {
public:
    PollSink() { open(QIODevice::WriteOnly | QIODevice::Unbuffered); }
    qint64 result{1};
    int attempts{0};
    QByteArray accepted;

protected:
    qint64 readData(char*, qint64) override { return 0; }
    qint64 writeData(const char* data, qint64 size) override
    {
        ++attempts;
        if (result == size) {
            accepted.append(data, size);
        }
        return result;
    }
};

bool containsWarning(const QString& text)
{
    return std::any_of(warnings.cbegin(), warnings.cend(), [&text](const QString& warning) {
        return warning.contains(text);
    });
}

bool hasRecoveryDuration(double minimumSeconds = 0.0)
{
    const QRegularExpression pattern(QStringLiteral("records flowing again after ([0-9]+[.][0-9]) s"));
    return std::any_of(warnings.cbegin(), warnings.cend(), [&pattern, minimumSeconds](const QString& warning) {
        const QRegularExpressionMatch match = pattern.match(warning);
        return match.hasMatch() && match.captured(1).toDouble() >= minimumSeconds;
    });
}

} // namespace

namespace AetherSDR {

class LpMeterConnectionTest {
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
        {
            PollSink sink;
            LpMeterConnection connection;
            start(connection, sink);
            warnings.clear();
            record(connection);
            check("healthy first record adds no warning", warnings.isEmpty());
            dim(connection);
            record(connection);
            check("silence and recovery both survive the warning-level filter",
                  warnings.size() == 2 && containsWarning(QStringLiteral("link up but no valid record"))
                  && hasRecoveryDuration());
            record(connection);
            check("subsequent records do not repeat the recovery", warnings.size() == 2);
            connection.onTransportDown();
            start(connection, sink);
            warnings.clear();
            record(connection);
            check("a closed outage stays closed after reconnect", warnings.isEmpty());
        }
        {
            PollSink sink;
            LpMeterConnection connection;
            connection.prepareConnection(LpMeterConnection::Mode::Network, QStringLiteral("meter-a"), 2000);
            start(connection, sink);
            record(connection);
            warnings.clear();
            dim(connection);
            connection.onTransportDown();
            connection.prepareConnection(LpMeterConnection::Mode::Network, QStringLiteral("meter-a"), 2000);
            start(connection, sink);
            record(connection);
            check("same-endpoint reconnect keeps the original warning/recovery pair",
                  warnings.size() == 2 && hasRecoveryDuration());
        }
        {
            PollSink sink;
            LpMeterConnection connection;
            start(connection, sink);
            warnings.clear();
            QMetaObject::invokeMethod(&connection.m_dataWatchdog, "timeout", Qt::DirectConnection);
            record(connection);
            check("startup silence at the clock origin pairs with the first record",
                  warnings.size() == 2 && hasRecoveryDuration());
        }
        for (const qint64 result : {qint64{-1}, qint64{0}}) {
            PollSink sink;
            sink.result = result;
            LpMeterConnection connection;
            start(connection, sink);
            warnings.clear();
            connection.pollTick();
            dim(connection);
            check(result < 0 ? "rejected poll cannot report a sent poll"
                             : "zero-byte write cannot report a sent poll",
                  sink.attempts == 1 && sink.accepted.isEmpty()
                  && containsWarning(QStringLiteral("no poll of ours sent since connect"))
                  && !containsWarning(QStringLiteral("our last poll")));
        }
        {
            PollSink sink;
            LpMeterConnection connection;
            start(connection, sink);
            connection.pollTick();
            check("accepted write records exactly the poll byte", sink.accepted == QByteArray(1, LpMeter::kPollCommand));
            const qint64 acceptedAt = connection.m_lastPollMs;
            QThread::msleep(2);
            sink.result = -1;
            connection.pollTick();
            warnings.clear();
            dim(connection);
            check("a failed retry preserves the previous accepted poll timestamp",
                  connection.m_lastPollMs == acceptedAt
                  && containsWarning(QStringLiteral("our last poll")));
        }
        const QList<LpMeterConnection::Mode> oldModes{
            LpMeterConnection::Mode::Network, LpMeterConnection::Mode::Network,
            LpMeterConnection::Mode::Serial, LpMeterConnection::Mode::Network};
        const QList<LpMeterConnection::Mode> newModes{
            LpMeterConnection::Mode::Network, LpMeterConnection::Mode::Network,
            LpMeterConnection::Mode::Serial, LpMeterConnection::Mode::Serial};
        const QStringList newEndpoints{QStringLiteral("meter-b"), QStringLiteral("meter-a"),
                                       QStringLiteral("serial-b"), QStringLiteral("meter-a")};
        const char* descriptions[]{"changing host clears the old outage",
                                   "changing port clears the old outage",
                                   "changing serial device clears the old outage",
                                   "changing transport mode clears the old outage"};
        for (qsizetype i = 0; i < oldModes.size(); ++i) {
            PollSink sink;
            LpMeterConnection connection;
            const QString oldEndpoint = oldModes[i] == LpMeterConnection::Mode::Serial
                ? QStringLiteral("serial-a") : QStringLiteral("meter-a");
            connection.prepareConnection(oldModes[i], oldEndpoint, 2000);
            start(connection, sink);
            record(connection);
            dim(connection);
            connection.prepareConnection(newModes[i], newEndpoints[i], i == 1 ? 2001 : 2000);
            start(connection, sink);
            warnings.clear();
            record(connection);
            check(descriptions[i], warnings.isEmpty());
        }
        {
            PollSink sink;
            LpMeterConnection connection;
            connection.prepareConnection(LpMeterConnection::Mode::Serial, QStringLiteral("serial-a"));
            start(connection, sink);
            record(connection);
            warnings.clear();
            dim(connection);
            connection.prepareConnection(LpMeterConnection::Mode::Serial, QStringLiteral("serial-a"));
            start(connection, sink);
            record(connection);
            check("same serial endpoint retains its outage across reconnect",
                  warnings.size() == 2 && hasRecoveryDuration());
        }
        return failed == 0 ? 0 : 1;
    }

private:
    static void start(LpMeterConnection& connection, PollSink& sink)
    {
        connection.m_device = &sink;
        connection.onTransportUp();
        connection.m_pollTimer.stop();
        connection.m_dataWatchdog.stop();
    }

    static void record(LpMeterConnection& connection)
    {
        // Captured idle record from lp100a_protocol_test; parser input only.
        connection.m_parser.feed(QByteArray(1, LpMeter::kRecordMarker)
            + QByteArray("0000.00,046.3,088.5,0,NF0T  ,2,1,-2.3,1.00"));
        connection.m_dataWatchdog.stop();
    }

    static void dim(LpMeterConnection& connection)
    {
        connection.m_lastRecordMs = connection.nowMs();
        QMetaObject::invokeMethod(&connection.m_dataWatchdog, "timeout", Qt::DirectConnection);
    }
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("lp100a_connection_test"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("aether.tuner.debug=false\naether.tuner.info=false\naether.tuner.warning=true"));
    const QtMessageHandler previous = qInstallMessageHandler(captureWarning);
    const int result = AetherSDR::LpMeterConnectionTest::run();
    qInstallMessageHandler(previous);
    return result;
}
