#include "RADEEngine.h"
#include "LogManager.h"
#include "Resampler.h"
#include <QString>
#include <cmath>
#include <cstring>
#include <vector>
#ifdef RADE_WAV_TAP
#include <QFile>
#endif

#ifdef HAVE_RADE
extern "C" {
#include "rade_api.h"
#include "rade_text.h"
#include "lpcnet.h"
#include "fargan.h"
}
#endif

#ifdef RADE_WAV_TAP
#ifndef RADE_TAP_DIR
#  define RADE_TAP_DIR "."
#endif
static const char* kTapDir = RADE_TAP_DIR;

static void writeWavFloat(const char* name, int sampleRate, int channels, const QByteArray& samples)
{
    QString path = QString("%1/rade_tap_%2.wav").arg(kTapDir).arg(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("RADE_WAV_TAP: cannot write %s", qPrintable(path));
        return;
    }
    const quint32 dataSize   = static_cast<quint32>(samples.size());
    const quint32 byteRate   = static_cast<quint32>(sampleRate * channels * 4);
    const quint16 blockAlign = static_cast<quint16>(channels * 4);
    const quint16 bitsPerSample = 32;
    const quint16 audioFormat   = 3;  // IEEE_FLOAT
    const quint16 numChannels   = static_cast<quint16>(channels);
    const quint32 sampleRateU   = static_cast<quint32>(sampleRate);
    const quint32 fmtSize  = 16;
    const quint32 riffSize = 36 + dataSize;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&riffSize),      4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmtSize),       4);
    f.write(reinterpret_cast<const char*>(&audioFormat),   2);
    f.write(reinterpret_cast<const char*>(&numChannels),   2);
    f.write(reinterpret_cast<const char*>(&sampleRateU),   4);
    f.write(reinterpret_cast<const char*>(&byteRate),      4);
    f.write(reinterpret_cast<const char*>(&blockAlign),    2);
    f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&dataSize),      4);
    f.write(samples);
    qDebug("RADE_WAV_TAP: wrote %s (%u bytes, %d Hz, %d ch)",
           qPrintable(path), dataSize, sampleRate, channels);
}
#endif

