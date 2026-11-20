// RadioModel's host transmit time-out (RFC #7021) on the production arming,
// stop and admission paths. Socket-free: a bare RadioModel with no backend;
// the radio's keyed edge is emitted by hand and the guard's clock injected.
// Covers: arming whatever keyed the radio, expiry through the stop route,
// the notification and keying source, the release latch on producer inputs,
// and that the time-out never keys.

#include "TestSettingsProfile.h"
#include "core/backends/CwControlRequest.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/HostCwTextKeyer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/TxController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    static bool hostRequestedNow(const RadioModel& radio) { return radio.m_txRequested; }
    static void radioPttKeyed(RadioModel& radio, bool on) { radio.m_radioPttKeyed = on; }
    static void radioPttDown(RadioModel& radio, bool down) { radio.m_radioPttObservedDown = down; }
    static void txOwnedByUs(RadioModel& radio, bool ours) { radio.m_txOwnedByUs = ours; }
    static bool flex(const RadioModel& radio) { return radio.m_flexBackend != nullptr; }
    static bool admit(RadioModel& radio, const TxCoordinator::Request& request)
    {
        return radio.beginTxActivity(TxCoordinator::Activity::Mox, &request);
    }
    static void hold(RadioModel& radio, const TxCoordinator::Request& request)
    {
        radio.noteTxTimeoutHold(request);
    }
    static bool armed(const RadioModel& radio) { return radio.m_txTimeout.armed(); }
    static int holders(const RadioModel& radio) { return radio.m_txTimeout.holderCount(); }
    static void prune(RadioModel& radio) { (void)radio.m_txTimeout.refreshLatch(); }
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

