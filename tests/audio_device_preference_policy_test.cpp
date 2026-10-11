// AudioDevicePreferencePolicy: a saved PC audio device that is absent stays
// saved, and is used again when it returns (#6040).
//
// The policy is a pure function over device ids, so every id below is a made-up
// string. No QtMultimedia, no audio device, no settings store, no socket.
//
// Session models one direction the way the app runs it: `saved` stands for the
// AudioOutputDeviceId / AudioInputDeviceId row, `live` for the engine's
// device, and every write to `saved` goes through
// persistsAudioDeviceSelection(), the same gate AudioEngine's setters use.

#include "core/AudioDevicePreferencePolicy.h"

#include <QByteArray>
#include <QList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const char* what)
{
    ++g_checks;
    if (ok) {
        std::printf("[ OK ] %s\n", what);
    } else {
        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }
}

const QByteArray kBuiltIn = "built-in";
const QByteArray kHeadphones = "bt-headphones";
const QByteArray kUsb = "usb-codec";

struct Session {
    QByteArray saved;
    QByteArray live;
    QList<QByteArray> known;
    AudioDeviceDecision last;

    // App launch: the saved device becomes live only if it is enumerable.
    static Session launch(const QByteArray& savedId, const QList<QByteArray>& present)
    {
        Session s;
        s.saved = savedId;
        s.known = present;
        if (present.contains(savedId)) {
            s.live = savedId;
        }
        return s;
    }

    void setLive(const QByteArray& id, AudioDeviceSelection selection)
    {
        live = id;
        if (persistsAudioDeviceSelection(selection)) {
            saved = id;
        }
    }

    // The operator accepts the hot-plug prompt with `selectedId` in this
    // direction's list (empty = "System default").
    void acceptPrompt(const QByteArray& selectedId)
    {
        setLive(selectedId, acceptedAudioDeviceSelection(selectedId, saved, known));
    }

    // One debounced device-list change, handled without the prompt.
    void devicesChange(const QList<QByteArray>& present, bool defaultChanged)
    {
        AudioDeviceListChange change;
        change.presentIds = present;
        for (const QByteArray& id : present) {
            if (!known.contains(id)) {
                change.addedIds.append(id);
            }
        }
        for (const QByteArray& id : known) {
            if (!present.contains(id)) {
                change.anyRemoved = true;
            }
        }
        change.defaultChanged = defaultChanged;
        change.liveId = live;
        change.preferredId = saved;
        known = present;

        last = decideAudioDeviceChange(change);
        switch (last.action) {
        case AudioDeviceAction::Keep:
            break;
        case AudioDeviceAction::FollowDefault:
            setLive(QByteArray{}, AudioDeviceSelection::Fallback);
            break;
        case AudioDeviceAction::RestorePreferred:
            setLive(saved, AudioDeviceSelection::Fallback);
            break;
        }
    }
};

} // namespace

static_assert(persistsAudioDeviceSelection(AudioDeviceSelection::OperatorChoice),
              "an operator's choice is the saved preference");
static_assert(!persistsAudioDeviceSelection(AudioDeviceSelection::Fallback),
              "a fallback never rewrites the saved preference (#6040)");

