// Inject pending work directly. No socket is bound, connected or serviced.
#include "TestSettingsProfile.h"
#include "core/Kpa1500Connection.h"
#include "core/Kpa1500Discovery.h"
#include "core/Kpa1500Protocol.h"
#include "core/PeripheralSettings.h"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <functional>
#include <cstdio>
#include <optional>
#include <utility>

namespace AetherSDR {

struct Kpa1500TrackingTestAccess {
    static void seed(Kpa1500Connection& connection, bool hasFrequency = true)
    {
        connection.m_followTx = true;
        connection.m_transmitting = false;
        connection.m_trackWantKhz = hasFrequency ? std::optional<int>(14074) : std::nullopt;
        connection.m_trackSentKhz = 14074;
        connection.m_trackTimer.start(300);
        Kpa1500Connection::PendingControl frequency;
        frequency.group = QStringLiteral("FR");
        frequency.frame = QByteArrayLiteral("^FR14074;");
        frequency.satisfied = [](const Kpa1500::Message&, const Kpa1500::Status&) { return false; };
        connection.m_pending.push_back(std::move(frequency));
        Kpa1500Connection::PendingControl operate;
        operate.group = QStringLiteral("OS");
        operate.satisfied = [](const Kpa1500::Message&, const Kpa1500::Status&) { return false; };
        connection.m_pending.push_back(std::move(operate));
    }

    static bool frequencyCancelled(const Kpa1500Connection& connection)
    {
        return !connection.m_trackTimer.isActive() && !connection.m_trackSentKhz
            && std::none_of(connection.m_pending.begin(), connection.m_pending.end(),
                            [](const Kpa1500Connection::PendingControl& control) {
                                return control.group == QLatin1String("FR");
                            });
    }

    static bool operatePreserved(const Kpa1500Connection& connection)
    {
        return std::count_if(connection.m_pending.begin(), connection.m_pending.end(),
                             [](const Kpa1500Connection::PendingControl& control) {
                                 return control.group == QLatin1String("OS");
                             }) == 1;
    }

    static std::optional<int> desired(const Kpa1500Connection& connection)
    {
        return connection.m_trackWantKhz;
    }

    static bool socketsUnopened(const Kpa1500Connection& connection)
    {
        return connection.m_socket.state() == QAbstractSocket::UnconnectedState
            && connection.m_udp.state() == QAbstractSocket::UnconnectedState;
    }

    static int retryAttemptsAfterWait(Kpa1500Connection& connection)
    {
        Kpa1500Connection::PendingControl antenna;
        antenna.group = QStringLiteral("AN");
        antenna.frame = QByteArrayLiteral("^AN2;");
        antenna.readback = QStringLiteral("AN");
        antenna.lastSent.start();
        connection.m_pending.push_back(std::move(antenna));
        QThread::msleep(650);
        connection.retryPendingControls();
        for (const auto& control : connection.m_pending) {
            if (control.group == QLatin1String("AN")) {
                return control.attempts;
            }
        }
        return -1;
    }

    static int retryAttemptsNow(Kpa1500Connection& connection)
    {
        connection.retryPendingControls();
        for (const auto& control : connection.m_pending) {
            if (control.group == QLatin1String("AN")) {
                return control.attempts;
            }
        }
        return -1;
    }

    // Tracking is on, the link counts as up (no socket is opened: writeFrame
    // refuses without one), and the amp's power state is whatever `power` says.
    // "Issued" means sendTrack() built and tried to send an FR: the write
    // itself fails here, so only the attempt (m_trackLastSent) is visible.
    static bool tracksWithPower(Kpa1500Connection& connection, std::optional<bool> power)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn = power;
        connection.m_trackWantKhz = 14074;
        connection.m_trackSentKhz.reset();
        connection.sendTrack();
        const bool issued = connection.m_trackLastSent.isValid();
        connection.m_connected = false;
        return issued;
    }

