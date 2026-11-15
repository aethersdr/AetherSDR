// The client-side voice keyer's VOX hold against a RadioModel and a backend.
// The #6292 review found it was about ORDER — which answer releases the hold,
// and what a restore still in flight means for the next recording — so it is
// driven through the real controller, TransmitModel and RadioModel rather than
// a fake keyer. (The disconnect unkey needs the fenced Flex writer, so it lives
// in tx_operation_integration_test.)
//
// Socket-free: the backend is a recorder injected with setBackendForTest() and
// no connection is opened.
//
// Run: ./build/voice_keyer_radio_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/VoiceKeyerSettings.h"
#include "core/backends/IRadioBackend.h"
#include "models/LocalVoiceKeyer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/VoiceKeyer.h"
#include "models/VoiceKeyerController.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <string>

using namespace AetherSDR;

namespace AetherSDR {
// Declared a friend by RadioModel for the voice keyer's tests.
class VoiceKeyerTxRouteTestAccess {
public:
    static void transmitDelta(RadioModel& radio, const TransmitDelta& delta)
    {
        radio.applyBackendTransmitDelta(delta);
    }
};
}  // namespace AetherSDR

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

void spin(int ms = 0)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

// Records what reaches the radio. VOX requests are kept so a test can answer
// them the way the radio does, by ticket.
class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    bool answers{true};
    QStringList* wire;
    QList<TransmitControlRequest> vox;

    explicit RecordingBackend(QStringList& record) : wire(&record)
    {
        caps.canTransmit = true;
        caps.voxControl = RadioCapabilities::VoxControl{/*hasDelay*/ true};   // as on a Flex
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool on, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override
    {
        *wire << (on ? QStringLiteral("xmit 1") : QStringLiteral("xmit 0"));
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    ReceiveDispatch requestTransmitControl(const TransmitControlRequest& request,
                                           TunePowerContext) override
    {
        using Field = TransmitControlRequest::Field;
        if (request.field == Field::VoxEnabled) {
            vox << request;
            *wire << QStringLiteral("vox_enable=%1").arg(int(std::get<VoxSetting>(request.value).enabled));
        } else if (request.field == Field::Dax) {
            *wire << QStringLiteral("dax=%1").arg(int(std::get<bool>(request.value)));
        }
        return ReceiveDispatch::Dispatched;
    }
    bool answersTransmitControl() const override { return answers; }

    // The radio's reply to the newest VOX request.
    quint64 lastTicket() const { return vox.isEmpty() ? 0 : vox.last().answerTicket; }
    bool lastAskedOn() const { return !vox.isEmpty() && std::get<VoxSetting>(vox.last().value).enabled; }
    void answer(quint64 ticket, bool applied) { emit transmitControlAnswered(ticket, applied); }
};

struct Bench {
    QStringList wire;
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    std::unique_ptr<VoiceKeyerController> ctl;
    QStringList failures;

    Bench()
    {
        auto owned = std::make_unique<RecordingBackend>(wire);
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0))
            qFatal("Could not install the slice fixture");
        SliceDelta slice;
        slice.txSlice = true;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(slice);
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        backend->connected = true;   // after the fixture, which wants no live radio

        ctl = std::make_unique<VoiceKeyerController>(radio, nullptr, nullptr);
        QObject::connect(keyer(), &VoiceKeyer::commandFailed, keyer(),
                         [this](const QString& verb, int id, uint, const QString& message) {
            failures << QStringLiteral("%1 %2: %3").arg(verb).arg(id).arg(message);
        });
        wire.clear();
    }
    VoiceKeyer* keyer() const { return ctl->bound(); }
    void radioReportsVox(bool on)
    {
        TransmitDelta delta;
        delta.voxEnable = on;
        VoiceKeyerTxRouteTestAccess::transmitDelta(radio, delta);
    }
    int voxRequests() const { return backend->vox.size(); }
};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("voice_keyer_radio_test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    VoiceKeyerSettings::setSource(VoiceKeyerSourceSetting::Local);

    // ── The VOX hold is released only by the answer to ITS OWN request ──────
    // Before #6292's review any successful vox_enable reply confirmed the hold,
    // VOX-on included. The answer that can still arrive at the wrong moment is
    // a real one: REC, STOP before the radio answered, REC again — the first
    // hold's late "applied" must not start the second recording, whose own
    // VOX-off has not been answered yet.
    {
        Bench b;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        const quint64 first = b.backend->lastTicket();
        const bool askedOff = b.voxRequests() == 1 && !b.backend->lastAskedOn() && first != 0;
        b.keyer()->recStop(1);                        // STOP while waiting
        const quint64 restore = b.backend->lastTicket();
        b.keyer()->recStart(2);                       // and REC again at once
        const quint64 second = b.backend->lastTicket();
        b.backend->answer(first, true);               // the abandoned hold's answer
        b.backend->answer(second + 1000, true);       // and one nobody asked for
        spin();
        const bool stillWaiting = b.keyer()->status() == VoiceKeyer::Idle
                                  && !b.keyer()->canStartOperation();
        b.backend->answer(restore, true);
        spin();
        const bool stillWaitingAfterRestore = b.keyer()->status() == VoiceKeyer::Idle;
        b.backend->answer(second, true);
        spin();
        report("hold_is_released_only_by_its_own_answer",
               askedOff && second != first && stillWaiting && stillWaitingAfterRestore
                   && b.keyer()->status() == VoiceKeyer::Recording && b.keyer()->activeId() == 2,
               b.wire.join(",").toStdString());
        b.keyer()->recStop(2);
    }

    // ── VOX changed by the operator while the radio is answering ───────────
    // The radio applies requests in order, so an "applied" for the hold's VOX
    // off says nothing once the operator has turned VOX on after it. The hold
    // fails, and the operator's change is left alone rather than undone.
    {
        Bench b;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        const quint64 hold = b.backend->lastTicket();
        b.radio.transmitModel().setVoxEnable(true);   // the operator's click
        const int before = b.voxRequests();
        b.backend->answer(hold, true);
        spin();
        report("an_operator_vox_change_mid_hold_fails_it_and_is_kept",
               b.keyer()->status() == VoiceKeyer::Idle && b.keyer()->canStartOperation()
                   && b.failures.size() == 1 && b.failures[0].contains(QLatin1String("VOX was changed"))
                   && b.voxRequests() == before   // no restore sent over the operator
                   && b.radio.transmitModel().voxEnable(),
               (b.failures.join(" | ") + QStringLiteral(" wire=") + b.wire.join(",")).toStdString());
    }

    // ── A REC started before the previous restore is answered holds again ──
    // The restore is in flight, so the model still reads VOX off; recording
    // straight away would let VOX come back on underneath it.
    {
        Bench b;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        b.backend->answer(b.backend->lastTicket(), true);
        spin();
        b.keyer()->recStop(1);   // ends the take; the release restores VOX
        const quint64 restore = b.backend->lastTicket();
        const bool restoring = b.backend->lastAskedOn();
        const bool modelStillOff = !b.radio.transmitModel().voxEnable();
        b.failures.clear();

        b.keyer()->recStart(2);   // before the restore's answer
        const bool heldAgain = b.voxRequests() == 3 && !b.backend->lastAskedOn()
                               && b.keyer()->status() == VoiceKeyer::Idle;
        const quint64 hold2 = b.backend->lastTicket();
        b.backend->answer(restore, true);
        spin();
        const bool notYet = b.keyer()->status() == VoiceKeyer::Idle;
        b.backend->answer(hold2, true);
        spin();
        report("rec_before_the_restore_is_answered_holds_vox_again",
               restoring && modelStillOff && heldAgain && notYet
                   && b.keyer()->status() == VoiceKeyer::Recording
                   && !b.radio.transmitModel().voxEnable(),
               b.wire.join(",").toStdString());
        b.keyer()->recStop(2);
    }

    // ── VOX turned on while the recording runs ──────────────────────────────
    {
        Bench b;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        b.backend->answer(b.backend->lastTicket(), true);
        spin();
        const bool recording = b.keyer()->status() == VoiceKeyer::Recording;
        const int before = b.voxRequests();
        b.radio.transmitModel().setVoxEnable(true);   // the operator turns VOX back on
        const int afterClick = b.voxRequests();
        spin();
        report("vox_turned_on_mid_recording_stops_it_unsaved",
               recording && b.keyer()->status() == VoiceKeyer::Idle
                   && b.failures.size() == 1 && b.failures[0].contains(QLatin1String("not saved"))
                   && b.keyer()->recordings()[0].durationMs == 0
                   && afterClick == before + 1 && b.voxRequests() == afterClick,
               (b.failures.join(" | ") + QStringLiteral(" wire=") + b.wire.join(",")).toStdString());
    }

    // ── VOX turned off by the operator while recording: no restore over it ──
    {
        Bench b;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        b.backend->answer(b.backend->lastTicket(), true);
        spin();
        b.radio.transmitModel().setVoxEnable(false);
        const int before = b.voxRequests();
        b.keyer()->recStop(1);
        report("an_operator_vox_off_mid_recording_is_not_undone",
               b.voxRequests() == before && !b.radio.transmitModel().voxEnable(),
               b.wire.join(",").toStdString());
    }

    // ── A backend that cannot hear the reply never confirms the hold ───────
    // Dispatched is not "applied": recording is refused promptly, not after
    // the 3 s backstop, rather than started on an assumption.
    {
        Bench b;
        b.backend->answers = false;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        spin();
        report("an_unanswering_backend_refuses_recording_at_once",
               b.keyer()->status() == VoiceKeyer::Idle && b.keyer()->canStartOperation()
                   && b.failures.size() == 1
                   && b.failures[0].contains(QLatin1String("did not confirm")),
               b.failures.join(" | ").toStdString());
    }

    // ── VOX turned off by the operator, REC pressed before the radio answers ─
    // (#6292 re-review.) setVoxEnable(false) writes the model at once and sends
    // an unticketed request, so the model reads off before the radio has said
    // so — and the radio may answer late or refuse. Recording asks again with
    // its own ticket and waits; off is what the operator wanted, so nothing is
    // put back afterwards.
    {
        Bench b;
        b.radioReportsVox(true);
        b.radio.transmitModel().setVoxEnable(false);   // the operator's click
        const int afterClick = b.voxRequests();
        const bool clickUnticketed = afterClick == 1 && b.backend->lastTicket() == 0;
        b.keyer()->recStart(1);
        const quint64 confirm = b.backend->lastTicket();
        const bool askedAgain = b.voxRequests() == 2 && confirm != 0 && !b.backend->lastAskedOn();
        const bool waits = b.keyer()->status() == VoiceKeyer::Idle && !b.keyer()->canStartOperation();
        b.backend->answer(confirm, true);
        spin();
        const bool records = b.keyer()->status() == VoiceKeyer::Recording;
        b.keyer()->recStop(1);
        spin();
        report("rec_after_an_unanswered_operator_vox_off_confirms_it_first",
               clickUnticketed && askedAgain && waits && records
                   && b.voxRequests() == 2   // no restore: off was wanted
                   && !b.radio.transmitModel().voxEnable(),
               b.wire.join(",").toStdString());
    }

    // ...and when the radio refuses that VOX-off, recording does not start.
    {
        Bench b;
        b.radioReportsVox(true);
        b.radio.transmitModel().setVoxEnable(false);
        b.keyer()->recStart(1);
        b.backend->answer(b.backend->lastTicket(), false);
        spin();
        report("a_refused_vox_off_refuses_the_recording",
               b.keyer()->status() == VoiceKeyer::Idle && b.keyer()->canStartOperation()
                   && b.failures.size() == 1 && b.failures[0].contains(QLatin1String("did not confirm"))
                   && b.voxRequests() == 2,   // and nothing switched VOX on
               (b.failures.join(" | ") + QStringLiteral(" wire=") + b.wire.join(",")).toStdString());
    }

    // VOX the radio has reported off needs no round trip at all.
    {
        Bench b;
        b.radioReportsVox(false);
        b.keyer()->recStart(1);
        report("a_radio_reported_vox_off_records_at_once",
               b.keyer()->status() == VoiceKeyer::Recording && b.voxRequests() == 0,
               b.wire.join(",").toStdString());
        b.keyer()->recStop(1);
    }

    // A radio with no VOX AetherSDR controls (the Hermes-Lite 2) cannot be
    // keyed by VOX, so recording neither holds nor asks, whatever the flag says.
    {
        Bench b;
        b.backend->caps.voxControl = std::nullopt;
        b.radioReportsVox(true);
        b.keyer()->recStart(1);
        const bool recordedWithoutHold = b.keyer()->status() == VoiceKeyer::Recording
                                         && b.voxRequests() == 0;
        // Its VOX flag means nothing to the radio, so a click on it does not
        // end the recording either.
        b.radio.transmitModel().setVoxEnable(true);
        spin();
        report("a_radio_without_vox_records_without_a_hold",
               recordedWithoutHold && b.keyer()->status() == VoiceKeyer::Recording
                   && b.failures.isEmpty(),
               b.wire.join(",").toStdString());
        b.keyer()->recStop(1);
    }

    // VOX turned on during a recording that started with VOX already off —
    // no hold taken — stops it as surely as one that held VOX off (#6292
    // review). VOX is left on, as the operator asked, and nothing is restored.
    {
        Bench b;
        b.radioReportsVox(false);
        b.keyer()->recStart(1);
        const bool recordingWithoutHold = b.keyer()->status() == VoiceKeyer::Recording
                                          && b.voxRequests() == 0;
        b.radio.transmitModel().setVoxEnable(true);   // the operator's click
        spin();
        report("vox_on_stops_a_recording_that_needed_no_hold",
               recordingWithoutHold && b.keyer()->status() == VoiceKeyer::Idle
                   && b.failures.size() == 1 && b.failures[0].contains(QLatin1String("not saved"))
                   && b.keyer()->recordings()[0].durationMs == 0
                   && b.radio.transmitModel().voxEnable()
                   && b.voxRequests() == 1,   // the operator's own, and no restore
               (b.failures.join(" | ") + QStringLiteral(" wire=") + b.wire.join(",")).toStdString());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
