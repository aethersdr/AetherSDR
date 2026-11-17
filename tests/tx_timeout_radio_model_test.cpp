// RadioModel's host transmit time-out (RFC #7021) on the production arming,
// stop and admission paths. Socket-free: a bare RadioModel with no backend;
// the radio's keyed edge is emitted by hand and the guard's clock injected.
// Covers: arming whatever keyed the radio, expiry through the stop route,
// the notification and keying source, the release latch on producer inputs,
// and that the time-out never keys.

#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "models/TxController.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <string>

namespace AetherSDR {
class TxTimeoutTestAccess {
public:
    static void setClock(RadioModel& radio, qint64* now)
    {
        radio.m_txTimeout.setClock([now] { return *now; });
    }
    static void radioKeyed(RadioModel& radio, bool on)
    {
        radio.m_radioTransmitting = on;
        emit radio.radioTransmittingChanged(on);
    }
    static void tick(RadioModel& radio) { radio.onTxTimeoutTick(); }
    static void hostRequested(RadioModel& radio, bool on) { radio.m_txRequested = on; }
    static void radioPttKeyed(RadioModel& radio, bool on) { radio.m_radioPttKeyed = on; }
    static void radioPttDown(RadioModel& radio, bool down) { radio.m_radioPttObservedDown = down; }
    static bool admit(RadioModel& radio, const TxCoordinator::Request& request)
    {
        return radio.beginTxActivity(TxCoordinator::Activity::Mox, &request);
    }
    static void hold(RadioModel& radio, const TxCoordinator::Request& request)
    {
        radio.noteTxTimeoutHold(request);
    }
    static bool armed(const RadioModel& radio) { return radio.m_txTimeout.armed(); }
    // emitInterlockNotification() drops a repeat of the last key within 5 s.
    static void clearNoticeDedup(RadioModel& radio) { radio.m_lastInterlockNotificationKey.clear(); }
};
} // namespace AetherSDR

using namespace AetherSDR;
using Access = TxTimeoutTestAccess;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

struct Recorder {
    QStringList keys;
    QStringList messages;
    QStringList sources;
    int warnings{0};
    int engaged{0};
    int moxOn{0};
    int tuneOn{0};
    explicit Recorder(RadioModel& radio)
    {
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
                         [this](const QString& message, const QString& key, const QString&) {
            messages << message;
            keys << key;
        });
        QObject::connect(&radio, &RadioModel::txTimeoutExpired, &radio,
                         [this](int, const QString& source) { sources << source; });
        QObject::connect(&radio, &RadioModel::txTimeoutWarning, &radio,
                         [this](int) { ++warnings; });
        QObject::connect(&radio, &RadioModel::localTransmitEngaged, &radio,
                         [this] { ++engaged; });
        QObject::connect(&radio.transmitModel(), &TransmitModel::moxCommandIssued, &radio,
                         [this](bool on) { moxOn += on ? 1 : 0; });
        QObject::connect(&radio.transmitModel(), &TransmitModel::tuneCommandIssued, &radio,
                         [this](bool on) { tuneOn += on ? 1 : 0; });
    }
};

