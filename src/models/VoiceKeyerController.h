#pragma once

#include "models/VoiceKeyerSource.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

namespace AetherSDR {

class AudioEngine;
class AudioOutputRouter;
class GeneratedAudioTransmitter;
class LocalVoiceKeyer;
class LocalWavPlayer;
class RadioModel;
class VoiceKeyer;
class VoiceKeyerTxRoute;

// Owns the client-side voice keyer and everything it needs to run: the keyer
// itself, the transmit route, the on-air transmitter, the preview player and
// the persisted source choice. It resolves which keyer the panel should drive —
// the radio-hosted DVK or the local one — and keeps that choice away from the
// UI, which only needs the VoiceKeyer interface and this object's API.
//
// The keyer's own state lives in LocalVoiceKeyer, which stays free of
// RadioModel and AudioEngine so it can be unit-tested on its own. This class is
// where the radio enters.
class VoiceKeyerController : public QObject {
    Q_OBJECT
public:
    VoiceKeyerController(RadioModel& radio,
                         AudioEngine* audio,
                         AudioOutputRouter* outputRouter,
                         QObject* parent = nullptr);
    ~VoiceKeyerController() override;

    // The keyer the panel should be driving now. Never null once constructed.
    VoiceKeyer* bound() const { return m_bound; }

    // The operator's stored choice, and the keyer it resolves to against the
    // radio's reported entitlement.
    VoiceKeyerSourceSetting sourceSetting() const;
    void setSourceSetting(VoiceKeyerSourceSetting setting);
    VoiceKeyerSource resolvedSource() const;

    // Where local recordings are kept, for the folder action and its tooltip.
    QString recordingsDir() const;

    // Re-read the local slots from disk, so a WAV dropped into the folder
    // appears without a restart.
    void reloadLocalSlots();

    // Whether local audio is on the air, and the abort the TX-mode guard needs.
    bool isTransmitting() const;
    void abortTransmission(const QString& reason);

    // Re-resolve the binding after a licence or capability change. Deferred
    // while the bound keyer is busy, so a swap never lands mid-transmission.
    void refreshBinding();

signals:
    // The panel must rebind. Carries the keyer so the UI never resolves one.
    void boundKeyerChanged(AetherSDR::VoiceKeyer* keyer);
    // VOX is held off for the duration of a recording, so the operator's own
    // voice cannot key the radio while they record. The panel says so, because
    // a radio setting changed and the operator did not change it.
    void voxHeldOffChanged(bool held);
    // The operator chose a source while the bound keyer was mid-operation, so
    // the swap waits for that operation to end. Emitted once per pending
    // change; boundKeyerChanged() follows when it is applied.
    void sourceChangeDeferred(AetherSDR::VoiceKeyerSource wanted);
    // Forwarded from the preview player: live RX leaves the sink for the
    // duration of a preview. The app owns what "mute RX" means per backend.
    void muteRxRequested(bool mute);

private:
    void wireTransmitter();

    RadioModel& m_radio;
    QPointer<AudioEngine> m_audio;
    LocalVoiceKeyer* m_local{nullptr};
    LocalWavPlayer* m_player{nullptr};
    VoiceKeyerTxRoute* m_route{nullptr};
    GeneratedAudioTransmitter* m_transmitter{nullptr};
    VoiceKeyer* m_bound{nullptr};
    // The keyer a deferral has already been announced for, so a busy keyer's
    // every status tick does not re-announce the same pending change.
    VoiceKeyer* m_deferralAnnounced{nullptr};
    // True while a recording holds the radio's VOX off, so release puts back
    // only what this took.
    bool m_voxHeldOff{false};
    // Recording does not start until the radio confirms VOX is off, because
    // asking is not the same as it being off and the operator's voice would
    // key the transmitter in the gap. This bounds the wait.
    QTimer m_voxConfirmTimer;
    QMetaObject::Connection m_voxConfirmWatch;
    void beginVoxHold();
    void finishVoxHold(bool confirmed);
    void restoreVox();
};

}  // namespace AetherSDR