// Takes every carrier edge, as a connected HL2 that granted TX does.
class CwKeyingStub final : public IRadioBackend {
public:
    RadioCapabilities caps;
    int* downs{nullptr};
    bool* keyDown{nullptr};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    ReceiveDispatch requestCwControl(const CwControlRequest& request,
                                     const TxCoordinator::Command& command) override
    {
        if (request.kind == CwControlRequest::Kind::PttEnvelope) {
            return IRadioBackend::requestCwControl(request, command);
        }
        if (!cwEdgeAdmitted(request, command, caps.canTransmit)) {
            return ReceiveDispatch::Unsupported;
        }
        *downs += request.down ? 1 : 0;
        *keyDown = request.down;
        return ReceiveDispatch::Dispatched;
    }
    QString sendCwText(const QString&, const TxCoordinator::Operation&,
                       const TxCoordinator::Completion&) override { return {}; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

bool pumpUntil(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done() || t.elapsed() >= ms) {
            loop.quit();
        }
    });
    poll.start(1);
    loop.exec();
    return done();
}

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
    TestSettingsProfile profile(QStringLiteral("tx-timeout-radio-model"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }

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

    // multiFLEX (ruling, ten9876): on a Flex, another station's over is not
    // this client's to time. It never arms, never stops and never latches.
    // Control: the same unowned over on a non-Flex backend still expires.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        Access::txOwnedByUs(radio, false);
        runOver(radio, now, 181'000);
        Access::radioKeyed(radio, false);
        report("flex_other_stations_over_is_not_timed",
               Access::flex(radio) && !rec.keys.contains(QStringLiteral("tx-timeout-expired"))
                   && !rec.keys.contains(QStringLiteral("tx-timeout-warning")) && rec.sources.isEmpty()
                   && !radio.txTimeoutLatched(),
               "flex=" + std::to_string(Access::flex(radio)) + " keys=" + rec.keys.join(QLatin1Char(',')).toStdString());

        RadioModel hl2;
        auto owned = std::make_unique<CwKeyingStub>();
        int downs = 0;
        bool keyDown = false;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        hl2.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        qint64 hl2Now = 5'000'000;
        Access::setClock(hl2, &hl2Now);
        Recorder hl2Rec(hl2);
        Access::txOwnedByUs(hl2, false);
        runOver(hl2, hl2Now, 180'000);
        report("non_flex_unowned_over_still_expires",
               !Access::flex(hl2) && hl2Rec.keys.contains(QStringLiteral("tx-timeout-expired")));
    }

    // Repeated keyed status (Flex re-sends state=TRANSMITTING within an over)
    // faster than the 250 ms tick must not restart it and starve the expiry.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        int expiries = 0;
        QObject::connect(&radio, &RadioModel::txTimeoutExpired, &radio, [&expiries] { ++expiries; });
        Access::radioKeyed(radio, true);
        now += 200'000;
        int repeats = 0;
        QTimer repeat;
        repeat.setInterval(50);
        QObject::connect(&repeat, &QTimer::timeout, &radio, [&radio, &repeats] {
            ++repeats;
            Access::radioKeyed(radio, true);
        });
        repeat.start();
        (void)pumpUntil([&expiries] { return expiries > 0; }, 1500);
        repeat.stop();
        report("repeated_keyed_status_does_not_starve_the_expiry", expiries == 1,
               "expiries=" + std::to_string(expiries) + " repeats=" + std::to_string(repeats));
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
        // ...and it does unkey: cancelLocalTransmit() withdrew the host's request.
        report("expiry_requests_the_stop", !Access::hostRequestedNow(radio));
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

        // A key-down that is not engaged rolls itself back with
        // setProducerTransmit(false) inside requestProducerPttOn(); that
        // rollback is not a release.
        (void)radio.requestProducerPttOn(tci.request(), TransmitModel::PttSource::TciHardware);
        report("refused_key_down_rollback_is_not_a_release", radio.txTimeoutLatched());

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

    // A TCI client repeats trx true while its PTT is held. One captured just
    // before the stop arrives after it, invalid, and TciServer refuses it
    // before admission (retireTrxRequest). That teardown is not a release:
    // the client's next repeat must still be refused by the latch.
    {
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        Recorder rec(radio);
        const TxCoordinator::Producer tci = radio.registerTxProducer();
        Access::hold(radio, tci.request());
        Access::radioKeyed(radio, true);
        const TxCoordinator::Request inFlight = tci.request(); // captured before the stop
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        const bool staleAfterStop = !inFlight.valid();
        const bool latched = radio.txTimeoutLatched();
        // TciServer::retireTrxRequest(): mark the refusal, then tear down.
        radio.noteTxKeyDownRefused(inFlight);
        radio.requestProducerPttOff(inFlight, TransmitModel::PttSource::Dax);
        const bool stillLatched = radio.txTimeoutLatched();
        rec.keys.clear();
        Access::clearNoticeDedup(radio);
        const bool repeatRefused = !Access::admit(radio, tci.request())
            && rec.keys.contains(QStringLiteral("tx-timeout-latched"));
        report("stale_trx_true_refused_after_stop_is_not_a_release",
               staleAfterStop && latched && stillLatched && repeatRefused);

        // The client's trx false still releases (handleTrxRequest).
        radio.noteTxInputReleased(tci.request());
        report("explicit_off_after_stale_refusal_releases", !radio.txTimeoutLatched());
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

    // Host CW text (rigctl send_morse on an HL2) is sent text, not a held
    // input: a time-out mid-message ends the message, and once the radio is
    // idle the latch clears and a fresh PTT is admitted. Both while an element
    // is down and in the gap between two elements (no CW intent open).
    for (const bool inGap : {false, true}) {
        int downs = 0;
        bool keyDown = false;
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        radio.transmitModel().setCwBreakIn(true);
        auto owned = std::make_unique<CwKeyingStub>();
        {
            hl2::Hl2Backend real;
            owned->caps = real.capabilities();
        }
        owned->caps.canTransmit = true;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        radio.emitBackendSliceChangedForTest(0, slice);
        int expiries = 0;
        QObject::connect(&radio, &RadioModel::txTimeoutExpired, &radio, [&expiries] { ++expiries; });
        HostCwTextKeyer keyer(&radio, QStringLiteral("rigctl"));
        const bool sent = keyer.send(QStringLiteral("MMMMMMMMMM"));
        const bool keyed = pumpUntil([&] { return downs >= 1 && keyDown != inGap; }, 3000);
        runOver(radio, now, 180'000);
        const bool expired = expiries == 1;
        const int downsAtExpiry = downs;
        (void)pumpUntil([] { return false; }, 600);   // the next element is refused
        const bool ended = downs == downsAtExpiry && !keyDown;
        Access::radioKeyed(radio, false);
        Access::tick(radio);
        const std::string detail = "sent=" + std::to_string(sent) + " keyed=" + std::to_string(keyed)
            + " expired=" + std::to_string(expired) + " ended=" + std::to_string(ended)
            + " latched=" + std::to_string(radio.txTimeoutLatched());
        report(inGap ? "host_cw_text_time_out_between_elements_does_not_hold_the_latch"
                     : "host_cw_text_time_out_mid_element_does_not_hold_the_latch",
               sent && keyed && expired && ended && !radio.txTimeoutLatched(), detail);
        const TxCoordinator::Producer ptt = radio.registerTxProducer();
        Access::clearNoticeDedup(radio);
        report(inGap ? "fresh_ptt_admitted_after_host_cw_text_time_out_between_elements"
                     : "fresh_ptt_admitted_after_host_cw_text_time_out_mid_element",
               Access::admit(radio, ptt.request()));
    }

    // Host CW text that ended normally earlier in the over holds nothing: a
    // footswitch then runs the over into a time-out, and its release alone
    // clears the latch once the radio is idle.
    {
        int downs = 0;
        bool keyDown = false;
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        radio.transmitModel().setCwBreakIn(true);
        auto owned = std::make_unique<CwKeyingStub>();
        {
            hl2::Hl2Backend real;
            owned->caps = real.capabilities();
        }
        owned->caps.canTransmit = true;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        radio.emitBackendSliceChangedForTest(0, slice);
        Access::radioKeyed(radio, true);
        HostCwTextKeyer keyer(&radio, QStringLiteral("rigctl"));
        const bool sent = keyer.send(QStringLiteral("E"));
        const bool done = pumpUntil([&] { return downs >= 1 && !keyDown; }, 3000);
        (void)pumpUntil([] { return false; }, 300);
        const TxCoordinator::Producer footswitch = radio.registerTxProducer();
        const TxCoordinator::Request press = footswitch.request();
        Access::hold(radio, press);
        runOver(radio, now, 180'000);
        const bool expired = radio.txTimeoutLatched();
        radio.requestProducerPttOff(press, TransmitModel::PttSource::Mox);
        Access::radioKeyed(radio, false);
        Access::tick(radio);
        const std::string detail = "sent=" + std::to_string(sent) + " done=" + std::to_string(done)
            + " expired=" + std::to_string(expired) + " holders=" + std::to_string(Access::holders(radio));
        report("finished_host_cw_text_does_not_hold_a_later_time_out",
               sent && done && expired && !radio.txTimeoutLatched(), detail);
        const TxCoordinator::Producer ptt = radio.registerTxProducer();
        Access::clearNoticeDedup(radio);
        report("fresh_ptt_admitted_after_finished_host_cw_text_and_time_out",
               Access::admit(radio, ptt.request()));
    }

    // An iambic paddle keys element by element (MainWindow's routed keyer edge
    // -> queueProducerCwKeyEdge); the paddle input, not an element, is the held
    // input. A time-out while it is held latches; element key-ups while it is
    // still held do not release; letting go of every paddle does.
    {
        int downs = 0;
        bool keyDown = false;
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        radio.transmitModel().setCwBreakIn(true);
        auto owned = std::make_unique<CwKeyingStub>();
        {
            hl2::Hl2Backend real;
            owned->caps = real.capabilities();
        }
        owned->caps.canTransmit = true;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        radio.emitBackendSliceChangedForTest(0, slice);
        const TxCoordinator::Producer paddle = radio.registerTxProducer();
        const TxCoordinator::Request input = paddle.request();
        // Like IambicKeyer: each element derives from the input, and its
        // key-up carries the same element.
        TxCoordinator::Request element;
        const auto edge = [&radio, &input, &element](bool down) {
            if (down) {
                element = input.derive();
            }
            radio.queueProducerCwKeyEdge(element, down, QStringLiteral("cw:iambic-keyer"),
                                         0, 0, std::chrono::steady_clock::now());
        };
        radio.setProducerCwPaddleHeld(input, true);
        edge(true);
        const bool keyed = pumpUntil([&] { return downs >= 1 && keyDown; }, 3000);
        edge(false);
        (void)pumpUntil([&] { return !keyDown; }, 3000);
        edge(true);
        const bool keyedAgain = pumpUntil([&] { return downs >= 2 && keyDown; }, 3000);
        runOver(radio, now, 180'000);
        Access::radioKeyed(radio, false);
        Access::tick(radio);
        const bool latched = radio.txTimeoutLatched();
        // The keyer runs on while the paddle is held: the key-up of the
        // element that was down at the stop, a refused down, its up.
        edge(false);
        edge(true);
        edge(false);
        (void)pumpUntil([] { return false; }, 300);
        Access::tick(radio);
        const std::string held = "keyed=" + std::to_string(keyed) + " again=" + std::to_string(keyedAgain)
            + " latched=" + std::to_string(latched) + " after=" + std::to_string(radio.txTimeoutLatched());
        report("paddle_element_key_ups_while_held_keep_the_latch",
               keyed && keyedAgain && latched && radio.txTimeoutLatched(), held);
        radio.setProducerCwPaddleHeld(input, false);
        Access::tick(radio);
        report("paddle_release_clears_the_latch", latched && !radio.txTimeoutLatched(),
               "holders=" + std::to_string(Access::holders(radio)));
        const TxCoordinator::Producer ptt = radio.registerTxProducer();
        Access::clearNoticeDedup(radio);
        report("fresh_ptt_admitted_after_paddle_release", Access::admit(radio, ptt.request()));
    }

    // A squeezed or memorised iambic element the keyer sends after the paddle
    // let go is not a held input: a later expiry in the same over clears its
    // latch once the radio is idle.
    {
        int downs = 0;
        bool keyDown = false;
        RadioModel radio;
        qint64 now = 5'000'000;
        Access::setClock(radio, &now);
        radio.transmitModel().setCwBreakIn(true);
        auto owned = std::make_unique<CwKeyingStub>();
        {
            hl2::Hl2Backend real;
            owned->caps = real.capabilities();
        }
        owned->caps.canTransmit = true;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        radio.emitBackendSliceChangedForTest(0, slice);
        const TxCoordinator::Producer paddle = radio.registerTxProducer();
        const TxCoordinator::Request input = paddle.request();
        TxCoordinator::Request element;
        const auto edge = [&radio, &input, &element](bool down) {
            if (down) {
                element = input.derive();
            }
            radio.queueProducerCwKeyEdge(element, down, QStringLiteral("cw:iambic-keyer"),
                                         0, 0, std::chrono::steady_clock::now());
        };
        int expiries = 0;
        QObject::connect(&radio, &RadioModel::txTimeoutExpired, &radio, [&expiries] { ++expiries; });
        Access::radioKeyed(radio, true);
        radio.setProducerCwPaddleHeld(input, true);
        edge(true);
        const bool keyed = pumpUntil([&] { return downs >= 1 && keyDown; }, 3000);
        radio.setProducerCwPaddleHeld(input, false);   // let go mid-element
        edge(false);
        (void)pumpUntil([&] { return !keyDown; }, 3000);
        edge(true);                                    // the trailing element
        const bool trailing = pumpUntil([&] { return downs >= 2 && keyDown; }, 3000);
        edge(false);
        (void)pumpUntil([&] { return !keyDown; }, 3000);
        const int holdersAfter = Access::holders(radio);
        runOver(radio, now, 180'000);
        const bool expired = expiries == 1;
        Access::radioKeyed(radio, false);
        Access::tick(radio);
        const std::string detail = "keyed=" + std::to_string(keyed) + " trailing=" + std::to_string(trailing)
            + " holders=" + std::to_string(holdersAfter) + " expired=" + std::to_string(expired)
            + " latched=" + std::to_string(radio.txTimeoutLatched());
        report("trailing_iambic_element_after_paddle_release_holds_nothing",
               keyed && trailing && holdersAfter == 0 && expired && !radio.txTimeoutLatched(), detail);
        const TxCoordinator::Producer ptt = radio.registerTxProducer();
        Access::clearNoticeDedup(radio);
        report("fresh_ptt_admitted_after_trailing_iambic_element", Access::admit(radio, ptt.request()));
    }

    // Fail closed past the holder table: with 32 live inputs holding, a 33rd
    // key-down is refused, and one of the 32 keying again is still admitted.
    {
        int downs = 0;
        bool keyDown = false;
        RadioModel radio;
        auto owned = std::make_unique<CwKeyingStub>();
        {
            hl2::Hl2Backend real;
            owned->caps = real.capabilities();
        }
        owned->caps.canTransmit = true;
        owned->downs = &downs;
        owned->keyDown = &keyDown;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        Recorder rec(radio);
        std::vector<TxCoordinator::Producer> holding;
        for (int i = 0; i < TxTimeoutGuard::kMaximumHolders; ++i) {
            holding.push_back(radio.registerTxProducer());
            Access::hold(radio, holding.back().request());
        }
        const TxCoordinator::Producer extra = radio.registerTxProducer();
        const bool admitted = Access::admit(radio, extra.request());
        report("thirty_third_holder_key_down_is_refused",
               !admitted && rec.keys.contains(QStringLiteral("tx-timeout-holders-full"))
                   && Access::holders(radio) == TxTimeoutGuard::kMaximumHolders,
               "admitted=" + std::to_string(admitted) + " holders=" + std::to_string(Access::holders(radio)));
        Access::clearNoticeDedup(radio);
        report("holding_input_keys_again_with_a_full_table",
               Access::admit(radio, holding.front().request()));
    }

    // A hold is the producer's, not the request's: noting one keeps no request
    // alive, so a finished CW element's work can still end under MOX.
    {
        RadioModel radio;
        const TxCoordinator::Producer paddle = radio.registerTxProducer();
        TxCoordinator::Request::Weak element;
        {
            const TxCoordinator::Request request = paddle.request();
            element = request.weak();
            Access::hold(radio, request);
        }
        const bool requestGone = !element.lock().valid();
        Access::prune(radio);
        const bool producerHolds = Access::holders(radio) == 1;
        paddle.invalidate();
        Access::prune(radio);
        report("hold_keeps_the_producer_not_its_request",
               requestGone && producerHolds && Access::holders(radio) == 0,
               "gone=" + std::to_string(requestGone) + " held=" + std::to_string(producerHolds)
                   + " after=" + std::to_string(Access::holders(radio)));
    }

    // Defaults and the Flex second layer, through the model getter.
    {
        RadioModel radio;
        const TxTimeoutSettings d = radio.txTimeoutSettings();
        report("model_default_enabled_180_warning_10",
               d.enabled && d.timeoutS == 180 && d.warningS == 10);
        report("unidentified_radio_does_not_store", !radio.setTxTimeoutSettings(d));
    }

    // With nothing stored the default follows the radio's own Timeout, and a
    // new one from the radio is republished (dialog, aetherd transmitTimeout).
    {
        RadioModel radio;
        int changes = 0;
        QObject::connect(&radio, &RadioModel::txTimeoutSettingsChanged, &radio, [&changes] { ++changes; });
        TransmitDelta timeout;
        timeout.interlockTimeout = 600'000;
        radio.applyBackendTransmitDeltaForTest(timeout);
        const int afterNew = changes;
        const int followedS = radio.txTimeoutSettings().timeoutS;
        radio.applyBackendTransmitDeltaForTest(timeout);
        report("radio_timeout_change_republishes_the_default",
               afterNew == 1 && followedS == 600 && changes == 1,
               "changes=" + std::to_string(changes) + " timeoutS=" + std::to_string(followedS));
    }

    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
