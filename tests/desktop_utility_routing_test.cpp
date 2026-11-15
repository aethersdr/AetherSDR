// Production routes and injected command sinks only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "TestEventLoop.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "core/AutomationServer.h"
#include "core/SpotRequestQueue.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/MixerControlAvailability.h"

#include <QApplication>
#include <QLocalServer>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <thread>

using namespace AetherSDR;

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject station(AutomationServer& server, const QString& name)
    { return server.doStation(name); }
    static void armStation(AutomationServer& server)
    {
        // An unbound server object permits stop() testing without an endpoint.
        server.m_server = new QLocalServer(&server);
        server.m_agentStation = QStringLiteral("Agent");
    }
    static void scheduleStation(AutomationServer& server)
    { server.onStationConnectionState(true); }
};
}

namespace {
int failures = 0;
void check(bool success, const char* message)
{
    std::printf("[%s] %s\n", success ? "PASS" : "FAIL", message);
    failures += !success;
}
SpotRequest addSpot(const QString& callsign = QStringLiteral("K1ABC"))
{
    return {SpotAction::Add, callsign, QStringLiteral("RBN"), QStringLiteral("W1AW"),
            QStringLiteral("CW 20 dB"), QStringLiteral("#FF00FF00"), 14.025, 120};
}
void utilityEncodingAndOwnership()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    PanadapterModel* pan = RadioSetupControlTestAccess::addPan(radio, QStringLiteral("0x40000000"));
    SliceModel* slice = RadioSetupControlTestAccess::addSlice(radio, 3);
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    check(!SpotRequest{}.valid() && !PanControlRequest{}.valid() && !SlicePresetRequest{}.valid(),
          "default-constructed utility requests fail closed");
    check(QMetaType::fromName("AetherSDR::SpotRequest").isValid()
              && QMetaType::fromName("AetherSDR::PanControlRequest").isValid()
              && QMetaType::fromName("AetherSDR::SlicePresetRequest").isValid(),
          "utility request metatypes are registered by the production model");
    check(radio.requestSpot(addSpot(QStringLiteral("K1ABC /P"))) == ReceiveDispatch::Dispatched,
          "spot addition traverses the production model and Flex writer");
    check(backend->commands == QStringList{QStringLiteral("spot add callsign=K1ABC") + QChar(0x7f)
              + QStringLiteral("/P rx_freq=14.025000 tx_freq=14.025000 source=RBN spotter_callsign=W1AW trigger_action=none lifetime_seconds=120 comment=CW")
              + QChar(0x7f) + QStringLiteral("20") + QChar(0x7f) + QStringLiteral("dB color=#FF00FF00")},
          "spot values are escaped exactly once without callsign-style comment sanitizing");
    backend->commands.clear();
    SpotRequest literal = addSpot(QStringLiteral("K1ABC%1"));
    literal.spotter = QStringLiteral("W1%5");
    check(radio.requestSpot(literal) == ReceiveDispatch::Dispatched
              && backend->commands.size() == 1
              && backend->commands.first().contains(QLatin1String("callsign=K1ABC%1 "))
              && backend->commands.first().contains(QLatin1String("spotter_callsign=W1%5 "))
              && backend->commands.first().contains(QLatin1String("lifetime_seconds=120")),
          "literal argument markers in spot text are never interpolated again");
    backend->commands.clear();
    SpotRequest bad = addSpot();
    bad.comment = QStringLiteral("comment\ntransmit tune 1");
    check(radio.requestSpot(bad) == ReceiveDispatch::Unsupported && backend->commands.isEmpty(),
          "a field cannot inject a second command");
    bad = addSpot(); bad.frequencyMhz = std::numeric_limits<double>::quiet_NaN();
    check(!bad.valid(), "non-finite spot frequencies are refused");
    bad = addSpot(); bad.comment = QString(QChar(0x7f));
    check(!bad.valid(), "already wire-escaped text is not accepted as a neutral value");
    bad = addSpot(); bad.color = QStringLiteral("#FF00GG00");
    check(!bad.valid(), "invalid color tokens are refused");

