// Inject pending work directly. No socket is bound, connected or serviced.
#include "TestSettingsProfile.h"
#include "core/Kpa1500Connection.h"
#include "core/Kpa1500Protocol.h"
#include "core/PeripheralSettings.h"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
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
    static bool tracksWithPower(Kpa1500Connection& connection, std::optional<bool> power)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn = power;
        connection.m_trackWantKhz = 14074;
        connection.m_trackSentKhz.reset();
        connection.sendTrack();
        const bool sent = connection.m_trackSentKhz.has_value();
        connection.m_connected = false;
        return sent;
    }

    static std::optional<int> firstReadingTracks(Kpa1500Connection& connection)
    {
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn.reset();
        connection.m_trackWantKhz = 14074;
        connection.m_trackSentKhz.reset();
        connection.scheduleTrack();
        if (connection.m_trackSentKhz) {
            connection.m_connected = false;
            return -1;  // sent before the power state was known
        }
        connection.onMessage(*Kpa1500::parseMessage("^ON1;"));
        const std::optional<int> sent = connection.m_trackSentKhz;
        connection.m_connected = false;
        return sent;
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
        check(Kpa1500TrackingTestAccess::firstReadingTracks(fresh) == 14074,
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