// Keys the radio, runs the clock past the limit in 250 ms ticks.
void runOver(RadioModel& radio, qint64& now, qint64 forMs)
{
    Access::radioKeyed(radio, true);
    for (qint64 t = 0; t < forMs; t += 250) {
        now += 250;
        Access::tick(radio);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Every keying path arms the one timer: what keyed the radio only names it.
    {
        struct Case {
            const char* name;
            TransmitModel::PttSource source;
            bool hostKeyed;
            bool radioPtt;
            const char* expected;
        };
        const Case cases[] = {
            {"source_mox_ptt", TransmitModel::PttSource::Mox, true, false, "MOX/PTT"},
            {"source_tci", TransmitModel::PttSource::TciHardware, true, false, "TCI"},
            {"source_serial_footswitch", TransmitModel::PttSource::Footswitch, true, false, "footswitch"},
            {"source_dax_cat_rigctl", TransmitModel::PttSource::Dax, true, false, "DAX/CAT"},
            {"source_wspr", TransmitModel::PttSource::Wspr, true, false, "WSPR"},
            {"source_tune", TransmitModel::PttSource::Tune, true, false, "TUNE"},
            {"source_atu", TransmitModel::PttSource::Atu, true, false, "ATU"},
            {"source_radio_ptt_input", TransmitModel::PttSource::Footswitch, true, true, "radio PTT input"},
            {"source_radio_reported_unowned", TransmitModel::PttSource::Mox, false, false,
             "radio-reported (not keyed by this client)"},
        };
        for (const Case& c : cases) {
            RadioModel radio;
            qint64 now = 5'000'000;
            Access::setClock(radio, &now);
            Recorder rec(radio);
            radio.transmitModel().noteActivePttSource(c.source);
            Access::hostRequested(radio, c.hostKeyed);
            Access::radioPttKeyed(radio, c.radioPtt);
            runOver(radio, now, 180'000);
            const bool expired = rec.keys.contains(QStringLiteral("tx-timeout-expired"));
            const QString message = rec.messages.isEmpty() ? QString() : rec.messages.last();
            report(c.name,
                   expired && rec.sources == QStringList{QString::fromLatin1(c.expected)}
                       && message.startsWith(QStringLiteral(
                              "Transmit time-out: transmission stopped after 180 s.")),
                   rec.sources.join(QLatin1Char(',')).toStdString());
        }
    }

    // Not a tick early.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        runOver(radio, now, 179'750);
        const bool early = rec.keys.contains(QStringLiteral("tx-timeout-expired"));
        now += 250;
        Access::tick(radio);
        report("expires_at_limit_not_before",
               !early && rec.keys.contains(QStringLiteral("tx-timeout-expired")));
    }

    // The warning is a notice plus the signal the desktop turns into a tone.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        runOver(radio, now, 170'000);
        report("warning_notice_and_signal_once_at_10s",
               rec.warnings == 1 && rec.keys.contains(QStringLiteral("tx-timeout-warning"))
                   && rec.messages.contains(QStringLiteral("Transmit time-out in 10 s.")));
    }

    // An unkey disarms: a 179 s over followed by another never trips.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        runOver(radio, now, 179'000);
        Access::radioKeyed(radio, false);
        runOver(radio, now, 179'000);
        report("unkey_disarms_and_next_over_restarts",
               !rec.keys.contains(QStringLiteral("tx-timeout-expired")) && Access::armed(radio));
    }

    // The time-out only unkeys: no MOX-on, TUNE-on or local engage on expiry.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        Access::hostRequested(radio, true);
        runOver(radio, now, 181'000);
        report("expiry_never_keys",
               rec.keys.contains(QStringLiteral("tx-timeout-expired")) && rec.engaged == 0
                   && rec.moxOn == 0 && rec.tuneOn == 0 && !radio.transmitModel().isMox()
                   && !radio.transmitModel().isTuning());
    }

    // The HL2 jack still held after the stop: the notice says so.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        Access::radioPttDown(radio, true);
        runOver(radio, now, 180'000);
        report("held_radio_ptt_is_named_in_the_notice",
               !rec.messages.isEmpty()
                   && rec.messages.last().contains(QStringLiteral("PTT input is still held")));
    }

    // Latch: a held producer input keeps every key-down out after the radio is
    // idle; its release reopens admission.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        const TxCoordinator::Producer tci = radio.registerTxProducer();
        const TxCoordinator::Request held = tci.request();
        Access::hold(radio, held);
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        const bool latched = radio.txTimeoutLatched();
        rec.keys.clear();
        const bool admitted = Access::admit(radio, tci.request());
        const bool refusedByLatch = !admitted && rec.keys.contains(QStringLiteral("tx-timeout-latched"));
        // Another input is refused too while this one holds.
        const TxCoordinator::Producer footswitch = radio.registerTxProducer();
        rec.keys.clear();
        Access::clearNoticeDedup(radio);
        const bool otherRefused = !Access::admit(radio, footswitch.request())
            && rec.keys.contains(QStringLiteral("tx-timeout-latched"));
        report("held_input_latches_after_unkey", latched && refusedByLatch && otherRefused);

        // The refused key-down's own rollback is not a release.
        rec.keys.clear();
        Access::clearNoticeDedup(radio);
        (void)radio.requestProducerPttOn(tci.request(), TransmitModel::PttSource::TciHardware);
        report("refused_key_down_rollback_is_not_a_release",
               rec.keys.contains(QStringLiteral("tx-timeout-latched")) && radio.txTimeoutLatched());

        // A producer abandoning the refused key (TciServer::abortTciPtt) is not either.
        const TxCoordinator::Request refused = tci.request();
        (void)Access::admit(radio, refused);
        radio.abortProducerPtt(refused, TransmitModel::PttSource::TciHardware);
        report("abort_of_a_refused_key_is_not_a_release", radio.txTimeoutLatched());

        // Its explicit release is.
        (void)radio.setProducerTransmit(tci.request(), false, TransmitModel::PttSource::TciHardware);
        rec.keys.clear();
        Access::clearNoticeDedup(radio);
        (void)Access::admit(radio, tci.request());
        report("release_then_new_key_down_is_admitted_past_the_latch",
               !radio.txTimeoutLatched() && !rec.keys.contains(QStringLiteral("tx-timeout-latched")));
    }

    // A serial footswitch's release edge releases.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        const TxCoordinator::Producer footswitch = radio.registerTxProducer();
        const TxCoordinator::Request press = footswitch.request();
        Access::hold(radio, press);
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        const bool latched = radio.txTimeoutLatched();
        radio.requestProducerPttOff(press, TransmitModel::PttSource::Mox);
        report("footswitch_release_clears_latch", latched && !radio.txTimeoutLatched());
    }

    // A half-open client: its socket's lifetime ends, and that releases it.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        const TxCoordinator::Producer client = radio.registerTxProducer();
        Access::hold(radio, client.request());
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        Access::tick(radio);
        const bool latched = radio.txTimeoutLatched();
        client.invalidate();
        Access::tick(radio);
        report("producer_lifetime_end_releases", latched && !radio.txTimeoutLatched());
    }

    // The local controls are reset by the stop: their next press is fresh.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        const std::shared_ptr<TxController> local = radio.localTxController();
        const TxCoordinator::Producer other = radio.registerTxProducer();
        // The exemption is by producer identity, not every request.
        report("unrelated_producer_is_not_the_local_controller",
               local && !local->ownsRequest(other.request()));
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        report("nothing_held_clears_latch_when_radio_idle", !radio.txTimeoutLatched());
    }

    // The radio still keyed keeps the latch with nothing held.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        runOver(radio, now, 180'000);
        const bool whileKeyed = radio.txTimeoutLatched();
        Access::radioKeyed(radio, false);
        report("latched_while_radio_keyed_cleared_when_idle",
               whileKeyed && !radio.txTimeoutLatched());
    }

    // Defaults and the Flex second layer, through the model getter.
    {
        RadioModel radio;
        const TxTimeoutSettings d = radio.txTimeoutSettings();
        report("model_default_enabled_180_warning_10",
               d.enabled && d.timeoutS == 180 && d.warningS == 10);
        report("unidentified_radio_does_not_store", !radio.setTxTimeoutSettings(d));
    }

    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
