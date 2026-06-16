#pragma once

// ─── AudioOutputRouter ───────────────────────────────────────────────────────
//
// One registry for "playback sinks that must follow the user-selected output
// device." Historically each such sink was wired by hand in MainWindow with its
// own connect-to-outputDeviceChanged lambda, and every NEW sink had to remember
// to add itself — the recurring "uncoupling" where a freshly-added sink kept
// playing on the old/default endpoint after the user changed devices
// (CW sidetone #2899, Aetherial/Pudu monitor + QSO playback #3361/#3378).
//
// This turns that into a single point: register a sink once with addFollower();
// it is seeded immediately with the current device and re-seeded on every
// change. Adding a future sink is one self-documenting line, so the class of
// bug can't silently reappear. Part of the audio sink factory (issue #3306);
// see docs/audio-sink-factory.md.
//
// Scope: this manages EXTERNAL playback sinks that own their own QAudioSink and
// are meant to play to the user's main selected output (ClientPuduMonitor,
// QsoRecorder, …). Deliberately NOT routed here:
//   * AudioEngine-internal sinks (RX speaker, CW sidetone, Quindar) — they
//     already follow the selection by being restarted inside
//     setOutputDevice()/startRxStream().
//   * The WFM demodulator's WaveOutWriter — it plays to its OWN device chosen
//     separately in WfmDeviceDialog, by design, not the main output. Do not
//     register it; following the main selection would be a regression.
//
// Threading: setCurrentDevice() runs the fan-out synchronously on the caller's
// thread. The caller bridges AudioEngine::outputDeviceChanged (emitted on the
// audio worker thread) with a QueuedConnection so followers are touched on the
// GUI thread, matching the previous hand-wired behaviour.
//
// Depends only on Qt Core/Multimedia so the registry/fan-out logic is unit-
// testable without instantiating the full AudioEngine.

#include <QAudioDevice>
#include <QObject>
#include <QPointer>

#include <functional>
#include <vector>

namespace AetherSDR {

class AudioOutputRouter : public QObject {
    Q_OBJECT

public:
    explicit AudioOutputRouter(QObject* parent = nullptr);

    // Register a sink that must play to the selected output device. The apply
    // callback is invoked immediately with the current device and again on every
    // subsequent setCurrentDevice().
    void addFollower(std::function<void(const QAudioDevice&)> apply);

    // Convenience for the common case: any object exposing
    // setOutputDevice(const QAudioDevice&). Guarded by QPointer so a follower
    // destroyed before the router is harmless.
    template <typename T>
    void addFollower(T* sink)
    {
        QPointer<T> guarded(sink);
        addFollower([guarded](const QAudioDevice& dev) {
            if (guarded)
                guarded->setOutputDevice(dev);
        });
    }

    // The device followers are currently bound to (may be null == "system
    // default", which followers resolve themselves).
    QAudioDevice currentDevice() const { return m_device; }

    int followerCount() const { return static_cast<int>(m_followers.size()); }

public slots:
    // Update the selected device and re-push to every registered follower.
    // Connect AudioEngine::outputDeviceChanged to a forwarder that calls this.
    void setCurrentDevice(const QAudioDevice& dev);

private:
    QAudioDevice m_device;
    std::vector<std::function<void(const QAudioDevice&)>> m_followers;
};

} // namespace AetherSDR
