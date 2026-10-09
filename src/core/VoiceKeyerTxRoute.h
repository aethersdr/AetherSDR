#pragma once

#include "core/GeneratedAudioTransmitter.h"
#include "core/TxCoordinator.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class AudioEngine;
class RadioModel;

// The transmitter's view of the connected radio: the TxAudioRoute the
// client-side voice keyer plays through. Every per-family difference is an
// answer read from RadioCapabilities, never a family name — the same questions
// Ax25HfPacketDecodeDialog asks, so HL2 and Icom need no new code.
//
// Keying and audio both go through one TxCoordinator producer request, the path
// AX.25 and RADE already use (#5659). The request is what admits the operation;
// the media context captured from it is what licenses every block to reach the
// transport. Keying through transmitModel() directly still keys the radio, but
// its audio then carries no admitted operation and AudioEngine::selectTxContext
// drops every block — a keyed, silent transmitter, which is why keyOn() reports
// refusal instead.
class VoiceKeyerTxRoute : public QObject, public TxAudioRoute {
    Q_OBJECT
public:
    VoiceKeyerTxRoute(RadioModel& radio, AudioEngine* audio, QObject* parent = nullptr);
    ~VoiceKeyerTxRoute() override;

    QString startRefusal() const override;
    bool admitOperation() override;

    bool needsStream() const override;
    bool streamReady() const override;
    bool requestStream() override;

    void claimAudioPath() override;
    void releaseAudioPath() override;

    bool keyOn() override;
    void keyOff() override;
    bool isKeyed() const override;
    bool waitsForRadioPtt() const override;
    bool isRadioKeyed() const override;

    void sendAudio(const QByteArray& float32Stereo24k) override;
    bool finishAudio(quint64 token) override;
    void clearAudio() override;

private:
    RadioModel& m_radio;
    QPointer<AudioEngine> m_audio;
    // One producer for the life of the route; one request per transmission.
    TxCoordinator::Producer m_producer;
    TxCoordinator::Request m_request;
    TxCoordinator::Context m_context;
    bool m_prevAudioDax{false};
    bool m_prevTransmitDax{false};
    bool m_restoreTransmitDax{false};
    // PTT was asked for against m_request; keyOff() hands back only what it took.
    bool m_pttRequested{false};
};

}  // namespace AetherSDR
