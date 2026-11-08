// Inject pending work directly. No socket is bound, connected or serviced.
#include "TestSettingsProfile.h"
#include "core/Kpa1500Connection.h"

#include <QCoreApplication>

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
        connection.m_pending.push_back(std::move(frequency));
        Kpa1500Connection::PendingControl operate;
        operate.group = QStringLiteral("OS");
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
    return failures == 0 ? 0 : 1;
}