int main()
{
    // ── Missing at launch, then connected: the issue's reproduction ─────────
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn});
        expect(s.live.isEmpty(), "launch without the saved device runs on the system default");
        expect(s.saved == kHeadphones, "launch without the saved device leaves the saved id");

        // The headphones connect and the OS makes them the default.
        s.devicesChange({kBuiltIn, kHeadphones}, /*defaultChanged*/ true);
        expect(s.last.action == AudioDeviceAction::RestorePreferred,
               "saved device arrives while on the default -> restore it");
        expect(s.live == kHeadphones, "the returning saved device is live again");
        expect(s.saved == kHeadphones, "the saved id survives the device returning");
    }

    // ── Missing at launch, then an unrelated device change ──────────────────
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn, kUsb});
        s.devicesChange({kBuiltIn}, /*defaultChanged*/ false);
        expect(s.last.action == AudioDeviceAction::FollowDefault,
               "a removal while on the default restarts on the default");
        expect(s.saved == kHeadphones, "an unrelated removal leaves the saved id");

        s.devicesChange({kBuiltIn, kUsb}, /*defaultChanged*/ true);
        expect(s.last.action == AudioDeviceAction::FollowDefault,
               "a default change while on the default restarts on the default");
        expect(s.live.isEmpty(), "an unrelated arrival is not adopted");
        expect(s.saved == kHeadphones, "an unrelated arrival leaves the saved id");

        s.devicesChange({kBuiltIn, kUsb, kHeadphones}, /*defaultChanged*/ false);
        expect(s.live == kHeadphones && s.saved == kHeadphones,
               "the saved device is restored after earlier unrelated changes");
    }

    // ── Present, then removed, then back: a Bluetooth dropout ───────────────
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn, kHeadphones});
        expect(s.live == kHeadphones, "launch with the saved device present uses it");

        s.devicesChange({kBuiltIn}, /*defaultChanged*/ true);
        expect(s.last.liveMissing, "the live device vanishing is reported as missing");
        expect(s.last.action == AudioDeviceAction::FollowDefault,
               "the live device vanishing falls back to the default");
        expect(s.live.isEmpty(), "after the dropout the run follows the default");
        expect(s.saved == kHeadphones, "the dropout leaves the saved id");

        s.devicesChange({kBuiltIn, kHeadphones}, /*defaultChanged*/ true);
        expect(s.live == kHeadphones && s.saved == kHeadphones,
               "the saved device is restored after the dropout");
    }

    // ── A live device the hot-plug monitor ignores as churn (Linux) ────────
    // presentIds is every enumerable id; addedIds leaves the churn out. The
    // caller that filtered presentIds too would report this device missing.
    {
        const QByteArray kEchoCancel("echo-cancel-source");
        const AudioDeviceDecision d = decideAudioDeviceChange({
            .presentIds = {kBuiltIn, kEchoCancel, kUsb},
            .addedIds = {kUsb},
            .anyRemoved = false,
            .defaultChanged = false,
            .liveId = kEchoCancel,
            .preferredId = kEchoCancel});
        expect(d.action == AudioDeviceAction::Keep && !d.liveMissing,
               "an enumerable live device is present, whatever counts as an arrival");
        expect(!audioDeviceChoiceRequired(true, d, d),
               "and an unrelated arrival does not ask the operator to choose");
        expect(acceptedAudioDeviceSelection({}, kEchoCancel, {kBuiltIn, kEchoCancel, kUsb})
                   == AudioDeviceSelection::OperatorChoice,
               "System default accepted beside a present saved device is a choice");
    }

    // ── Removed while a different device is selected ────────────────────────
    {
        Session s = Session::launch(kUsb, {kBuiltIn, kUsb, kHeadphones});
        s.devicesChange({kBuiltIn, kUsb}, /*defaultChanged*/ true);
        expect(s.last.action == AudioDeviceAction::Keep && !s.last.liveMissing,
               "another device leaving does not touch a present live device");
        expect(s.live == kUsb && s.saved == kUsb, "live and saved stay on the selected device");

        s.devicesChange({kBuiltIn, kUsb, kHeadphones}, /*defaultChanged*/ true);
        expect(s.last.action == AudioDeviceAction::Keep,
               "another device arriving does not touch a present live device (#2864)");
    }

    // ── Explicit operator choice ────────────────────────────────────────────
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn, kHeadphones, kUsb});
        s.setLive(kUsb, AudioDeviceSelection::OperatorChoice);
        expect(s.saved == kUsb, "an operator's choice replaces the saved id");

        // Choosing the system default is a choice too: nothing to restore later.
        s.setLive(QByteArray{}, AudioDeviceSelection::OperatorChoice);
        expect(s.saved.isEmpty(), "choosing the system default clears the saved id");
        s.devicesChange({kBuiltIn, kUsb}, /*defaultChanged*/ false);
        s.devicesChange({kBuiltIn, kUsb, kHeadphones}, /*defaultChanged*/ true);
        expect(s.last.action == AudioDeviceAction::FollowDefault && s.live.isEmpty(),
               "with nothing saved, an arriving device is not adopted");
    }

    // ── When the operator is asked ──────────────────────────────────────────
    {
        AudioDeviceDecision fine;
        AudioDeviceDecision missing;
        missing.liveMissing = true;
        missing.action = AudioDeviceAction::FollowDefault;
        AudioDeviceDecision restoring;
        restoring.action = AudioDeviceAction::RestorePreferred;

        expect(!audioDeviceChoiceRequired(true, fine, fine),
               "an arrival with both live devices present does not prompt (#2864)");
        expect(!audioDeviceChoiceRequired(true, restoring, restoring),
               "the saved device returning does not prompt");
        expect(!audioDeviceChoiceRequired(false, missing, fine),
               "a live device vanishing with nothing new does not prompt");
        expect(audioDeviceChoiceRequired(true, missing, fine),
               "a live input vanishing as another device arrives prompts");
        expect(audioDeviceChoiceRequired(true, fine, missing),
               "a live output vanishing as another device arrives prompts");
    }

    // ── Accepting the prompt beside an absent saved device ──────────────────
    {
        // The saved output is unplugged as a new input arrives: the prompt
        // opens, and its output list has only "System default" to show.
        Session output = Session::launch(kHeadphones, {kBuiltIn, kHeadphones});
        Session input = Session::launch(kBuiltIn, {kBuiltIn});
        output.devicesChange({kBuiltIn}, /*defaultChanged*/ true);
        input.devicesChange({kBuiltIn, kUsb}, /*defaultChanged*/ false);
        expect(audioDeviceChoiceRequired(true, input.last, output.last),
               "saved output unplugged as a new input arrives prompts");

        input.acceptPrompt(kUsb);
        output.acceptPrompt(QByteArray{});
        expect(output.saved == kHeadphones && output.live.isEmpty(),
               "accepting for the input keeps the absent output's saved id");
        expect(input.saved == kUsb && input.live == kUsb,
               "the input picked in the prompt is saved");
    }
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn, kHeadphones});
        s.acceptPrompt(QByteArray{});
        expect(s.saved.isEmpty() && s.live.isEmpty(),
               "System default accepted with the saved device present clears the saved id");
    }
    {
        expect(acceptedAudioDeviceSelection(QByteArray{}, QByteArray{}, {kBuiltIn})
                   == AudioDeviceSelection::OperatorChoice,
               "System default accepted with nothing saved is the operator's choice");
    }
    {
        Session s = Session::launch(kHeadphones, {kBuiltIn, kUsb});
        s.acceptPrompt(kUsb);
        expect(s.saved == kUsb && s.live == kUsb,
               "a device picked while the saved one is absent replaces the saved id");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