    radio.spotModel().applySpotStatus(7, {{"callsign", "K1ABC"}, {"source", "RBN"}});
    radio.spotModel().applySpotStatus(10000, {{"callsign", "TCI1"}, {"source", "tCi"}});
    radio.spotModel().applySpotStatus(-1000000, {{"callsign", "Memory"}, {"source", "Memory"}});
    radio.spotModel().applySpotStatus(-2000000, {{"callsign", "Local"}, {"source", "N1MM"}});
    SpotRequest trigger;
    trigger.action = SpotAction::Trigger; trigger.index = 7;
    slice->setLocked(true);
    check(radio.requestSpot(trigger, pan) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"spot trigger 7 pan=0x40000000"},
          "radio trigger retains firmware admission instead of TCI's local lock gate");
    backend->commands.clear();
    for (const int id : {10000, -1000000, -2000000}) {
        trigger.index = id;
        check(radio.requestSpot(trigger, pan) == ReceiveDispatch::Unsupported,
              "TCI, memory and passive namespaces never become radio triggers");
        trigger.action = SpotAction::Remove;
        check(radio.requestSpot(trigger) == ReceiveDispatch::Unsupported,
              "local namespaces never become radio removals");
        trigger.action = SpotAction::Trigger;
    }
    check(backend->commands.isEmpty(), "no invalid namespace reached the sink");
    trigger.action = SpotAction::Remove; trigger.index = 7;
    check(radio.requestSpot(trigger) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"spot remove 7"}, "radio-owned removal has exactly one route");
    backend->commands.clear();
    check(radio.requestPanControl(pan, {PanControlField::BandZoom, true}) == ReceiveDispatch::Dispatched
              && radio.requestPanControl(pan, {PanControlField::SegmentZoom, false}) == ReceiveDispatch::Dispatched
              && radio.requestPanControl(pan, {PanControlField::ReceiveAntenna, QStringLiteral("ANT2")}) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"display pan set 0x40000000 band_zoom=1",
                  "display pan set 0x40000000 segment_zoom=0", "display pan set 0x40000000 rxant=ANT2"},
          "all three pan utilities encode below the seam");
    check(!pan->bandZoomOn(), "pan command admission does not fabricate radio status");
    backend->commands.clear();
    RadioSetupControlTestAccess::addPan(radio, pan->panId());
    check(radio.requestPanControl(pan, {PanControlField::BandZoom, true}) == ReceiveDispatch::Unsupported
              && backend->commands.isEmpty(), "a retired pan cannot control a reused id");
    SlicePresetRequest preset;
    preset.stepHz = 50;
    check(radio.requestSlicePreset(slice, preset) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"slice set 3 step=50"}, "radio-owned tuning step dispatches exactly once");
    backend->commands.clear();
    MemoryEntry memory;
    memory.toneValue = 123.0;
    check(radio.requestSlicePreset(slice, SlicePresetRequest::repeaterFixup(memory)) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"slice set 3 fm_tone_value=123.0"},
          "partial net correction does not reset mode or repeater offset");
    backend->commands.clear();
    memory.offsetDir = QStringLiteral("DOWN"); memory.repeaterOffset = -0.6;
    memory.toneMode = QStringLiteral("OFF"); memory.toneValue = 0.0;
    check(radio.requestSlicePreset(slice, SlicePresetRequest::repeaterFixup(memory)) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{"slice set 3 repeater_offset_dir=down fm_repeater_offset_freq=0.600000 tx_offset_freq=-0.600000 fm_tone_mode=off fm_tone_value=0.0"},
          "grouped corrections preserve offset direction, magnitude and explicit tone reset");
    backend->commands.clear();
    RadioSetupControlTestAccess::addSlice(radio, 3);
    check(radio.requestSlicePreset(slice, preset) == ReceiveDispatch::Unsupported && backend->commands.isEmpty(),
          "a retired slice cannot edit its replacement");
    backend->accept = false;
    check(radio.requestSpot(addSpot()) == ReceiveDispatch::Unsupported, "writer refusal is not claimed as dispatched");
    backend->accept = true;
    const qsizetype refusals = dropped.size();
    std::thread wrongThread([&] { radio.requestSpot(addSpot()); });
    wrongThread.join();
    check(backend->commands.isEmpty() && dropped.size() == refusals,
          "wrong-thread input has neither dispatch nor a raw fallback");
    backend->live = false;
    check(radio.requestSpot(addSpot()) == ReceiveDispatch::Unsupported && backend->commands.isEmpty(),
          "disconnected utilities fail closed");
}

