#pragma once

#include <QAudioDevice>
#include <QObject>
#include <QPointer>

class QAudioSink;
class QIODevice;
class QTimer;

namespace AetherSDR {

class ClientQuindarTone;

// Dedicated local sink for Quindar tones (#2262), separate from the CW
// sidetone path (the modes are mutually exclusive). Started with the RX stream
// so it is primed at MOX; pushed every 10 ms by a QTimer like
// CwSidetoneQAudioSink. Invariant: every Quindar sample overlaid on TX also
// plays locally — the operator never transmits a sound they can't hear.
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

} // namespace AetherSDR
