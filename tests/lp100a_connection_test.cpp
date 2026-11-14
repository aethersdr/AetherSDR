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
        radioCable(check);
        return failed == 0 ? 0 : 1;
    }

private:
    struct RadioRig {
        LpMeter::CableSnapshot cable;
        QList<LpMeter::CableChange> changes;
        QList<QPair<QString, QByteArray>> writes;
        bool accept{true};
        int connectedCount{0};
        int disconnectedCount{0};
        QStringList failures;
        QList<LpMeter::Reading> readings;

        void attach(LpMeterConnection& connection)
        {
            LpMeterConnection::RadioCableHooks hooks;
            hooks.snapshot = [this](const QString& serial) {
                return serial == QStringLiteral("ABBDMO0I") ? cable : LpMeter::CableSnapshot{};
            };
            hooks.configure = [this](const QString&, const LpMeter::CableChange& change) {
                changes.append(change);
                return accept;
            };
            hooks.write = [this](const QString& serial, const QByteArray& bytes) {
                writes.append({serial, bytes});
                return accept;
            };
            connection.setRadioCableHooks(std::move(hooks));
            QObject::connect(&connection, &LpMeterConnection::connected, [this] { ++connectedCount; });
            QObject::connect(&connection, &LpMeterConnection::disconnected, [this] { ++disconnectedCount; });
            QObject::connect(&connection, &LpMeterConnection::connectionFailed,
                             [this](const QString& e) { failures.append(e); });
            QObject::connect(&connection, &LpMeterConnection::readingUpdated,
                             [this](const LpMeter::Reading& r) { readings.append(r); });
        }
    };

    static LpMeter::CableSnapshot catCable()
    {
        // As the radio first reports the meter's FT232R (FLEX-8600, 4.3.16).
        return {true, true, QStringLiteral("cat"), 9600, 8, QStringLiteral("none"), 1,
                QStringLiteral("none")};
    }

    template <typename Check>
    static void radioCable(const Check& check)
    {
        using Setting = LpMeter::CableChange::Setting;
        const QString sn = QStringLiteral("ABBDMO0I");
        {
            RadioRig rig;
            rig.cable = catCable();
            LpMeterConnection connection;
            connection.setAutoReconnect(true);
            rig.attach(connection);
            connection.connectRadioCable(sn);
            check("a CAT cable is first switched to passthrough",
                  rig.changes.size() == 1 && rig.changes[0].setting == Setting::Type
                  && rig.changes[0].text == QStringLiteral("passthrough") && !connection.isConnected());
            connection.onRadioCableStatus(sn, rig.cable);
            check("an unchanged status does not resend the pending setting", rig.changes.size() == 1);
            rig.cable.type = QStringLiteral("passthrough");
            connection.onRadioCableStatus(sn, rig.cable);
            check("the speed is set only after the radio confirms the type",
                  rig.changes.size() == 2 && rig.changes[1].setting == Setting::Speed
                  && rig.changes[1].number == 115200 && !connection.isConnected());
            rig.cable.speed = 115200;
            connection.onRadioCableStatus(sn, rig.cable);
            check("a confirmed passthrough 115200 8N1 cable brings the link up",
                  connection.isConnected() && rig.connectedCount == 1
                  && connection.sourceLabel() == QStringLiteral("RADIO")
                  && connection.description() == QStringLiteral("radio USB cable ABBDMO0I")
                  && !connection.m_cableConfigureTimer.isActive());
            connection.m_pollTimer.stop();
            connection.m_dataWatchdog.stop();

            connection.pollTick();
            check("a poll is written through the radio hook",
                  rig.writes.size() == 1 && rig.writes[0].first == sn
                  && rig.writes[0].second == QByteArray(1, LpMeter::kPollCommand));
            // Measured: the radio often splits one record across two statuses.
            connection.onRadioCableData(sn, QByteArray(";0000.00,047.5,071.6,0,LP-"));
            connection.onRadioCableData(QStringLiteral("OTHER"), QByteArray(";9999.99"));
            connection.onRadioCableData(sn, QByteArray("100,2,1,-2.3,1.00"));
            connection.onRadioCableData(sn, QByteArray(";"));
            connection.m_dataWatchdog.stop();
            check("a record split across statuses decodes once; other cables are ignored",
                  rig.readings.size() == 1 && rig.readings[0].zOhms > 47.4 && rig.readings[0].zOhms < 47.6);

            rig.cable.type = QStringLiteral("cat");
            rig.cable.speed = 9600;
            connection.onRadioCableStatus(sn, rig.cable);
            check("a cable another client moves off passthrough drops the link and is not taken back",
                  !connection.isConnected() && rig.disconnectedCount == 1 && rig.changes.size() == 2
                  && !connection.reconnectPending());

            rig.cable = LpMeter::CableSnapshot{};
            connection.onRadioCableStatus(sn, rig.cable);
            rig.cable = catCable();
            connection.onRadioCableStatus(sn, rig.cable);
            check("a replugged cable is configured again",
                  rig.changes.size() == 3 && rig.changes[2].setting == Setting::Type);
        }
        {
            RadioRig rig;
            LpMeterConnection connection;
            rig.attach(connection);
            connection.connectRadioCable(sn);
            check("an unreported cable is reported to the operator and waited for",
                  rig.changes.isEmpty() && !connection.isConnected() && rig.failures.size() == 1);
            rig.cable = catCable();
            connection.onRadioCableStatus(sn, rig.cable);
            check("the cable is configured once the radio reports it", rig.changes.size() == 1);
            connection.disconnect();
            rig.cable.type = QStringLiteral("passthrough");
            connection.onRadioCableStatus(sn, rig.cable);
            check("after disconnect the cable is left alone", rig.changes.size() == 1 && !connection.isConnected());
        }
        {
            RadioRig rig;
            rig.cable = catCable();
            rig.cable.type = QStringLiteral("passthrough");
            rig.cable.speed = 115200;
            LpMeterConnection connection;
            connection.setAutoReconnect(false);
            rig.attach(connection);
            connection.connectRadioCable(sn);
            const bool up = connection.isConnected() && rig.changes.isEmpty();
            connection.onRadioCableStatus(sn, LpMeter::CableSnapshot{});
            rig.cable = catCable();
            connection.onRadioCableStatus(sn, rig.cable);
            check("without auto-reconnect a dropped radio cable stays down and untouched",
                  up && !connection.isConnected() && rig.changes.isEmpty());
        }
        {
            RadioRig rig;
            rig.cable = catCable();
            rig.accept = false;
            LpMeterConnection connection;
            rig.attach(connection);
            connection.connectRadioCable(sn);
            rig.accept = true;
            rig.cable.type = QStringLiteral("bit");
            connection.onRadioCableStatus(sn, rig.cable);
            check("a refused setting fails the attempt and stops configuring",
                  rig.failures.size() == 1 && rig.changes.size() == 1 && !connection.isConnected());
        }
        {
            RadioRig rig;
            rig.cable = catCable();
            LpMeterConnection connection;
            rig.attach(connection);
            connection.connectRadioCable(sn);
            const bool armed = connection.m_cableConfigureTimer.isActive();
            QMetaObject::invokeMethod(&connection.m_cableConfigureTimer, "timeout", Qt::DirectConnection);
            check("a setting the radio never confirms fails the attempt",
                  armed && rig.failures.size() == 1 && rig.failures[0].contains(QStringLiteral("type passthrough")));
        }
        {
            // Auto-reconnect switched off while the cable is gone cancels the
            // resume the drop left pending.
            RadioRig rig;
            rig.cable = catCable();
            rig.cable.type = QStringLiteral("passthrough");
            rig.cable.speed = 115200;
            LpMeterConnection connection;
            connection.setAutoReconnect(true);
            rig.attach(connection);
            connection.connectRadioCable(sn);
            const bool up = connection.isConnected();
            connection.onRadioCableStatus(sn, LpMeter::CableSnapshot{});
            connection.setAutoReconnect(false);
            rig.cable = catCable();
            connection.onRadioCableStatus(sn, rig.cable);
            const bool untouched = rig.changes.isEmpty() && !connection.m_cableConfigureTimer.isActive();
            rig.cable.type = QStringLiteral("passthrough");
            rig.cable.speed = 115200;
            connection.onRadioCableStatus(sn, rig.cable);
            check("auto-reconnect turned off during an outage cancels the pending resume",
                  up && untouched && rig.connectedCount == 1 && !connection.isConnected());
            connection.connectRadioCable(sn);
            check("an operator Connect afterwards still connects", connection.isConnected()
                  && rig.connectedCount == 2);
        }
        {
            RadioRig rig;
            rig.cable = catCable();
            rig.cable.type = QStringLiteral("bit");
            LpMeterConnection connection;
            rig.attach(connection);
            connection.connectRadioCable(sn, LpMeterConnection::RadioCableIntent::Saved);
            check("a saved cable that is now a BIT cable is refused at startup, not retyped",
                  rig.changes.isEmpty() && rig.failures.size() == 1
                  && rig.failures[0].contains(QStringLiteral("bit")) && !connection.isConnected());
            rig.cable.type = QStringLiteral("passthrough");
            connection.onRadioCableStatus(sn, rig.cable);
            check("a refused cable is left alone afterwards", rig.changes.isEmpty());
            rig.cable.type = QStringLiteral("bit");
            connection.connectRadioCable(sn);
            check("an operator Connect may retype a BIT cable",
                  rig.changes.size() == 1 && rig.changes[0].setting == Setting::Type);
        }
        {
            RadioRig rig;
            rig.cable = catCable();
            rig.cable.type = QStringLiteral("passthrough");
            rig.cable.speed = 115200;
            LpMeterConnection connection;
            connection.setAutoReconnect(true);
            rig.attach(connection);
            connection.connectRadioCable(sn);
            const bool up = connection.isConnected();
            connection.onRadioCableStatus(sn, LpMeter::CableSnapshot{});
            rig.cable.type = QStringLiteral("ldpa");
            connection.onRadioCableStatus(sn, rig.cable);
            check("a cable replugged as another type is refused, not retyped",
                  up && rig.changes.isEmpty() && rig.failures.size() == 1 && !connection.isConnected());
        }
        {
            RadioRig rig;
            LpMeterConnection connection;
            rig.attach(connection);
            connection.connectRadioCable(sn);   // not reported yet: nothing was confirmed
            rig.cable = catCable();
            rig.cable.type = QStringLiteral("bcd");
            connection.onRadioCableStatus(sn, rig.cable);
            check("a cable first reported after Connect as another type is refused",
                  rig.changes.isEmpty() && rig.failures.size() == 2);
        }
        {
            LpMeterConnection connection;
            connection.setPollIntervalMs(250);
            check("the poll interval reaches the timer and stretches the watchdog",
                  connection.pollIntervalMs() == 250 && connection.m_pollTimer.interval() == 250
                  && connection.m_dataWatchdog.interval() == 3150);
            connection.setPollIntervalMs(20);
            check("the poll interval is clamped", connection.pollIntervalMs() == 100
                  && connection.m_dataWatchdog.interval() == 3000);
        }
    }

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
