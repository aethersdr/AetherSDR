#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// What GeneratedAudioTransmitter needs from a radio to put pre-made audio on
// the air. The per-radio differences live behind this interface as ANSWERS,
// never as a family test in the sequencer:
//
//   Flex  needs a dax_tx stream first; the command edge is the keyed edge.
//   HL2   no stream (host modulates, audio goes over the seam); command edge.
//   Icom  no stream (audio over the seam); audio waits for the radio's own
//         PTT readback before sample zero.
//
// This is the sequence Ax25HfPacketDecodeDialog runs (#4812), shared rather
// than reimplemented per generated-audio source.
class TxAudioRoute {
public:
    virtual ~TxAudioRoute() = default;

    // Non-empty: why a transmission may not start now (disconnected, not a
    // voice mode, the radio already transmitting, another generated-audio
    // source owning the path). Checked once at start; the owner aborts on
    // later changes.
    virtual QString startRefusal() const = 0;

    // A radio-side stream must exist before audio can be sent (a Flex
    // dax_tx stream). requestStream() returns false when refused outright;
    // readiness is reported through GeneratedAudioTransmitter::onStreamReady.
    virtual bool needsStream() const = 0;
    virtual bool streamReady() const = 0;
    virtual bool requestStream() = 0;

    // Take transmit audio from the microphone for the duration, remembering
    // what to hand back; releaseAudioPath restores exactly that.
    virtual void claimAudioPath() = 0;
    virtual void releaseAudioPath() = 0;

    // Admit the operation: capture the transmit authority this transmission
    // will carry. Called synchronously at the input boundary, before any
    // queued hop, which is where TxCoordinator requires a request to be taken.
    // False when nothing was admitted, and the transmission never starts.
    virtual bool admitOperation() = 0;

    // False when the key could not even be REQUESTED — no transmit authority
    // admitted, so nothing this route sends would be licensed to reach the
    // transport. The transmitter fails closed on it rather than keying a radio
    // it cannot feed: keyed-and-silent is the one outcome worth refusing
    // outright, because the operator hears nothing wrong.
    virtual bool keyOn() = 0;
    virtual void keyOff() = 0;
    virtual bool isKeyed() const = 0;

    // Sample zero waits for the radio to confirm it is keyed.
    virtual bool waitsForRadioPtt() const = 0;
    virtual bool isRadioKeyed() const = 0;

    // 24 kHz stereo float32. Non-blocking: the route queues it.
    virtual void sendAudio(const QByteArray& float32Stereo24k) = 0;
    // Queue an ordered end-of-audio barrier behind every sendAudio block;
    // the result arrives through GeneratedAudioTransmitter::onAudioFinished.
    virtual bool finishAudio(quint64 token) = 0;
    // Drop partially packetised audio so the next burst starts clean.
    virtual void clearAudio() = 0;
};

// Timings, in ms. Defaults are AetherModem's, proven on Flex, HL2 and Icom.
struct GeneratedAudioTiming {
    int streamWaitMs{5000};    // dax_tx create reply
    int settleMs{150};         // transmit-audio source switch before PTT
    int pttConfirmMs{2000};    // radio PTT readback (Icom)
    int leadMs{200};           // keyed, before sample zero
    int chunkMs{20};           // pacer tick
    int leadBufferMs{120};     // FIFO cushion against GUI-thread stalls
    int finishWaitMs{3000};    // end-of-audio barrier
    int tailMs{150};           // after the drain, before unkey
};

// Keys the transmitter, sends one finite block of generated audio in real
// time, and unkeys — every key-up traceable to one start().
// Any failure unkeys and restores the audio path; nothing retries by itself.
// GUI thread only.
class GeneratedAudioTransmitter : public QObject {
    Q_OBJECT
public:
    enum class Phase { Idle, WaitingForStream, Settling, WaitingForPtt, LeadIn, Sending, Draining, Tail };
    Q_ENUM(Phase)
    enum class Outcome { Completed, Stopped, Failed };
    Q_ENUM(Outcome)

    static constexpr int kSampleRate = 24000;
    static constexpr qsizetype kFrameBytes = 2 * qsizetype(sizeof(float));

    explicit GeneratedAudioTransmitter(TxAudioRoute* route, QObject* parent = nullptr,
                                       GeneratedAudioTiming timing = {});

    // Starts the sequence. False, with a reason, when it cannot begin; after
    // true, exactly one finished() follows.
    bool start(const QByteArray& float32Stereo24k, QString& error);
    // Operator stop: unkey now, no tail.
    void stop();
    // Fail closed: unkey now and report why.
    void abort(const QString& reason);

    bool isActive() const { return m_phase != Phase::Idle; }
    Phase phase() const { return m_phase; }
    int audioMs() const;
    int sentMs() const;

public slots:
    // RadioModel::txAudioStreamReady.
    void onStreamReady();
    // TransmitModel::pttBlocked — only a refusal of OUR key request counts.
    void onPttBlocked(const QString& message);
    // TransmitModel::moxChanged(false) — someone released the key under us.
    void onKeyReleased();
    // RadioModel::radioTransmittingChanged / radioTransmitConfirmed.
    void onRadioKeyed(bool keyed);
    // RadioModel::txAudioFinished — drainMs is what the backend still holds.
    void onAudioFinished(quint64 token, int drainMs);

signals:
    void phaseChanged(GeneratedAudioTransmitter::Phase phase);
    void finished(GeneratedAudioTransmitter::Outcome outcome, const QString& reason);

private:
    void setPhase(Phase phase);
    void beginKeying();
    void keyAfterSettle();
    void startLeadIn();
    void startSending();
    void paceTick();
    void finish(Outcome outcome, const QString& reason);
    // Runs fn after ms unless this transmission has ended meanwhile.
    void after(int ms, void (GeneratedAudioTransmitter::*fn)());

    TxAudioRoute* m_route;
    GeneratedAudioTiming m_timing;
    QTimer m_paceTimer;
    QElapsedTimer m_paceClock;

    Phase m_phase{Phase::Idle};
    // Also the end-of-audio barrier token. RadioModel::txAudioFinished is
    // broadcast to every producer, so start far from AetherModem's small
    // counters and a stale barrier of theirs can never read as ours.
    quint64 m_generation{0x564B'0000'0000'0000ULL};
    QByteArray m_pcm;
    qsizetype m_offset{0};
    bool m_claimed{false};
    bool m_keyRequested{false};
    // The route holds admitted authority, whatever the radio has echoed. The
    // release depends on this, not on isKeyed(): a key the radio never showed
    // still left a request with the coordinator.
    bool m_admitted{false};
};

} // namespace AetherSDR
