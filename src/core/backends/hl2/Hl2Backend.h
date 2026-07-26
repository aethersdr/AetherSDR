#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/dsp/WdspChannel.h"

#include <QString>

#include <QThread>

#include "core/backends/hl2/Hl2DbReference.h"

namespace AetherSDR::hl2 {

class MetisClient;
class Hl2RxDsp;

// IRadioBackend implementation for the Hermes-Lite 2 (HPSDR Protocol 1, raw IQ).
// Owns a MetisClient (UDP wire) and an Hl2RxDsp (demod + panadapter) and maps the
// neutral seam verbs/signals onto them. This is the first backend that owns an
// engine-side DSP chain (RFC §5.5) rather than decoding a cooked stream.
//
// RX-only: capabilities().canTransmit is false, so the engine TX guard (RFC §6)
// denies keying; setKeying() is a no-op and nothing here can key the radio.
//
// Phase 1b runs the wire + DSP on this object's thread (iqBlockReady ->
// processIqBlock is a direct call); relocating the DSP onto its own thread is a
// later refinement once the data plane is wired through RadioModel.
class Hl2Backend : public IRadioBackend {
    Q_OBJECT

public:
    explicit Hl2Backend(QObject* parent = nullptr);
    ~Hl2Backend() override;

    RadioCapabilities capabilities() const override;

    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz) override;
    void setKeying(bool key) override;

    void invokeExtension(const QString& ns, const QString& verb, quint64 requestId,
                         const QVariant& arg) override;

private:
    void emitSliceState();   // sliceChanged(delta) from current freq/mode/filter
    void emitPanState();     // panCenterBandwidthChanged from freq + sample rate

    MetisClient* m_metis = nullptr;
    Hl2RxDsp* m_dsp = nullptr;
    bool m_connected = false;

    // Authoritative RX state (HL2 has no status wire echoing it back).
    // The slice's tuned frequency, and — separately — where the DDC's NCO sits.
    // These were one value, which nailed the slice to the centre of the
    // panadapter: every tune moved the NCO, so the pan centre moved with it
    // and the display re-centred under the operator on every click. They are
    // now independent, with the slice tuned inside the passband by a WDSP
    // shift and the NCO moved only when the target would leave the window.
    double m_rxFreqHz = 10'000'000.0;   // slice
    double m_ncoHz    = 10'000'000.0;   // DDC / pan centre
    int m_sampleRateHz = 48000;
    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 150;
    int m_filterHighHz = 3000;
    int m_lnaGainDb = 20;
    // Owns the LNA gain <-> dBm coupling so a gain change cannot move the trace.
    Hl2DbReference m_dbRef;

    // The wire and the DSP both live here, off the GUI thread. See MetisClient's
    // header for why the EP2 pacer in particular must not share a thread with
    // the UI. Owned by this object; joined in the destructor.
    QThread* m_ioThread = nullptr;
    // Authoritative AGC state, mirroring the DSP defaults in Hl2RxDsp::Config so
    // the first sliceChanged reports what WDSP was actually opened with.
    QString m_agcMode = QStringLiteral("med");
    int m_agcThresholdDb = 65;

    // Fraction of the half-span the slice may occupy before the NCO re-centres.
    // 0.8 leaves the outer 20% of each side for filter roll-off.
    // Slice AGC threshold (0..100) -> WDSP gain ceiling in dB. 0.6 spans
    // 0..60 dB; see the measurement in setSliceAgc().
    static constexpr double kAgcCeilingDbPerUnit = 0.6;
    static constexpr double kUsablePassbandFraction = 0.8;
    static constexpr int kSliceId = 0;
    static constexpr const char* kPanId = "hl2";
};

}  // namespace AetherSDR::hl2
