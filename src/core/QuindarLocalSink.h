#pragma once

#include <QAudioDevice>
#include <QObject>
#include <QPointer>

class QAudioSink;
class QIODevice;
class QTimer;

namespace MasterSDR {

class ClientQuindarTone;

// Dedicated local audio sink for Quindar tones (#2262).  Independent
// of the CW sidetone path because Quindar and CW are mutually exclusive
// modes — mixing them would conflate two unrelated systems.
//
// Lifecycle: started alongside the RX stream so the sink is always
// primed when the operator hits MOX.  Pulls samples every 10 ms via a
// push-mode QTimer (matches the CwSidetoneQAudioSink pattern that
// keeps Pulse/PipeWire pull-mode happy with a moderate buffer).
//
// The hard rule: every sample the Quindar tone overlays into the TX
// stream MUST also play through the local output.  Users must never
// transmit a sound they aren't hearing themselves.
class QuindarLocalSink : public QObject {
    Q_OBJECT

public:
    explicit QuindarLocalSink(QObject* parent = nullptr);
    ~QuindarLocalSink() override;

    // Open the audio device, prime the sink, start the push-mode
    // timer.  Idempotent — safe to call when already running.
    bool start(const QAudioDevice& device, ClientQuindarTone* tone);

    // Close the device and release library resources.
    void stop();

    bool isRunning() const { return m_sink != nullptr; }
    int  actualRateHz() const { return m_actualRate; }

private:
    void onTimerTick();

    QAudioSink*          m_sink{nullptr};
    QPointer<QIODevice>  m_device;
    QTimer*              m_timer{nullptr};
    ClientQuindarTone*   m_tone{nullptr};
    int                  m_actualRate{0};
};

} // namespace MasterSDR