    static bool firstReadingTracks(Kpa1500Connection& connection)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn.reset();
        connection.m_trackWantKhz = 14074;
        connection.m_trackSentKhz.reset();
        connection.scheduleTrack();
        if (connection.m_trackLastSent.isValid()) {
            connection.m_connected = false;
            return false;  // issued before the power state was known
        }
        connection.onMessage(*Kpa1500::parseMessage("^ON1;"));
        const bool issued = connection.m_trackLastSent.isValid();
        connection.m_connected = false;
        return issued;
    }

    // An awake, linked amp; the FR write fails because no socket is open.
    static bool failedWriteRetried(Kpa1500Connection& connection)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn = true;
        connection.m_trackWantKhz = 7100;
        connection.m_trackSentKhz = 14074;
        connection.sendTrack();
        const bool retried = connection.m_trackSentKhz == 14074 && connection.m_trackTimer.isActive();
        connection.m_trackTimer.stop();
        connection.m_connected = false;
        return retried;
    }

    // Connected, so only the TX refusal (not "not connected") can stop it.
    static QStringList refusalsFor(Kpa1500Connection& connection, bool transmitting,
                                   const std::function<void(Kpa1500Connection&)>& action)
    {
        QStringList refusals;
        const auto link = QObject::connect(&connection, &Kpa1500Connection::controlRefused,
                                           [&refusals](const QString& m) { refusals.append(m); });
        connection.m_connected = true;
        connection.setTxFrequency(std::nullopt, false, transmitting);
        action(connection);
        connection.setTxFrequency(std::nullopt, false, false);
        connection.m_connected = false;
        QObject::disconnect(link);
        return refusals;
    }

    // An awake, linked amp with TRACK on; TX ends while a new frequency is held.
    static bool frequencyHeldPastUnkey(Kpa1500Connection& connection)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_status.powerOn = true;
        connection.m_trackSentKhz = 14074;
        connection.m_trackWantKhz = 14074;
        connection.setTxFrequency(7100, false, true);   // QSY while transmitting
        connection.setTxFrequency(7100, false, false);  // unkey
        const bool held = connection.m_trackSentKhz == 14074 && connection.m_trackTimer.isActive()
            && connection.m_trackTimer.remainingTime() > Kpa1500Connection::kTrackIntervalMs;
        connection.m_connected = false;
        return held;
    }

    static bool sleepDropsPendingFrequency(Kpa1500Connection& connection)
    {
        seed(connection);
        connection.m_connected = true;
        connection.m_status.powerOn = true;
        connection.onMessage(*Kpa1500::parseMessage("^ON0;"));
        connection.m_connected = false;
        return frequencyCancelled(connection) && operatePreserved(connection);
    }

    static bool timerDeliveryHeld(Kpa1500Connection& connection)
    {
        // Simulate a timer already delivered after the TX-state transition.
        connection.m_connected = true;
        connection.sendTrack();
        connection.m_connected = false;
        return !connection.m_trackSentKhz;
    }
};

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kpa1500-tracking-state-test"));
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    int failures = 0;
    const auto check = [&failures](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) {
            ++failures;
        }
    };

    for (const std::optional<int> frequency : {std::optional<int>{}, std::optional<int>{144200}}) {
        Kpa1500Connection connection;
        Kpa1500TrackingTestAccess::seed(connection);
        connection.setTxFrequency(frequency, false, false);
        check(Kpa1500TrackingTestAccess::frequencyCancelled(connection),
              "missing or unsupported TX context revokes pending FR and timer");
        check(!Kpa1500TrackingTestAccess::desired(connection), "invalid context clears desired frequency");
        check(Kpa1500TrackingTestAccess::operatePreserved(connection), "unrelated control survives");
        connection.setTxFrequency(14074, false, false);
        check(Kpa1500TrackingTestAccess::desired(connection) == 14074,
              "same frequency can return after context loss");
        check(Kpa1500TrackingTestAccess::socketsUnopened(connection), "transport remains unopened");
    }

    {
        Kpa1500Connection connection;
        Kpa1500TrackingTestAccess::seed(connection, false);
        connection.setTxFrequency(std::nullopt, false, false);
        check(Kpa1500TrackingTestAccess::frequencyCancelled(connection),
              "repeated absent context still revokes stale work");
    }
    {
        Kpa1500Connection connection;
        Kpa1500TrackingTestAccess::seed(connection);
        connection.setTxFrequency(7100, false, true);
        check(Kpa1500TrackingTestAccess::frequencyCancelled(connection),
              "observed TX transition revokes pending FR");
        check(Kpa1500TrackingTestAccess::desired(connection) == 7100,
              "TX holds the latest desired frequency");
        check(Kpa1500TrackingTestAccess::timerDeliveryHeld(connection),
              "already delivered tracking timer cannot send during TX");
        check(Kpa1500TrackingTestAccess::operatePreserved(connection), "TX preserves other controls");
        connection.setTxFrequency(7100, false, false);
        check(Kpa1500TrackingTestAccess::desired(connection) == 7100,
              "unkey retains frequency for subsequent tracking");
        check(Kpa1500TrackingTestAccess::socketsUnopened(connection), "TX test opens no transport");
    }
    {
        Kpa1500Connection connection;
        Kpa1500TrackingTestAccess::seed(connection);
        connection.setFollowTx(false);
        check(Kpa1500TrackingTestAccess::frequencyCancelled(connection),
              "disabling tracking revokes pending FR");
        check(Kpa1500TrackingTestAccess::operatePreserved(connection),
              "disabling tracking preserves other controls");
    }
    {
        Kpa1500Connection connection;
        connection.setTxFrequency(14074, false, true);
        check(Kpa1500TrackingTestAccess::retryAttemptsAfterWait(connection) == 0,
              "a pending AN is not resent while TX is held");
        connection.setTxFrequency(14074, false, false);
        check(Kpa1500TrackingTestAccess::retryAttemptsAfterWait(connection) == 1,
              "a pending AN resumes its retry once TX ends");
        static int refusals = 0;
        const QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext&, const QString& message) {
                if (message.contains(QLatin1String("refusing sleep while transmitting"))) {
                    ++refusals;
                }
            });
        connection.setTxFrequency(14074, true, false);
        connection.setPower(false);
        check(refusals == 1, "sleep is refused while transmitting");
        connection.setTxFrequency(14074, false, false);
        connection.setPower(false);
        connection.setTxFrequency(14074, true, false);
        connection.setPower(true);
        check(refusals == 1, "sleep after TX, and wake during TX, are not refused");
        qInstallMessageHandler(previous);
        check(Kpa1500TrackingTestAccess::socketsUnopened(connection), "retry test opens no transport");
    }
    {
        // UNKEY_REQUESTED clears m_transmitting while RF may still be up; a
        // held control must not repeat on the first poll after that edge.
        Kpa1500Connection connection;
        connection.setTxFrequency(14074, false, true);
        check(Kpa1500TrackingTestAccess::retryAttemptsAfterWait(connection) == 0,
              "a pending AN is held while TX lasts longer than the retry interval");
        connection.setTxFrequency(14074, false, false);
        check(Kpa1500TrackingTestAccess::retryAttemptsNow(connection) == 0,
              "a held AN waits a full retry interval after unkey");
        check(Kpa1500TrackingTestAccess::socketsUnopened(connection),
              "unkey retry test opens no transport");
    }
    {
        Kpa1500Connection connection;
        check(!Kpa1500TrackingTestAccess::tracksWithPower(connection, std::nullopt),
              "TRACK sends no FR before the amp's power state is known");
        check(!Kpa1500TrackingTestAccess::tracksWithPower(connection, false),
              "TRACK sends no FR to a sleeping amp");
        check(Kpa1500TrackingTestAccess::tracksWithPower(connection, true),
              "TRACK issues FR to an amp known to be awake (the decision; no socket)");
        check(Kpa1500TrackingTestAccess::socketsUnopened(connection),
              "power-state test opens no transport");
        Kpa1500Connection fresh;  // no earlier ^FR, so the rate limit is not in play
        check(Kpa1500TrackingTestAccess::firstReadingTracks(fresh),
              "the first ON1 reading on a link issues the pending frequency");
        Kpa1500Connection unkeyed;
        check(Kpa1500TrackingTestAccess::frequencyHeldPastUnkey(unkeyed),
              "a frequency held under TX waits a full retry interval after unkey");
        Kpa1500Connection sleeping;
        check(Kpa1500TrackingTestAccess::sleepDropsPendingFrequency(sleeping),
              "ON0 drops a pending FR and keeps other controls");
        check(Kpa1500TrackingTestAccess::socketsUnopened(unkeyed)
                  && Kpa1500TrackingTestAccess::socketsUnopened(sleeping),
              "unkey and sleep tracking tests open no transport");
    }
    {
        Kpa1500Connection connection;
        check(Kpa1500TrackingTestAccess::failedWriteRetried(connection),
              "a failed FR write is not marked sent and is retried");
    }
    {
        using A = Kpa1500TrackingTestAccess;
        Kpa1500Connection c;
        const QStringList antenna = A::refusalsFor(c, true, [](Kpa1500Connection& k) { k.selectAntenna(2); });
        check(antenna == QStringList{QStringLiteral("Antenna 2 refused while transmitting")},
              "an antenna change is refused while transmitting, with a notice");
        const QStringList bypass = A::refusalsFor(c, true, [](Kpa1500Connection& k) { k.setAtuInline(false); });
        check(bypass == QStringList{QStringLiteral("ATU bypass refused while transmitting")},
              "an ATU in-line/bypass change is refused while transmitting, with a notice");
        check(A::refusalsFor(c, false, [](Kpa1500Connection& k) {
                  k.selectAntenna(1);
                  k.setAtuInline(true);
              }).isEmpty(),
              "antenna and ATU in-line changes are not refused when not transmitting");
        check(A::refusalsFor(c, true, [](Kpa1500Connection& k) {
                  k.startTune();
                  k.setPower(true);
              }).isEmpty(),
              "ATU tune and wake are not refused while transmitting");
        const QStringList sleep = A::refusalsFor(c, true, [](Kpa1500Connection& k) { k.setPower(false); });
        check(sleep == QStringList{QStringLiteral("Sleep refused while transmitting")},
              "a refused sleep shows \"Sleep refused while transmitting\"");
        check(A::socketsUnopened(c), "refusal tests open no transport");
    }
    {
        using F = QNetworkInterface;
        const F::InterfaceFlags lan = F::IsUp | F::IsRunning | F::CanBroadcast;
        check(Kpa1500Discovery::probesInterface(lan), "Find sweeps an up, running LAN interface");
        check(!Kpa1500Discovery::probesInterface(lan | F::IsPointToPoint),
              "Find skips a point-to-point (VPN) interface");
        check(!Kpa1500Discovery::probesInterface(lan | F::IsLoopBack), "Find skips loopback");
        check(!Kpa1500Discovery::probesInterface(F::IsUp), "Find skips an interface that is not running");
    }
    {
        PeripheralSettings::setDeviceString(QStringLiteral("Kpa1500"), QStringLiteral("Transport"),
                                            QStringLiteral("udp"));
        PeripheralSettings::setDeviceString(QStringLiteral("Kpa1500"), QStringLiteral("ManualIp"),
                                            QStringLiteral("192.0.2.15"));
        PeripheralSettings::clearDeviceConnection(QStringLiteral("Kpa1500"));
        check(PeripheralSettings::deviceString(QStringLiteral("Kpa1500"),
                                               QStringLiteral("Transport")).isEmpty(),
              "Remove clears the saved KPA1500 transport");
    }
    return failures == 0 ? 0 : 1;
}
