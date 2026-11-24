// Inject pending work directly. No socket is bound, connected or serviced.
#include "TestSettingsProfile.h"
#include "core/Kpa1500Connection.h"
#include "core/Kpa1500Discovery.h"
#include "core/Kpa1500Protocol.h"
#include "core/PeripheralSettings.h"

#include <QCoreApplication>
#include <QNetworkAddressEntry>
#include <QThread>

#include <algorithm>
#include <functional>
#include <cstdio>
#include <string>
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
            && connection.m_udp.state() == QAbstractSocket::UnconnectedState
            && connection.m_wakeSocket.state() == QAbstractSocket::UnconnectedState;
    }

    struct TxRetry {
        int standbyAfterFirst{-1};
        int standbyAfterSecond{-1};   // -1: expired and removed
        int tuneCancel{-1};
        int faultClear{-1};
        int operate{-1};
        int atu{-1};
        int antenna{-1};
        QStringList unconfirmed;
    };

    // Under TX, two retry intervals: STANDBY starts one attempt short of the
    // limit, so it is resent on the first and expires on the second.
    static TxRetry retriesUnderTx(Kpa1500Connection& c)
    {
        TxRetry out;
        const auto link = QObject::connect(&c, &Kpa1500Connection::controlUnconfirmed,
                                           [&out](const QString& l) { out.unconfirmed.append(l); });
        c.setTxFrequency(14074, false, true);
        auto add = [&c](Kpa1500Connection::PendingControl p, int attempts) {
            p.attempts = attempts;
            p.lastSent.start();
            c.m_pending.push_back(std::move(p));
        };
        add(Kpa1500Connection::operateControl(false), Kpa1500Connection::kMaxControlAttempts - 1);
        add(Kpa1500Connection::cancelTuneControl(), 1);
        add(Kpa1500Connection::clearFaultControl(), 1);
        add(Kpa1500Connection::atuInlineControl(false), 1);
        Kpa1500Connection::PendingControl antenna;
        antenna.group = QStringLiteral("AN");
        antenna.frame = QByteArrayLiteral("^AN2;");
        antenna.readback = QStringLiteral("AN");
        add(std::move(antenna), 1);
        auto attempts = [&c](const QString& group, const QString& label = QString()) {
            for (const auto& p : c.m_pending) {
                if (p.group == group && (label.isEmpty() || p.label == label)) {
                    return p.attempts;
                }
            }
            return -1;
        };
        QThread::msleep(650);
        c.retryPendingControls();
        out.standbyAfterFirst = attempts(QStringLiteral("OS"));
        // OPERATE (^OS1) replaces nothing here: it is checked on its own below.
        QThread::msleep(650);
        c.retryPendingControls();
        out.standbyAfterSecond = attempts(QStringLiteral("OS"));
        out.tuneCancel = attempts(QStringLiteral("TUNE"));
        out.faultClear = attempts(QStringLiteral("FL"));
        out.atu = attempts(QStringLiteral("AI"));
        out.antenna = attempts(QStringLiteral("AN"));
        c.m_pending.clear();
        add(Kpa1500Connection::operateControl(true), 1);
        QThread::msleep(650);
        c.retryPendingControls();
        out.operate = attempts(QStringLiteral("OS"));
        c.m_pending.clear();
        c.setTxFrequency(14074, false, false);
        QObject::disconnect(link);
        return out;
    }

    // A UDP reply that confirms STANDBY also releases the pacer, and the cycle
    // it ends had deferred a poll. That poll (and its retry pass) must see the
    // confirmation: the control is due for its last retry when the reply lands.
    static std::pair<int, bool> confirmingReplyBeforeDeferredPoll(Kpa1500Connection& c)
    {
        int unconfirmed = 0;
        const auto link = QObject::connect(&c, &Kpa1500Connection::controlUnconfirmed,
                                           [&unconfirmed](const QString&) { ++unconfirmed; });
        c.m_activeTransport = Kpa1500::Transport::Udp;
        c.m_udpOpen = true;
        c.m_connected = true;
        c.m_status.powerOn = true;
        c.m_status.operate = true;
        auto standby = Kpa1500Connection::operateControl(false);
        standby.attempts = 5;   // kMaxControlAttempts: the next retry pass expires it
        standby.lastSent.start();
        c.m_pending.push_back(std::move(standby));
        QThread::msleep(650);
        c.m_pacer.enqueue(QStringLiteral("OS"));
        c.m_pacer.takeNext(c.m_clock.elapsed());   // ^OS; outstanding (nothing written)
        c.m_pollDeferred = true;
        c.m_lastRxMs = c.m_clock.elapsed();
        c.onMessage(*Kpa1500::parseMessage("^OS0;"));
        const bool confirmed = c.m_pending.empty();
        QObject::disconnect(link);
        c.m_pacer.clear();
        c.m_paceTimer.stop();
        c.m_pollTimer.stop();
        c.m_udpOpen = false;
        c.m_connected = false;
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        return {unconfirmed, confirmed};
    }

    // An amp that never answers ^MA or ^WL (firmware before 01.67): the slow
    // rotation stops asking after kMaxWakeQueryAsks. Writes fail (no socket).
    static std::pair<int, int> wakeQueryAsksAfterRotations(Kpa1500Connection& c, int rotations)
    {
        c.m_activeTransport = Kpa1500::Transport::Udp;
        c.m_udpOpen = true;
        c.m_connected = true;
        c.m_status.powerOn = true;
        c.m_lastRxMs = c.m_clock.elapsed();
        for (int i = 0; i < rotations * 14; ++i) {
            c.poll();
        }
        const std::pair<int, int> asks{c.m_maAsks, c.m_wlAsks};
        c.m_pacer.clear();
        c.m_paceTimer.stop();
        c.m_udpOpen = false;
        c.m_connected = false;
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        return asks;
    }

    struct SleepDrop {
        QStringList events;          // "down", "asleep:1", "asleep:0", "up"
        bool asleepOffline{false};
        bool canWake{false};
        int wakePackets{0};
        bool packetIsMagic{false};
        bool ampTargeted{false};
        bool afterRelink{true};
    };

    // UDP: the link drops (silence) while the amp is `asleep`, then POWER is
    // pressed, then the amp answers. The wake sender is replaced: nothing
    // leaves the process.
    static SleepDrop sleepThenDrop(Kpa1500Connection& c, bool asleep, const QString& mac)
    {
        SleepDrop out;
        const auto l1 = QObject::connect(&c, &Kpa1500Connection::disconnected,
                                         [&out]() { out.events << QStringLiteral("down"); });
        const auto l2 = QObject::connect(&c, &Kpa1500Connection::connected,
                                         [&out]() { out.events << QStringLiteral("up"); });
        const auto l3 = QObject::connect(&c, &Kpa1500Connection::asleepOfflineChanged, [&out](bool on) {
            out.events << (on ? QStringLiteral("asleep:1") : QStringLiteral("asleep:0"));
        });
        const QByteArray magic = Kpa1500::buildWakeOnLanPacket(
            Kpa1500::parseMacAddress(mac).value_or(QByteArray()));
        c.m_wakeSender = [&out, magic](const QByteArray& packet, const QHostAddress& target, quint16 port) {
            ++out.wakePackets;
            out.packetIsMagic = !magic.isEmpty() && packet == magic && port == 9;
            out.ampTargeted = out.ampTargeted || target == QHostAddress(QStringLiteral("192.0.2.15"));
            return static_cast<qint64>(packet.size());
        };
        c.m_lastHost = QStringLiteral("192.0.2.15");
        c.m_activeTransport = Kpa1500::Transport::Udp;
        c.m_udpOpen = true;
        c.m_connected = true;
        c.m_status.powerOn = !asleep;
        c.setWakeMacAddress(mac);
        c.dropSilentLink();
        out.asleepOffline = c.isAsleepOffline();
        out.canWake = c.canWakeFromSleep();
        c.setPower(true);
        c.onMessage(*Kpa1500::parseMessage("^ON0;"));   // the amp answers again
        out.afterRelink = c.isAsleepOffline();
        QObject::disconnect(l1);
        QObject::disconnect(l2);
        QObject::disconnect(l3);
        c.m_pollTimer.stop();
        c.m_paceTimer.stop();
        c.m_pacer.clear();
        c.m_udpOpen = false;
        c.m_connected = false;
        c.m_lastHost.clear();
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        return out;
    }

    // TCP by host name, link down: the address resolved at link-up is the
    // wake target, and ^WL0 read before the drop does not stop the packet.
    static bool hostnameWakeTarget(Kpa1500Connection& c)
    {
        bool targeted = false;
        c.m_wakeSender = [&targeted](const QByteArray&, const QHostAddress& target, quint16) {
            targeted = targeted || target == QHostAddress(QStringLiteral("192.0.2.15"));
            return static_cast<qint64>(102);
        };
        c.m_lastHost = QStringLiteral("kpa1500.lan");
        c.m_peerAddress = QHostAddress(QStringLiteral("192.0.2.15"));
        c.m_connected = true;
        c.m_status.powerOn = false;
        c.m_status.netWake = false;
        c.setWakeMacAddress(QStringLiteral("54:10:EC:14:75:22"));
        c.onTransportDown();
        c.m_reconnectTimer.stop();
        c.m_wakeSentAt.invalidate();
        c.sendWakeOnLan(true);   // wakeFromSleep() without the redial
        c.disconnect();
        c.m_lastHost.clear();
        return targeted;
    }

    // TCP: a dropped, confirmed-asleep link. A failed reconnect attempt is
    // retried only within the window after a wake (auto-reconnect off).
    static std::pair<bool, bool> tcpWakeRetries(Kpa1500Connection& c)
    {
        c.m_connected = true;
        c.m_status.powerOn = false;
        c.onTransportDown();
        const bool asleep = c.isAsleepOffline();
        c.onTransportError(QStringLiteral("refused"));
        const bool retriedBeforeWake = c.m_reconnectTimer.isActive();
        c.m_wakeSentAt.start();   // what wakeFromSleep() records (it would also dial)
        c.onTransportError(QStringLiteral("refused"));
        const bool retriedAfterWake = c.m_reconnectTimer.isActive();
        c.m_reconnectTimer.stop();
        c.disconnect();
        return {asleep && !retriedBeforeWake, retriedAfterWake && !c.isAsleepOffline()};
    }

    // TCP, link up, nothing received for `quietMs`; the amp's last power
    // reading is `power`. No socket: the check runs before the open-link test.
    static std::pair<bool, bool> tcpQuiet(Kpa1500Connection& c, qint64 quietMs, std::optional<bool> power)
    {
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        c.m_connected = true;
        c.m_status.powerOn = power;
        c.m_lastRxMs = c.m_clock.elapsed() - quietMs;
        c.poll();
        const std::pair<bool, bool> out{!c.m_connected, c.isAsleepOffline()};
        c.m_reconnectTimer.stop();
        c.disconnect();
        return out;
    }

    // A ^ON0 sent `agoMs` ago, never confirmed (power still reads on), then
    // the link drops.
    static bool unconfirmedSleepThenDrop(Kpa1500Connection& c, qint64 agoMs)
    {
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        c.m_connected = true;
        c.m_status.powerOn = true;
        c.setWakeMacAddress(QStringLiteral("54:10:EC:14:75:22"));
        c.m_sleepSentMs = c.m_clock.elapsed() - agoMs;
        c.onTransportDown();
        const bool asleep = c.isAsleepOffline();
        c.m_reconnectTimer.stop();
        c.disconnect();
        return asleep;
    }

    // No link to the saved address has come up in this run and the attempt
    // fails: with a saved MAC, the ASLEEP/POWER wake offer appears.
    static bool startupFailureOffersWake(Kpa1500Connection& c, const QString& mac, bool linkSeen)
    {
        c.m_lastHost = QStringLiteral("192.0.2.15");
        c.setWakeMacAddress(mac);
        c.m_linkUpSeen = linkSeen;
        c.onTransportError(QStringLiteral("Connection timed out"));
        const bool offered = c.isAsleepOffline();
        c.m_reconnectTimer.stop();
        c.disconnect();
        c.m_lastHost.clear();
        return offered;
    }

    struct Probe {
        int disconnects{0};
        int connects{0};
        bool downButOpen{false};
        bool reconnectArmed{true};
        bool statusCleared{false};
        bool upAgain{false};
    };

    // A UDP link that went silent: no socket is opened (writes fail), the
    // link state is injected.
    static Probe silenceThenReply(Kpa1500Connection& c, bool autoReconnect)
    {
        Probe out;
        const auto down = QObject::connect(&c, &Kpa1500Connection::disconnected,
                                           [&out]() { ++out.disconnects; });
        const auto up = QObject::connect(&c, &Kpa1500Connection::connected, [&out]() { ++out.connects; });
        c.m_activeTransport = Kpa1500::Transport::Udp;
        c.m_udpOpen = true;
        c.m_connected = true;
        c.m_autoReconnect = autoReconnect;
        c.m_status.powerOn = true;
        c.m_status.serialNumber = QStringLiteral("00123");
        c.m_pollTimer.start();
        c.dropSilentLink();
        out.downButOpen = !c.m_connected && c.m_udpOpen && c.m_pollTimer.isActive();
        out.reconnectArmed = c.m_reconnectTimer.isActive();
        out.statusCleared = !c.m_status.powerOn && !c.m_status.serialNumber;
        c.onMessage(*Kpa1500::parseMessage("^ON1;"));
        out.upAgain = c.m_connected && c.m_status.powerOn == true;
        QObject::disconnect(down);
        QObject::disconnect(up);
        c.m_pollTimer.stop();
        c.m_paceTimer.stop();
        c.m_reconnectTimer.stop();
        c.m_udpOpen = false;
        c.m_connected = false;
        c.m_activeTransport = Kpa1500::Transport::Tcp;
        return out;
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
        connection.m_trackLastSent.invalidate();
        connection.m_trackTimer.stop();
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
    // Every write fails here: after kMaxControlAttempts the retry stops and
    // the control is reported unconfirmed.
    static bool failedWriteGivesUp(Kpa1500Connection& connection)
    {
        int unconfirmed = 0;
        const auto link = QObject::connect(&connection, &Kpa1500Connection::controlUnconfirmed,
                                           [&unconfirmed](const QString&) { ++unconfirmed; });
        connection.m_followTx = true;
        connection.m_connected = true;
        connection.m_transmitting = false;
        connection.m_status.powerOn = true;
        connection.m_trackWantKhz = 7100;
        connection.m_trackSentKhz = 14074;
        for (int i = 0; i < Kpa1500Connection::kMaxControlAttempts; ++i) {
            connection.m_trackLastSent.invalidate();
            connection.m_trackTimer.stop();
            connection.sendTrack();
        }
        const bool gaveUp = unconfirmed == 1 && !connection.m_trackTimer.isActive()
            && connection.m_trackSentKhz == 14074;
        QObject::disconnect(link);
        connection.m_connected = false;
        return gaveUp;
    }

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

    // TX has just ended (RF may still be up): one click in the tail, then
    // one after it.
    static QStringList unkeyTailRefusals(Kpa1500Connection& c)
    {
        c.m_connected = true;
        c.setTxFrequency(std::nullopt, false, true);
        c.setTxFrequency(std::nullopt, false, false);
        QStringList tail;
        const auto link = QObject::connect(&c, &Kpa1500Connection::controlRefused,
                                           [&tail](const QString& m) { tail.append(m); });
        c.selectAntenna(2);
        c.m_txEndedAt.invalidate();
        c.selectAntenna(2);   // the tail is over: not refused
        QObject::disconnect(link);
        c.m_connected = false;
        return tail;
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
        connection.m_txEndedAt.invalidate();   // each call starts clear of an unkey tail
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
        Kpa1500Connection failing;
        check(Kpa1500TrackingTestAccess::failedWriteGivesUp(failing),
              "an FR that cannot be written is given up after the attempt limit, with a notice");
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
        const QStringList tail = A::unkeyTailRefusals(c);
        check(tail == QStringList{QStringLiteral("Antenna 2 refused while transmitting")},
              "an antenna change in the unkey tail is refused; after the tail it is not");
        check(A::socketsUnopened(c), "refusal tests open no transport");
    }
    {
        // Finding 1: STANDBY, tune cancel and fault clear keep retrying (and
        // can expire) under TX; relay controls and OPERATE are held.
        Kpa1500Connection c;
        const auto r = Kpa1500TrackingTestAccess::retriesUnderTx(c);
        check(r.standbyAfterFirst == 5,   // kMaxControlAttempts
              "a pending STANDBY (^OS0) is resent while transmitting");
        check(r.standbyAfterSecond == -1 && r.unconfirmed == QStringList{QStringLiteral("Standby")},
              "a pending STANDBY expires under TX with \"Standby not confirmed\"");
        check(r.tuneCancel == 3 && r.faultClear == 3,
              "a pending tune cancel and fault clear are resent while transmitting");
        check(r.atu == 1 && r.antenna == 1, "a pending ATU in-line/bypass or antenna change is held under TX");
        check(r.operate == 1, "a pending OPERATE (^OS1) is held under TX");
        check(Kpa1500TrackingTestAccess::socketsUnopened(c), "TX retry test opens no transport");
    }
    {
        Kpa1500Connection c;
        const auto [unconfirmed, confirmed] =
            Kpa1500TrackingTestAccess::confirmingReplyBeforeDeferredPoll(c);
        check(confirmed && unconfirmed == 0,
              "a UDP reply confirms its control before the deferred poll it releases runs");
        check(Kpa1500TrackingTestAccess::socketsUnopened(c), "deferred-poll test opens no transport");
    }
    {
        Kpa1500Connection c;
        const auto [ma, wl] = Kpa1500TrackingTestAccess::wakeQueryAsksAfterRotations(c, 6);
        check(ma == 3 && wl == 3, "unanswered ^MA and ^WL stop being asked after three slow rotations");
        check(Kpa1500TrackingTestAccess::socketsUnopened(c), "wake-query test opens no transport");
    }
    {
        // A confirmed-asleep amp keeps its applet, and POWER wakes it.
        using A = Kpa1500TrackingTestAccess;
        const QString mac = QStringLiteral("54:10:EC:14:75:22");
        Kpa1500Connection asleep;
        const auto r = A::sleepThenDrop(asleep, true, mac);
        check(r.asleepOffline && r.events.mid(0, 2) == QStringList({QStringLiteral("down"), QStringLiteral("asleep:1")}),
              "a drop after a confirmed sleep enters the asleep-offline state (applet stays up)");
        check(r.canWake, "with a saved MAC, POWER can wake the amp from that state");
        check(r.wakePackets >= 1 && r.packetIsMagic && r.ampTargeted,
              "POWER from that state sends the Wake-on-LAN packet to the saved MAC, at the amp");
        check(!r.afterRelink && r.events.contains(QStringLiteral("asleep:0")) && r.events.contains(QStringLiteral("up")),
              "the amp answering again ends the asleep-offline state");
        Kpa1500Connection awake;
        const auto w = A::sleepThenDrop(awake, false, mac);
        check(!w.asleepOffline && !w.events.contains(QStringLiteral("asleep:1")) && w.wakePackets == 0,
              "a drop while the amp was awake hides the applet as before (no asleep state, no wake)");
        Kpa1500Connection noMac;
        const auto n = A::sleepThenDrop(noMac, true, QString());
        check(n.asleepOffline && !n.canWake && n.wakePackets == 0,
              "without a saved MAC, POWER cannot wake the amp and sends nothing");
        Kpa1500Connection tcp;
        const auto [heldBeforeWake, retriedAndCleared] = A::tcpWakeRetries(tcp);
        check(heldBeforeWake, "TCP: a confirmed-asleep drop does not redial before POWER (auto-reconnect off)");
        check(retriedAndCleared, "TCP: after a wake, a failed redial is retried; disconnect() ends the state");
        Kpa1500Connection named;
        check(A::hostnameWakeTarget(named),
              "offline, a host-name amp is woken at the address resolved at link-up, even after ^WL0");
        check(A::socketsUnopened(named) && A::socketsUnopened(asleep) && A::socketsUnopened(awake) && A::socketsUnopened(noMac)
                  && A::socketsUnopened(tcp),
              "asleep-offline tests open no transport");
    }
    {
        using A = Kpa1500TrackingTestAccess;
        // TCP silence, the same rule as UDP.
        Kpa1500Connection t1;
        check(A::tcpQuiet(t1, 4000, true) == std::make_pair(true, false),
              "TCP: no bytes for longer than the silence limit drops the link (awake: no asleep state)");
        Kpa1500Connection t2;
        check(A::tcpQuiet(t2, 4000, false) == std::make_pair(true, true),
              "TCP: a silent link to a sleeping amp enters the asleep-offline state");
        Kpa1500Connection t3;
        check(A::tcpQuiet(t3, 2900, true).first == false,
              "TCP: a gap under the silence limit (e.g. between slow polls) keeps the link");
        // A sleep whose readback never came.
        Kpa1500Connection u1;
        check(A::unconfirmedSleepThenDrop(u1, 1000),
              "a sleep sent just before the drop, unconfirmed, enters the asleep-offline state");
        Kpa1500Connection u2;
        check(!A::unconfirmedSleepThenDrop(u2, 6100),   // kSleepDropWindowMs + 100
              "a sleep sent longer ago than the window does not count");
        // An amp already asleep when this run started.
        Kpa1500Connection s1;
        check(A::startupFailureOffersWake(s1, QStringLiteral("54:10:EC:14:75:22"), false),
              "no link yet and a saved MAC: a failed attempt offers the wake (ASLEEP, POWER)");
        Kpa1500Connection s2;
        check(!A::startupFailureOffersWake(s2, QString(), false),
              "no saved MAC: a failed attempt offers no wake");
        Kpa1500Connection s3;
        check(!A::startupFailureOffersWake(s3, QStringLiteral("54:10:EC:14:75:22"), true),
              "after a link came up this run, a failed reconnect offers no wake (an awake drop hides)");
        check(A::socketsUnopened(t1) && A::socketsUnopened(t2) && A::socketsUnopened(t3)
                  && A::socketsUnopened(u1) && A::socketsUnopened(s1),
              "silence, unconfirmed-sleep and startup-wake tests open no transport");
    }
    for (const bool autoReconnect : {false, true}) {
        // Finding 3: UDP keeps probing after the silence drop.
        Kpa1500Connection c;
        const auto p = Kpa1500TrackingTestAccess::silenceThenReply(c, autoReconnect);
        const char* const suffix = autoReconnect ? " (auto-reconnect on)" : " (auto-reconnect off)";
        check(p.disconnects == 1 && p.downButOpen,
              (std::string("UDP silence reports the link down and keeps probing") + suffix).c_str());
        check(!p.reconnectArmed, (std::string("UDP silence does not arm a reconnect") + suffix).c_str());
        check(p.statusCleared, (std::string("UDP silence forgets the old link's readings") + suffix).c_str());
        check(p.upAgain && p.connects == 1,
              (std::string("the first reply after UDP silence brings the link back up") + suffix).c_str());
        check(Kpa1500TrackingTestAccess::socketsUnopened(c), "UDP silence test opens no transport");
    }
    {
        // Wake-on-LAN targets: the amp's subnet broadcast, then the amp.
        QNetworkAddressEntry lan;
        lan.setIp(QHostAddress(QStringLiteral("192.168.1.10")));
        lan.setPrefixLength(24);
        lan.setBroadcast(QHostAddress(QStringLiteral("192.168.1.255")));
        QNetworkAddressEntry other;
        other.setIp(QHostAddress(QStringLiteral("10.0.0.5")));
        other.setPrefixLength(8);
        other.setBroadcast(QHostAddress(QStringLiteral("10.255.255.255")));
        const QList<QNetworkAddressEntry> entries{lan, other, lan};
        check(Kpa1500Connection::wakeTargets(QHostAddress(QStringLiteral("192.168.1.60")), entries)
                  == QList<QHostAddress>{QHostAddress(QStringLiteral("192.168.1.255")),
                                         QHostAddress(QStringLiteral("192.168.1.60"))},
              "Wake-on-LAN goes to the amp's subnet broadcast and to the amp");
        check(Kpa1500Connection::wakeTargets(QHostAddress(QStringLiteral("172.16.0.9")), entries)
                  == QList<QHostAddress>{QHostAddress(QStringLiteral("172.16.0.9"))},
              "an amp on no local subnet gets the unicast packet only");
        check(Kpa1500Connection::wakeTargets(QHostAddress(), entries).isEmpty()
                  && Kpa1500Connection::wakeTargets(QHostAddress(QStringLiteral("fe80::1")), entries).isEmpty(),
              "no Wake-on-LAN target without an IPv4 address for the amp");
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
        PeripheralSettings::setDeviceString(QStringLiteral("Kpa1500"), QStringLiteral("MacAddress"),
                                            QStringLiteral("54:10:EC:14:75:22"));
        PeripheralSettings::clearDeviceConnection(QStringLiteral("Kpa1500"));
        check(PeripheralSettings::deviceString(QStringLiteral("Kpa1500"),
                                               QStringLiteral("MacAddress")).isEmpty(),
              "Remove clears the saved KPA1500 MAC address");
    }
    return failures == 0 ? 0 : 1;
}
