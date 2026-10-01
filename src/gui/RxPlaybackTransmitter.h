#pragma once

#include "models/TxController.h"   // TxCoordinator::Request / Context

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class RadioModel;
class SliceModel;

// Transmits a Client-Side QSO recording over the radio: AetherRX's PLAY button
// carries "TX Playback" on its context menu, and this is what that entry runs.
//
// The audio takes the path the AX.25 modem and RADE already use -- the
// recording arrives already decoded to wireFormat(), the engine's 24 kHz
// stereo float (QsoRecorder::lastRecordingPcm), and is paced onto the modem TX route
// (AudioEngine::sendModemTxAudio) with local DAX TX mode holding the
// microphone off the wire, and the transmitter is keyed and released through
// the producer PTT API under the operator's own captured input. Nothing here
// keys on its own: start() needs a TxCoordinator::Request captured at the
// menu click, and every refusal, disconnect, PTT block or external unkey
// ends the session with the transmitter released (Principle VI).
//
// One session at a time. Calling start() while active is refused; abort()
// stops the one running.
class RxPlaybackTransmitter : public QObject {
    Q_OBJECT

public:
    RxPlaybackTransmitter(RadioModel* radio, AudioEngine* audio, QObject* parent = nullptr);
    ~RxPlaybackTransmitter() override;

    // True from start() until the transmitter has been released again,
    // including the wait for a DAX TX stream before keying.
    bool active() const { return m_active || m_pendingStream; }

    // What start() takes: 24 kHz stereo float32, interleaved.
    static QAudioFormat wireFormat();
    // How many bytes of wireFormat() audio make `seconds`, for a caller
    // bounding what it hands to start().
    static qsizetype bytesForSeconds(int seconds);

    // Begin transmitting `pcm` (in wireFormat()) on `slice` (made the TX
    // slice first if it is not already) under `input`, the operator's request
    // captured at the click. False, with `whyNot` filled, when the session
    // never began; once it has, every outcome arrives through finished()
    // instead.
    bool start(const QByteArray& pcm, SliceModel* slice,
               const TxCoordinator::Request& input, QString* whyNot = nullptr);

    // Release the transmitter now, whatever is left to send.
    void abort(const QString& reason);

signals:
    void activeChanged(bool active);
    // Every session ends here exactly once, keyed or not.
    void finished(bool aborted, const QString& reason);

private:
    bool bypassesDax() const;
    void beginWhenReady();
    void startAudioAfterPtt();
    void pace();
    void onTxAudioFinished(quint64 token, int drainMs);
    void finish(bool aborted, const QString& reason);
    void disconnectPttConfirmation();

    QPointer<RadioModel> m_radio;
    QPointer<AudioEngine> m_audio;
    QTimer* m_pacer{nullptr};

    TxCoordinator::Request m_request;
    TxCoordinator::Context m_context;
    QByteArray m_pcm;           // 24 kHz stereo float32, interleaved
    qsizetype  m_offset{0};
    // Stamps deferred work and is the token handed to finishModemTxAudio.
    // RadioModel::txAudioFinished is a broadcast every modem-route producer
    // hears, and each compares the token to its own, so tokens are drawn
    // from one process-wide counter started far above the small per-object
    // counters the other producers use: a packet or beacon completing can
    // never satisfy this session's wait, nor this session's theirs.
    quint64    m_generation{0};

    bool m_active{false};
    bool m_pendingStream{false};
    bool m_audioStartArmed{false};
    bool m_awaitingFinish{false};
    bool m_restoreAudioDaxMode{false};
    bool m_previousAudioDaxMode{false};
    bool m_restoreTransmitDax{false};
    bool m_previousTransmitDax{false};

    QElapsedTimer m_paceClock;
    QMetaObject::Connection m_pttConfirm;
    QMetaObject::Connection m_pttConfirmed;
};

} // namespace AetherSDR
