#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <memory>

#ifdef HAVE_RADE
struct rade;
struct LPCNetEncState;
// FARGANState is defined in fargan.h (opus), included only in .cpp
// Use void* here to avoid pulling in opus headers
// rade_text_t is typedef void* in the C header — mirror it here to avoid pulling in C headers
using rade_text_t = void*;
#endif

namespace MasterSDR {

class Resampler;

// Wraps the RADE v1 (Radio Autoencoder) codec for FreeDV digital voice.
// The radio is set to DIGU (SSB passthrough). RADE handles the encoding
// and decoding locally:
//
// TX: Mic(24kHz stereo) → 16kHz mono → LPCNet features → rade_tx() → RADE_COMP(8kHz) → 24kHz stereo → DAX TX
// RX: DAX RX(24kHz stereo) → 8kHz mono → RADE_COMP → rade_rx() → features → FARGAN → 16kHz mono → 24kHz stereo
//
class RADEEngine : public QObject {
    Q_OBJECT

public:
    explicit RADEEngine(QObject* parent = nullptr);
    ~RADEEngine() override;

    bool start();
    void stop();
    bool isActive() const;
    bool isSynced() const;

    // Returns "RADE vN" where N is the integer from rade_version(), or empty if RADE not compiled in.
    static QString versionString();

public slots:
    // Feed DAX RX audio (24kHz stereo int16) for decoding.
    // channel is the DAX channel number (1-4), only processes channel 1.
    void feedRxAudio(int channel, const QByteArray& pcm);

    // Feed mic audio (24kHz stereo int16) for encoding.
    void feedTxAudio(const QByteArray& pcm);

    // Flush TX encoder state (call on MOX release to prevent stale audio)
    void resetTx();

signals:
    void rxSpeechReady(const QByteArray& pcm);   // Decoded speech, 24kHz stereo int16
    void txModemReady(const QByteArray& pcm);     // Encoded modem, 24kHz stereo int16
    void syncChanged(bool synced);
    void snrChanged(float snrDb);
    void freqOffsetChanged(float hz);
    void eooCallsignReceived(const QString& callsign);

private:
#ifdef HAVE_RADE
    struct rade*         m_rade{nullptr};
    LPCNetEncState*      m_lpcnetEnc{nullptr};
    void*                m_fargan{nullptr};  // actually FARGANState*, opaque here
    rade_text_t          m_radeText{nullptr}; // EOO callsign encoder/decoder
    bool                 m_synced{false};

    // TX accumulation: 12 frames of NB_TOTAL_FEATURES = 432 floats
    QByteArray m_txAccum;
    QByteArray m_txFeatAccum;

    // RX accumulation: rade_nin() RADE_COMP samples
    QByteArray m_rxAccum;
    QByteArray m_rxFeatAccum;
    QByteArray m_rxOutAccum;

    bool m_farganWarmedUp{false};

    // Resamplers (r8brain)
    std::unique_ptr<Resampler> m_down24to8;   // 24k→8k (modem RX input)
    std::unique_ptr<Resampler> m_up8to24;     // 8k→24k (modem TX output)
    std::unique_ptr<Resampler> m_down24to16;  // 24k→16k (LPCNet TX input)
    std::unique_ptr<Resampler> m_up16to24;    // 16k→24k (FARGAN RX output)
#endif
};

} // namespace MasterSDR