void queueFencing()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    SpotRequestQueue queue;
    const auto enqueue = [&](const QString& call) {
        queue.append(addSpot(call), radio.controlSession(), radio.spotRequestEpoch());
    };
    enqueue("FIRST"); enqueue("STALE"); enqueue("TAIL");
    const QList<QueuedSpotRequest> batch = queue.takeBatch(2, radio.controlSession(), radio.spotRequestEpoch());
    bool cleared = false;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        backend->commands.append(command);
        if (!cleared && command.startsWith(QLatin1String("spot add"))) {
            cleared = true;
            radio.requestSpot({SpotAction::Clear});
            enqueue("AFTER");
        }
        return true;
    });
    for (const QueuedSpotRequest& entry : batch) {
        radio.requestSpot(entry.request, nullptr, entry.session, entry.epoch);
    }
    check(backend->commands.size() == 2 && backend->commands.last() == QLatin1String("spot clear"),
          "reentrant Clear All retires the rest of the detached batch");
    const QList<QueuedSpotRequest> after = queue.takeBatch(10, radio.controlSession(), radio.spotRequestEpoch());
    check(after.size() == 1 && after.first().request.callsign == QLatin1String("AFTER"),
          "clear discards the old tail but preserves a newly enqueued spot");
    backend->commands.clear();
    for (const QueuedSpotRequest& entry : after) { radio.requestSpot(entry.request, nullptr, entry.session, entry.epoch); }
    check(backend->commands.size() == 1 && backend->commands.first().contains(QLatin1String("callsign=AFTER")),
          "post-clear arrival dispatches on the next batch");
    enqueue("OLDSESSION");
    RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    check(queue.takeBatch(10, radio.controlSession(), radio.spotRequestEpoch()).isEmpty(),
          "reconnect discards queued work from the original session");
    enqueue("NEWSESSION");
    check(queue.takeBatch(-1, radio.controlSession(), radio.spotRequestEpoch()).isEmpty() && queue.size() == 1,
          "negative rate limits cannot overrun or remove queued entries");
    const QList<QueuedSpotRequest> old = queue.takeBatch(1, radio.controlSession(), radio.spotRequestEpoch());
    RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    backend->commands.clear();
    radio.requestSpot(old.first().request, nullptr, old.first().session, old.first().epoch);
    check(backend->commands.isEmpty(), "an already detached old-session entry cannot dispatch");

    bool nested = false;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        backend->commands.append(command);
        if (!nested && command.startsWith(QLatin1String("spot add"))) {
            nested = true;
            radio.requestSpot({SpotAction::Clear});
            radio.queueSpot(addSpot(QStringLiteral("AFTER")));
            radio.flushQueuedSpots(10);
        }
        return true;
    });
    radio.queueSpot(addSpot(QStringLiteral("FIRST")));
    radio.queueSpot(addSpot(QStringLiteral("STALE")));
    radio.flushQueuedSpots(2);
    check(backend->commands.size() == 2 && backend->commands.last() == QLatin1String("spot clear"),
          "production flush fences the old batch and refuses recursive flush");
    backend->commands.clear();
    radio.flushQueuedSpots(10);
    check(backend->commands.size() == 1 && backend->commands.first().contains(QLatin1String("callsign=AFTER")),
          "production queue preserves post-clear arrivals for the next tick");
    radio.queueSpot(addSpot(QStringLiteral("OLD")));
    RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    backend->commands.clear();
    radio.flushQueuedSpots(10);
    check(backend->commands.isEmpty(), "production queue drops entries on connection replacement");
}

