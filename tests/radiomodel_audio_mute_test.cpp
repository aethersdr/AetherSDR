// RadioModel's audio-mute setters must mirror the command into the model (#4771).
//
// Before this test, setLineoutMute() / setHeadphoneMute() / setFrontSpeakerMute()
// sent the command and returned -- they never assigned, never emitted. Their
// neighbours setLineoutGain() / setHeadphoneGain() did both. The asymmetry was
// invisible on a Flex, whose `audio` status echo writes the flags a beat later,
// and permanent everywhere else: FlexBackend is the ONLY parser of lineout_mute
// / headphone_mute / front_speaker_mute, so on hl2 or sim the flags sat at their
// `false` initialisers for the whole session.
//
// What that cost, once #4733 made the title bar reconcile from them: mute the
// headphones, nudge the headphone volume slider, and setHeadphoneGain()'s
// audioOutputChanged drove the glyph back to unmuted from the stale flag --
// under a QSignalBlocker, so no command was sent to undo it and nothing was
// logged. The control and the radio disagreed silently.
//
// The assertions below are the contract, not the implementation: a mute request
// always reaches the wire, the model mirrors it, the mirror is idempotent, and
// -- the Principle II half -- a radio status still supersedes the optimistic
// value. That last one is why this is safe to do at all.

#include "models/RadioModel.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/sim/SimBackend.h"
#include "core/RadioDiscovery.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include <QHostAddress>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The demo entry exactly as ConnectionPanel::addDemoRadio() builds it.
static RadioInfo demoInfo()
{
    RadioInfo i;
    i.name    = QStringLiteral("FLEX-6700");
    i.model   = SimBackend::demoModelName();
    i.serial  = SimBackend::demoSerial();
    i.family  = SimBackend::familyName();
    i.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    i.port    = 4992;
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    RadioModel model;
    QSignalSpy audio(&model, &RadioModel::audioOutputChanged);

    // --- Headphone -----------------------------------------------------------
    check(!model.headphoneMute(), "precondition: headphone mute starts false");

    model.setHeadphoneMute(true);
    check(model.headphoneMute(),
          "setHeadphoneMute(true) mirrors into the model");
    check(audio.count() == 1,
          "setHeadphoneMute(true) raises audioOutputChanged once");

    // Idempotence: Flex re-sends `audio` on every mixer change, and the UI
    // reconciles on each one. A no-op setter call must not add churn.
    model.setHeadphoneMute(true);
    check(model.headphoneMute() && audio.count() == 1,
          "repeating setHeadphoneMute(true) does not re-emit");

    model.setHeadphoneMute(false);
    check(!model.headphoneMute() && audio.count() == 2,
          "setHeadphoneMute(false) mirrors and emits");

    // --- Line out ------------------------------------------------------------
    audio.clear();
    model.setLineoutMute(true);
    check(model.lineoutMute(), "setLineoutMute(true) mirrors into the model");
    check(audio.count() == 1, "setLineoutMute(true) raises audioOutputChanged once");

    model.setLineoutMute(true);
    check(audio.count() == 1, "repeating setLineoutMute(true) does not re-emit");

    // --- Front speaker -------------------------------------------------------
    audio.clear();
    model.setFrontSpeakerMute(true);
    check(model.frontSpeakerMute(),
          "setFrontSpeakerMute(true) mirrors into the model");
    check(audio.count() == 1,
          "setFrontSpeakerMute(true) raises audioOutputChanged once");

    // --- The reported failure, end to end ------------------------------------
    // This is the exact sequence from #4771: mute, then move the volume. The
    // gain setter raises audioOutputChanged, which is what the title bar
    // reconciles the glyph from -- so the mute flag it reads there must still
    // say muted.
    model.setHeadphoneMute(true);
    model.setHeadphoneGain(60);
    check(model.headphoneMute(),
          "a headphone volume change does not clear the mute the operator set");
    check(model.headphoneGain() == 60,
          "the volume change itself still lands");

    // --- Principle II: the radio still wins ----------------------------------
    // The optimistic mirror is only defensible because a status echo overrides
    // it. Feed a radio-authoritative delta through the backend seam and confirm
    // it beats the value the setter just wrote.
    if (IRadioBackend* backend = model.backend()) {
        model.setHeadphoneMute(true);
        check(model.headphoneMute(), "precondition: optimistic value is set");

        RadioDelta d;
        d.headphoneMute = false;      // the radio says otherwise
        emit backend->radioChanged(d);

        check(!model.headphoneMute(),
              "a radio status supersedes the optimistic mute (Principle II)");
    } else {
        std::fprintf(stderr,
                     "NOTE: no backend on a bare RadioModel; supersede leg not "
                     "exercised here (applyRadioChanges assigns unconditionally "
                     "at RadioModel.cpp:9139)\n");
    }

    // --- Disconnect clears the mutes -----------------------------------------
    // onDisconnected() resets the audio GAINS to 50 so a reconnect cannot render
    // the previous radio's state while the new radio's status is in flight. The
    // mutes were missed by that block, so they survived a disconnect holding the
    // old rig's values (#4771 item 2).
    // Reached through a real lifecycle: onDisconnected() only runs for a model
    // that actually connected, so drive the in-process demo backend rather than
    // calling disconnectFromRadio() on a model that never connected.
    model.connectToRadio(demoInfo());
    for (int i = 0; i < 40 && !model.isConnected(); ++i)
        spin(50);
    check(model.isConnected(), "precondition: connected to the demo backend");

    model.setHeadphoneMute(true);
    model.setLineoutMute(true);
    model.setFrontSpeakerMute(true);
    model.setHeadphoneGain(70);
    check(model.headphoneMute() && model.lineoutMute() && model.frontSpeakerMute(),
          "precondition: all three mutes set before disconnect");

    model.disconnectFromRadio();
    // isConnected() goes false BEFORE onDisconnected() runs its state reset, so
    // waiting on the connection flag alone races the thing under test. Wait for
    // the reset itself to land, bounded at 2 s.
    for (int i = 0; i < 40 && model.headphoneGain() != 50; ++i)
        spin(50);
    check(!model.isConnected(), "precondition: model reports disconnected");
    check(!model.headphoneMute(), "disconnect clears headphone mute");
    check(!model.lineoutMute(),   "disconnect clears line out mute");
    check(!model.frontSpeakerMute(), "disconnect clears front speaker mute");
    check(model.headphoneGain() == 50,
          "disconnect still resets the gains it always did");

    if (g_failures == 0)
        std::fprintf(stderr, "radiomodel_audio_mute_test: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
