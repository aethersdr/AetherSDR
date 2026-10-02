#pragma once

// Registry of external playback sinks that must follow the user-selected output
// device (ClientPuduMonitor, QsoRecorder, ...): addFollower() seeds a sink with
// the current device and re-seeds it on every change (#3306; see
// docs/audio-sink-factory.md). Not routed here: AudioEngine-internal sinks (RX,
// CW sidetone, Quindar), restarted in setOutputDevice()/startRxStream(); and the
// WFM WaveOutWriter, which plays to its own WfmDeviceDialog device by design.
// setCurrentDevice() fans out synchronously on the caller's thread; the caller
// bridges AudioEngine::outputDeviceChanged (audio thread) with a QueuedConnection
// so followers are touched on the GUI thread. Qt Core/Multimedia only.

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
        registerFollower(
            [guarded](const QAudioDevice& dev) {
                if (guarded)
                    guarded->setOutputDevice(dev);
            },
            [guarded]() { return !guarded.isNull(); });
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
    // A registered follower plus an optional liveness predicate. `alive` is set
    // only by the QPointer-guarded template overload; a raw std::function
    // follower carries no liveness info (null `alive` == always alive). Dead
    // followers (guard gone null) are pruned after each fan-out (#3660).
    struct Follower {
        std::function<void(const QAudioDevice&)> apply;
        std::function<bool()>                    alive;
    };

    // Shared registration path for both addFollower() overloads: seed + store.
    void registerFollower(std::function<void(const QAudioDevice&)> apply,
                          std::function<bool()> alive);

    QAudioDevice          m_device;
    std::vector<Follower> m_followers;
};

} // namespace AetherSDR