void stationOwnership()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    AppSettings::instance().setValue("StationName", QStringLiteral("Operator Desk"));
    const auto owner = radio.requestTemporaryStation(QStringLiteral("Agent"));
    check(owner && AppSettings::instance().value("StationName").toString() == QLatin1String("Operator Desk"),
          "temporary station identity never persists client settings");
    check(owner && radio.restoreTemporaryStation(*owner)
              && backend->commands.last() == QStringLiteral("client station Operator") + QChar(0x7f) + QStringLiteral("Desk"),
          "restoration uses the saved station and the backend encoder");
    const auto first = radio.requestTemporaryStation(QStringLiteral("First"));
    const auto second = radio.requestTemporaryStation(QStringLiteral("Second"));
    const qsizetype before = backend->commands.size();
    check(first && second && !radio.restoreTemporaryStation(*first) && backend->commands.size() == before,
          "an old owner cannot restore over a newer override");
    radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("New Operator")});
    check(second && !radio.restoreTemporaryStation(*second), "a successful operator edit supersedes the bridge override");
    const auto retained = radio.requestTemporaryStation(QStringLiteral("Agent"));
    backend->accept = false;
    check(!radio.requestTemporaryStation(QStringLiteral("Rejected")), "a refused write creates no temporary owner");
    radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("Latest Saved")});
    backend->accept = true;
    check(retained && radio.restoreTemporaryStation(*retained)
              && backend->commands.last() == QStringLiteral("client station Latest") + QChar(0x7f) + QStringLiteral("Saved"),
          "failed operator radio copy remains saved and an owned restore uses that latest intent");
    const auto stale = radio.requestTemporaryStation(QStringLiteral("Agent"));
    RadioModel origin;
    installRadioSetupBackend(origin);
    const auto originalOwner = origin.requestTemporaryStation(QStringLiteral("OriginAgent"));
    RadioModel other;
    RadioSetupTestBackend* otherBackend = installRadioSetupBackend(other);
    const auto otherOwner = other.requestTemporaryStation(QStringLiteral("OtherAgent"));
    otherBackend->commands.clear();
    check(originalOwner && otherOwner && originalOwner->session == otherOwner->session
              && originalOwner->revision == otherOwner->revision
              && !other.restoreTemporaryStation(*originalOwner) && otherBackend->commands.isEmpty(),
          "station tokens cannot cross model objects even when session counters coincide");
    RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    backend->commands.clear();
    check(stale && !radio.restoreTemporaryStation(*stale) && backend->commands.isEmpty(),
          "restoration never crosses reconnect");
    bool nested = false;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        backend->commands.append(command);
        if (!nested) {
            nested = true;
            radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("Newest")});
        }
        return true;
    });
    check(!radio.requestTemporaryStation(QStringLiteral("Superseded"))
              && AppSettings::instance().value("StationName").toString() == QLatin1String("Newest"),
          "reentrant operator edits supersede override admission");
    check(!radio.requestTemporaryStation(QStringLiteral("!!!")), "an identity erased by the SDK sanitizer is refused");
}

void savedStationIntent()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    AppSettings::instance().setValue("StationName", QStringLiteral("Before"));
    std::optional<RadioStationOverride> agent;
    bool nested = false;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        backend->commands.append(command);
        if (!nested) {
            nested = true;
            agent = radio.requestTemporaryStation(QStringLiteral("AgentDuringEdit"));
        }
        return true;
    });
    radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("Operator Latest")});
    check(agent && AppSettings::instance().value("StationName").toString() == QLatin1String("Operator Latest"),
          "a temporary override cannot discard a pending saved station edit");
    backend->commands.clear();
    check(agent && radio.restoreTemporaryStation(*agent)
              && backend->commands == QStringList{QStringLiteral("client station Operator")
                  + QChar(0x7f) + QStringLiteral("Latest")},
          "override cleanup restores the operator edit made around its admission");

    nested = false;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        backend->commands.append(command);
        if (!nested) {
            nested = true;
            radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("Newest Operator")});
        }
        return true;
    });
    radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("Older Operator")});
    check(AppSettings::instance().value("StationName").toString() == QLatin1String("Newest Operator"),
          "a reentrant operator edit remains the newest saved station intent");
}

class NativeLineoutBackend final : public SimBackend {
public:
    bool isConnected() const override { return true; }
    int gain{-1}; int muteWrites{0};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps = SimBackend::capabilities();
        caps.lineoutControl = RadioCapabilities::LineoutControl{};
        caps.radioSetupControl = RadioSetupControl{{RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
        return caps;
    }
    void setLineoutGain(int value) override { gain = value; }
    void setLineoutMute(bool) override { ++muteWrites; }
};

void physicalOutputs()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    radio.setLineoutGain(37); radio.setHeadphoneGain(38);
    radio.setLineoutMute(true); radio.setLineoutMute(true);
    radio.setHeadphoneMute(true); radio.setHeadphoneMute(true);
    radio.setFrontSpeakerMute(true); radio.setFrontSpeakerMute(true);
    check(backend->commands == QStringList{"mixer lineout gain 37", "mixer headphone gain 38",
              "mixer lineout mute 1", "mixer lineout mute 1", "mixer headphone mute 1",
              "mixer headphone mute 1", "mixer front_speaker mute 1", "mixer front_speaker mute 1"},
          "output setters keep optimistic state and repeated mute reapplication");
    backend->commands.clear();
    radio.requestRadioSetup({RadioSetupField::LineoutGain, 37});
    radio.requestRadioSetup({RadioSetupField::LineoutGain, 37});
    check(backend->commands.size() == 2 && radio.lineoutGain() == 37,
          "controller dispatch is not suppressed by the model setter's same-value cache");
    radio.requestRadioSetup({RadioSetupField::LineoutGain, 62});
    check(radio.lineoutGain() == 37, "controller gain requests leave radio readback authoritative");
    bool nested = false;
    backend->setSetupCommandSink([&](const QString&, const RadioSetupCompletion&) {
        if (!nested) { nested = true; radio.setLineoutMute(false); }
        return true;
    });
    radio.setLineoutMute(true);
    check(!radio.lineoutMute(), "a superseded mute has no stale optimistic tail");