namespace AetherSDR {

#ifdef HAVE_RADE
static void radeTextCallbackFn(rade_text_t, const char *txt, int len, void *state)
{
    auto *engine = static_cast<RADEEngine*>(state);
    emit engine->eooCallsignReceived(QString::fromUtf8(txt, len));
}
#endif

RADEEngine::RADEEngine(QObject* parent)
    : QObject(parent)
{}

RADEEngine::~RADEEngine()
{
    stop();
}

bool RADEEngine::start()
{
#ifdef HAVE_RADE
    if (m_rade) return true;  // already running

    rade_initialize();

    m_rade = rade_open(const_cast<char*>("dummy"),
                       RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
    if (!m_rade) {
        qCWarning(lcRade) << "RADEEngine: rade_open() failed";
        rade_finalize();
        return false;
    }

    // TX: LPCNet feature extractor (speech → features)
    m_lpcnetEnc = lpcnet_encoder_create();
    if (!m_lpcnetEnc) {
        qCWarning(lcRade) << "RADEEngine: lpcnet_encoder_create() failed";
        rade_close(m_rade); m_rade = nullptr;
        rade_finalize();
        return false;
    }

    // RX: FARGAN vocoder (features → speech)
    auto* fargan = new FARGANState;
    fargan_init(fargan);
    m_fargan = fargan;
    m_farganWarmedUp = false;

    // Create resamplers
    m_down24to8  = std::make_unique<Resampler>(24000, 8000);
    // ReqTransBand=45 (max, ~42 taps, ~5ms FIR) instead of default 2.0 (~950 taps, ~118ms).
    // RADE occupies 750-2200 Hz so the wider transition band is safe, and the short FIR
    // clears within the 60ms silence tail so the EOO pilot frame reaches the far-end
    // correlator intact (Prong A confirmed default band drops Dtmax12 below threshold).
    m_up8to24    = std::make_unique<Resampler>(8000, 24000, 4096, 45.0);
    m_down24to16 = std::make_unique<Resampler>(24000, 16000);
    m_up16to24   = std::make_unique<Resampler>(16000, 24000);

    m_txAccum.clear();
    m_txFeatAccum.clear();
    m_rxAccum.clear();
    m_synced = false;

    m_radeText = rade_text_create();
    if (m_radeText) {
        rade_text_set_rx_callback(m_radeText, radeTextCallbackFn, this);
    } else {
        qCWarning(lcRade) << "RADEEngine: rade_text_create() failed — EOO callsign decode disabled";
    }

    int n_features = rade_n_features_in_out(m_rade);
    int n_tx_out = rade_n_tx_out(m_rade);
    int nin = rade_nin(m_rade);
    qCDebug(lcRade) << "RADEEngine: started — n_features=" << n_features
            << "n_tx_out=" << n_tx_out << "nin=" << nin;
    return true;
#else
    qCWarning(lcRade) << "RADEEngine: built without RADE support (HAVE_RADE not defined)";
    return false;
#endif
}

void RADEEngine::stop()
{
#ifdef HAVE_RADE
    if (!m_rade) return;

    if (m_lpcnetEnc) {
        lpcnet_encoder_destroy(m_lpcnetEnc);
        m_lpcnetEnc = nullptr;
    }
    if (m_fargan) {
        delete static_cast<FARGANState*>(m_fargan);
        m_fargan = nullptr;
    }
    if (m_radeText) {
        rade_text_destroy(m_radeText);
        m_radeText = nullptr;
    }

    rade_close(m_rade);
    m_rade = nullptr;
    rade_finalize();

    m_txAccum.clear();
    m_txFeatAccum.clear();
    m_rxAccum.clear();
    m_synced = false;
    m_farganWarmedUp = false;
    m_eooRequested = false;
    m_eooSent = false;
    m_eooFinished = false;

    qCDebug(lcRade) << "RADEEngine: stopped";
#endif
}

bool RADEEngine::isActive() const
{
#ifdef HAVE_RADE
    return m_rade != nullptr;
#else
    return false;
#endif
}

bool RADEEngine::isSynced() const
{
#ifdef HAVE_RADE
    return m_synced;
#else
    return false;
#endif
}

void RADEEngine::resetTx()
{
#ifdef HAVE_RADE
    m_txAccum.clear();
    m_txFeatAccum.clear();
    m_eooRequested = false;
    m_eooSent = false;
    m_eooFinished = false;
#ifdef RADE_WAV_TAP
    m_tapVoiceAccum.clear();
    m_tap8kVoiceAccum.clear();
#endif
#endif
}

void RADEEngine::setEooRequested(bool requested)
{
#ifdef HAVE_RADE
    if (m_eooRequested == requested) return;
    m_eooRequested = requested;
    if (requested) {
        qCDebug(lcRade) << "RADEEngine: EOO requested — draining pipeline...";
        // Trigger a feed with empty audio to kick the drain logic if no more mic audio is coming
        feedTxAudio(QByteArray());
    }
#else
    Q_UNUSED(requested);
#endif
}

void RADEEngine::setTxCallsign(const QString& callsign)
{
#ifdef HAVE_RADE
    if (!m_rade || !m_radeText) return;
    const QByteArray cs = callsign.toUpper().trimmed().toLatin1();
    const int n_eoo_bits = rade_n_eoo_bits(m_rade);
    std::vector<float> eooSyms(n_eoo_bits);
    rade_text_generate_tx_string(m_radeText, cs.constData(), cs.size(),
                                 eooSyms.data(), n_eoo_bits);
    rade_tx_set_eoo_bits(m_rade, eooSyms.data());
    qCDebug(lcRade) << "RADEEngine: TX EOO callsign encoded (LDPC) —" << cs;
#else
    Q_UNUSED(callsign);
#endif
}

void RADEEngine::feedTxAudio(const QByteArray& pcm)
{
#ifdef HAVE_RADE
    if (!m_rade || !m_lpcnetEnc || m_eooFinished) return;

    if (m_eooRequested && pcm.isEmpty())
        qCDebug(lcRade) << "RADEEngine: drain kick —"
                        << "txAccum=" << m_txAccum.size()
                        << "featAccum=" << m_txFeatAccum.size();

    if (!pcm.isEmpty()) {
        // 1. Downsample 24kHz stereo float32 → 16kHz mono float32 for LPCNet
        const auto* srcF = reinterpret_cast<const float*>(pcm.constData());
        QByteArray mono16k = m_down24to16->processStereoToMono(srcF, pcm.size() / (2 * static_cast<int>(sizeof(float))));

        // 2. Convert float32 mono → int16 mono for LPCNet
        const auto* mf = reinterpret_cast<const float*>(mono16k.constData());
        const int nMono = mono16k.size() / static_cast<int>(sizeof(float));
        QByteArray mono16kInt16(nMono * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
        auto* mi = reinterpret_cast<int16_t*>(mono16kInt16.data());
        for (int i = 0; i < nMono; ++i)
            mi[i] = static_cast<int16_t>(std::clamp(mf[i] * 32768.0f, -32768.0f, 32767.0f));
        m_txAccum.append(mono16kInt16);
    }

    // Process 10ms frames (LPCNET_FRAME_SIZE = 160 samples @ 16kHz)
    while ((m_txAccum.size() / sizeof(int16_t)) >= LPCNET_FRAME_SIZE || (m_eooRequested && !m_txAccum.isEmpty())) {
        int nToTake = std::min<int>(LPCNET_FRAME_SIZE, static_cast<int>(m_txAccum.size() / sizeof(int16_t)));
        QByteArray sampleArray = m_txAccum.left(nToTake * sizeof(int16_t));
        m_txAccum.remove(0, sampleArray.size());

        // Pad partial frame with zeros if draining
        if (sampleArray.size() < (int)(LPCNET_FRAME_SIZE * sizeof(int16_t))) {
            sampleArray.append(QByteArray((LPCNET_FRAME_SIZE * sizeof(int16_t)) - sampleArray.size(), 0));
        }

        const int16_t* samples = reinterpret_cast<const int16_t*>(sampleArray.constData());

        // Extract features for one 10ms frame
        float features[NB_TOTAL_FEATURES];
        lpcnet_compute_single_frame_features(m_lpcnetEnc, const_cast<int16_t*>(samples), features, 0);

        // Accumulate features (RADE needs n_features_in features per call)
        m_txFeatAccum.append(reinterpret_cast<const char*>(features), NB_TOTAL_FEATURES * sizeof(float));
    }

    // RADE encoder needs 12 feature frames (120ms)
    int n_features_in = rade_n_features_in_out(m_rade);
    int n_tx_out = rade_n_tx_out(m_rade);
    while ((m_txFeatAccum.size() / static_cast<qsizetype>(sizeof(float))) >= qsizetype(n_features_in) || (m_eooRequested && !m_txFeatAccum.isEmpty())) {
        int nFeatures = static_cast<int>(m_txFeatAccum.size() / sizeof(float));
        int nToProcess = std::min(n_features_in, nFeatures);
        
        std::vector<float> feat_in(n_features_in, 0.0f);
        memcpy(feat_in.data(), m_txFeatAccum.constData(), nToProcess * sizeof(float));
        m_txFeatAccum.remove(0, nToProcess * sizeof(float));

        std::vector<RADE_COMP> tx_out(n_tx_out);
        rade_tx(m_rade, tx_out.data(), feat_in.data());

        // 3. Convert RADE_COMP → 8kHz mono float32 (take real part)
        QByteArray modem8k(n_tx_out * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(modem8k.data());
        for (int i = 0; i < n_tx_out; ++i)
            out[i] = tx_out[i].real;

        // 4. Upsample 8kHz mono float32 → 24kHz stereo float32
        QByteArray stereo24k = m_up8to24->processMonoToStereo(out, n_tx_out);
#ifdef RADE_WAV_TAP
        m_tapVoiceAccum.append(stereo24k);
        m_tap8kVoiceAccum.append(modem8k);
#endif
        emit txModemReady(stereo24k);
    }

    // Final Stage: Generate EOO frame if requested and voice pipeline is empty
    if (m_eooRequested && !m_eooSent && m_txAccum.isEmpty() && m_txFeatAccum.isEmpty()) {
        qCDebug(lcRade) << "RADEEngine: Voice pipeline drained — generating EOO frame";
        
        int n_eoo_out = rade_n_tx_eoo_out(m_rade);
        std::vector<RADE_COMP> eoo_samples(n_eoo_out);
        int n = rade_tx_eoo(m_rade, eoo_samples.data());
        
        if (n > 0) {
            std::vector<float> eoo_mono(n);
            for (int i = 0; i < n; ++i)
                eoo_mono[i] = eoo_samples[i].real;

            QByteArray eoo24k = m_up8to24->processMonoToStereo(eoo_mono.data(), n);

            // Append silence to push EOO through the upsampler's FIR taps
            int silenceSamples = kEooSilenceTailMs * 24000 / 1000;
            QByteArray silence(silenceSamples * 2 * static_cast<int>(sizeof(float)), 0);

#ifdef RADE_WAV_TAP
            // Tap A — raw 8kHz mono from rade_tx_eoo(), before any upsampling.
            writeWavFloat("A_8k_mono_raw_eoo", 8000, 1,
                QByteArray(reinterpret_cast<const char*>(eoo_mono.data()),
                           n * static_cast<int>(sizeof(float))));
            // Tap B — 24kHz stereo after r8brain upsample.
            writeWavFloat("B_24k_stereo_eoo_upsampled", 24000, 2, eoo24k);
            // Tap D — eoo24k + silence as emitted to AudioEngine.
            {
                QByteArray tapD;
                tapD.reserve(eoo24k.size() + silence.size());
                tapD.append(eoo24k);
                tapD.append(silence);
                writeWavFloat("D_24k_stereo_eoo_block", 24000, 2, tapD);
            }
            // Tap E — full TX session: all voice modem frames + eoo + silence.
            {
                QByteArray tapE;
                tapE.reserve(m_tapVoiceAccum.size() + eoo24k.size() + silence.size());
                tapE.append(m_tapVoiceAccum);
                tapE.append(eoo24k);
                tapE.append(silence);
                writeWavFloat("E_24k_stereo_full_session", 24000, 2, tapE);
            }
            // Tap F — 8kHz mono full session (voice + EOO, no upsampling).
            {
                QByteArray eoo8k(reinterpret_cast<const char*>(eoo_mono.data()),
                                 n * static_cast<int>(sizeof(float)));
                QByteArray tapF;
                tapF.reserve(m_tap8kVoiceAccum.size() + eoo8k.size());
                tapF.append(m_tap8kVoiceAccum);
                tapF.append(eoo8k);
                writeWavFloat("F_8k_mono_full_session", 8000, 1, tapF);
            }
#endif
            qCDebug(lcRade) << "RADEEngine: rade_tx_eoo n=" << n
                            << "eoo24k=" << eoo24k.size() << "bytes"
                            << "silence=" << silence.size() << "bytes"
                            << "— emitting both before eooFinished";
            emit txModemReady(eoo24k);
            emit txModemReady(silence);
        } else {
            qCWarning(lcRade) << "RADEEngine: rade_tx_eoo returned" << n << "(no EOO samples generated)";
        }

        m_eooSent = true;
        m_eooFinished = true;
        emit eooFinished();
        qCDebug(lcRade) << "RADEEngine: EOO transmission complete — eooFinished emitted";
    }
#else
    Q_UNUSED(pcm);
#endif
}

void RADEEngine::feedRxAudio(int channel, const QByteArray& pcm)
{
#ifdef HAVE_RADE
    if (!m_rade || !m_fargan) return;
    if (channel != 1) return;  // only process DAX channel 1
    auto* fargan = static_cast<FARGANState*>(m_fargan);

    QByteArray speech16k;

    // 1. Downsample 24kHz stereo float32 → 8kHz mono float32 for RADE modem
    const auto* srcF = reinterpret_cast<const float*>(pcm.constData());
    QByteArray mono8k = m_down24to8->processStereoToMono(srcF, pcm.size() / (2 * static_cast<int>(sizeof(float))));

    // 2. Convert float32 → RADE_COMP (real = sample, imag = 0)
    const auto* samples = reinterpret_cast<const float*>(mono8k.constData());
    int nSamples = mono8k.size() / static_cast<int>(sizeof(float));

    // Append as RADE_COMP to accumulator
    for (int i = 0; i < nSamples; ++i) {
        RADE_COMP c;
        c.real = samples[i];
        c.imag = 0.0f;
        m_rxAccum.append(reinterpret_cast<const char*>(&c), sizeof(RADE_COMP));
    }

    // 3. Process when we have enough samples
    int nin = rade_nin(m_rade);
    while (m_rxAccum.size() >= static_cast<int>(nin * sizeof(RADE_COMP))) {
        int n_features_out = rade_n_features_in_out(m_rade);
        std::vector<float> features_out(n_features_out);
        int has_eoo = 0;
        int n_eoo_bits = rade_n_eoo_bits(m_rade);
        std::vector<float> eoo_out(n_eoo_bits);

        auto* rx_in = reinterpret_cast<RADE_COMP*>(m_rxAccum.data());
        int n_out = rade_rx(m_rade, features_out.data(), &has_eoo, eoo_out.data(), rx_in);

        // Remove consumed samples
        m_rxAccum.remove(0, nin * sizeof(RADE_COMP));

        if (has_eoo && m_radeText) {
            qCDebug(lcRade) << "RADEEngine: EOO frame received — decoding callsign (LDPC+CRC)";
            // eoo_out contains n_eoo_bits floats in IQIQI order; symSize = n_eoo_bits/2
            rade_text_rx(m_radeText, eoo_out.data(),
                         static_cast<int>(eoo_out.size()) / 2);
            // decoded callsign fires radeTextCallbackFn → eooCallsignReceived signal
        }

        // 4. If features available, synthesize speech via FARGAN
        if (n_out > 0) {
            m_rxFeatAccum.append(reinterpret_cast<const char*>(&features_out[0]), sizeof(float) * n_out);
        }

        while (m_rxFeatAccum.size() >= qsizetype(sizeof(float) * NB_TOTAL_FEATURES))
        {
            // FARGAN warmup: need initial features
            if (!m_farganWarmedUp) {
                // Feed zeros for warmup
                float zeros[320] = {0};
                float warmup_features[5 * NB_TOTAL_FEATURES] = {0};
                fargan_cont(fargan, zeros, warmup_features);
                m_farganWarmedUp = true;
            }

            // Process features frame by frame (NB_TOTAL_FEATURES per frame)
            // FARGAN uses only first NB_FEATURES (20) features
            const float* feat = reinterpret_cast<const float*>(m_rxFeatAccum.constData());
            float fpcm[LPCNET_FRAME_SIZE];
            fargan_synthesize(fargan, fpcm, feat);

            // Append float32 samples directly
            speech16k.append(reinterpret_cast<const char*>(fpcm),
                             LPCNET_FRAME_SIZE * sizeof(float));

            m_rxFeatAccum.remove(0, sizeof(float) * NB_TOTAL_FEATURES);
        }

        nin = rade_nin(m_rade);
    }

    // 5. Upsample 16kHz mono → 24kHz stereo for speaker
    if (!speech16k.isEmpty())
    {
        auto tmp = m_up16to24->processMonoToStereo(reinterpret_cast<const float*>(speech16k.constData()), speech16k.size() / static_cast<int>(sizeof(float)));
        m_rxOutAccum.append(tmp);
    }

    if (m_rxOutAccum.size() >= pcm.size())
    {
        emit rxSpeechReady(m_rxOutAccum.left(pcm.size()));
        m_rxOutAccum.remove(0, pcm.size());
    }
    else
    {
        emit rxSpeechReady(QByteArray(pcm.size(), '\0'));
    }

    // Update sync status
    bool synced = rade_sync(m_rade) != 0;
    if (synced != m_synced) {
        m_synced = synced;
        emit syncChanged(synced);
    }
    if (synced) {
        emit snrChanged(static_cast<float>(rade_snrdB_3k_est(m_rade)));
        emit freqOffsetChanged(static_cast<float>(rade_freq_offset(m_rade)));
    }

#else
    Q_UNUSED(channel); Q_UNUSED(pcm);
#endif
}

QString RADEEngine::versionString()
{
#ifdef HAVE_RADE
    return QString("RADE v%1").arg(rade_version());
#else
    return QString();
#endif
}

} // namespace AetherSDR

#if 0 // Old resampling helpers replaced by r8brain Resampler
QByteArray RADEEngine::downsample24kTo8k(const QByteArray& stereo24k)
{
    // 24kHz stereo int16 → 8kHz mono int16 (3:1 decimation with averaging)
    const auto* in = reinterpret_cast<const int16_t*>(stereo24k.constData());
    int totalFrames = stereo24k.size() / 4;  // stereo frames (4 bytes each)
    int outSamples = totalFrames / 3;

    QByteArray out(outSamples * 2, Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(out.data());

    for (int i = 0; i < outSamples; ++i) {
        // Average 3 stereo frames (L+R averaged to mono, then 3 mono samples averaged)
        int base = i * 3 * 2;  // *2 for stereo
        int sum = (in[base] + in[base + 1])       // frame 0: avg L+R
                + (in[base + 2] + in[base + 3])   // frame 1: avg L+R
                + (in[base + 4] + in[base + 5]);   // frame 2: avg L+R
        dst[i] = static_cast<int16_t>(sum / 6);
    }
    return out;
}

QByteArray RADEEngine::upsample8kTo24k(const QByteArray& mono8k)
{
    // 8kHz mono int16 → 24kHz stereo int16 (1:3 interpolation, duplicate to stereo)
    const auto* in = reinterpret_cast<const int16_t*>(mono8k.constData());
    int nSamples = mono8k.size() / 2;
    int outSamples = nSamples * 3;

    QByteArray out(outSamples * 2 * 2, Qt::Uninitialized);  // *2 for stereo, *2 for int16
    auto* dst = reinterpret_cast<int16_t*>(out.data());

    for (int i = 0; i < nSamples; ++i) {
        int16_t s = in[i];
        // Simple sample-and-hold interpolation (good enough for modem signal)
        for (int j = 0; j < 3; ++j) {
            int idx = (i * 3 + j) * 2;
            dst[idx]     = s;  // left
            dst[idx + 1] = s;  // right
        }
    }
    return out;
}

QByteArray RADEEngine::downsample24kTo16k(const QByteArray& stereo24k)
{
    // 24kHz stereo int16 → 16kHz mono int16 (3:2 rational resampling)
    // For every 3 input frames, produce 2 output samples using linear interpolation
    const auto* in = reinterpret_cast<const int16_t*>(stereo24k.constData());
    int totalFrames = stereo24k.size() / 4;  // stereo frames (4 bytes each)

    // First convert to mono (avg L+R)
    std::vector<int16_t> mono(totalFrames);
    for (int i = 0; i < totalFrames; ++i)
        mono[i] = static_cast<int16_t>((in[i * 2] + in[i * 2 + 1]) / 2);

    int outSamples = (totalFrames * 2) / 3;
    QByteArray out(outSamples * 2, Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(out.data());

    for (int i = 0; i < outSamples; ++i) {
        // Output position i maps to input position i * 1.5
        float srcPos = i * 1.5f;
        int idx = static_cast<int>(srcPos);
        float frac = srcPos - idx;
        if (idx + 1 < totalFrames)
            dst[i] = static_cast<int16_t>(mono[idx] * (1.0f - frac) + mono[idx + 1] * frac);
        else
            dst[i] = mono[idx];
    }
    return out;
}

QByteArray RADEEngine::upsample16kTo24k(const QByteArray& mono16k)
{
    // 16kHz mono int16 → 24kHz stereo int16 (2:3 interpolation)
    // From 2 input samples, produce 3 stereo frames
    const auto* in = reinterpret_cast<const int16_t*>(mono16k.constData());
    int nSamples = mono16k.size() / 2;
    int outFrames = (nSamples * 3) / 2;

    QByteArray out(outFrames * 4, Qt::Uninitialized);  // 4 bytes per stereo frame
    auto* dst = reinterpret_cast<int16_t*>(out.data());

    int outIdx = 0;
    for (int i = 0; i + 1 < nSamples; i += 2) {
        int16_t s0 = in[i];
        int16_t s1 = in[i + 1];
        int16_t mid = static_cast<int16_t>((s0 + s1) / 2);

        // 2 input → 3 output frames (linear interpolation)
        if (outIdx + 5 < outFrames * 2) {
            dst[outIdx++] = s0;  dst[outIdx++] = s0;   // frame 0: L R
            dst[outIdx++] = mid; dst[outIdx++] = mid;   // frame 1: L R
            dst[outIdx++] = s1;  dst[outIdx++] = s1;    // frame 2: L R
        }
    }
    out.resize(outIdx * 2);
    return out;
}
#endif