    RadioModel native;
    auto owned = std::make_unique<NativeLineoutBackend>();
    NativeLineoutBackend* output = owned.get();
    native.setBackendForTest(std::move(owned), QStringLiteral("sim"));
    native.setLineoutGain(33); native.setLineoutMute(true); native.setLineoutMute(true);
    check(output->gain == 33 && output->muteWrites == 2, "native lineout remains on its implemented seam verbs");
    check(native.requestRadioSetup({RadioSetupField::HeadphoneGain, 50}) == ReceiveDispatch::Unsupported,
          "native lineout does not invent headphone support");
    SliceModel* slice = RadioSetupControlTestAccess::addSlice(native, 1);
    SlicePresetRequest step; step.stepHz = 250;
    check(native.requestSlicePreset(slice, step) == ReceiveDispatch::LocalOnly && slice->stepHz() == 250,
          "non-command-plane tuning step retains its client-owned consumer");
    step.stepHz = 500; step.toneHz = 88.5;
    check(native.requestSlicePreset(slice, step) == ReceiveDispatch::Unsupported && slice->stepHz() == 250,
          "an unsupported grouped correction does not partially mutate the local tuning step");
    check(!masterKnobDrivesLocalOutput(true, false, false)
              && masterKnobDrivesLocalOutput(true, false, true)
              && !masterKnobDrivesLocalOutput(true, true, true)
              && !masterKnobDrivesLocalOutput(false, false, true),
          "MIDI master routing stays distinct from native lineout availability");
}

void bridgeStationLifetime()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    AutomationServer server;
    server.setRadioModel(&radio);
    AutomationServerTestAccess::armStation(server);
    AppSettings::instance().setValue("StationName", QStringLiteral("Operator"));
    check(AutomationServerTestAccess::station(server, QStringLiteral("Agent")).value("ok").toBool(),
          "bridge reports station success only after actual dispatch");
    AutomationServerTestAccess::scheduleStation(server);
    server.stop();
    backend->commands.clear();
    AetherTest::pumpFor(1100);
    check(backend->commands.isEmpty(), "queued reconnect reapplication cannot run after bridge stop");
    AutomationServerTestAccess::armStation(server);
    AutomationServerTestAccess::scheduleStation(server);
    radio.requestRadioSetup({RadioSetupField::ClientStation, QStringLiteral("User Wins")});
    backend->commands.clear();
    AetherTest::pumpFor(1100);
    check(backend->commands.isEmpty(), "a pending reconnect callback cannot overwrite a newer user station edit");
    backend->accept = false;
    check(!AutomationServerTestAccess::station(server, QStringLiteral("Rejected")).value("ok").toBool(),
          "bridge does not report success when station dispatch fails");

    backend->accept = true;
    check(AutomationServerTestAccess::station(server, QStringLiteral("BeforeRebind")).value("ok").toBool(),
          "bridge can acquire a fresh override after a refused request");
    RadioModel replacement;
    RadioSetupTestBackend* replacementBackend = installRadioSetupBackend(replacement);
    backend->commands.clear();
    server.setRadioModel(&replacement);
    check(backend->commands == QStringList{QStringLiteral("client station User") + QChar(0x7f) + QStringLiteral("Wins")}
              && replacementBackend->commands.isEmpty(),
          "rebinding restores the owned original model, never the replacement");
    server.stop();
    AetherTest::pumpFor(1100);
    check(replacementBackend->commands.isEmpty(), "bridge stop cancels a station application scheduled by rebind");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("desktop-utility-routing"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    utilityEncodingAndOwnership();
    queueFencing();
    stationOwnership();
    savedStationIntent();
    physicalOutputs();
    bridgeStationLifetime();
    return failures ? 1 : 0;
}
