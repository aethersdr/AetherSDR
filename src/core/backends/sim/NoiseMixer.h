#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace AetherSDR {

// NoiseMixer — the demo-mode AUDIO engine (RFC #4288, Phase 2b), the audio
// sibling of SpectrumPatternGenerator. Where that class draws the panadapter,
// this one synthesizes the RX AUDIO a demo radio delivers, so AE's noise
// reduction (NR2/RN2/NR4/BNR/LMS-NR) can run against realistic HF noise with no
// hardware. SimBackend emits mixFrame() over audioFrameReady().
//
// It is an ADDITIVE mixer: N independently-enabled channels sum in linear sample
// space, then soft-clip. Signal channels (voice/cw) are the wanted signal NR
// must PRESERVE; noise channels (white/pink/qrn/…) are what it must REMOVE. All
// noise is generated on the spot — the whole point is non-repeating randomness.
//
// Notches (spectrumNotches()/audio): a Tracking Notch Filter (TNF, manual) or an
// Auto-Notch Filter (ANF, which auto-detects the tonal channels) removes a
// carrier from both the audio (biquad) and the matching spectrum render — one
// notch list drives both, so ear and eye agree.
//
// Ported from nigelfenton/flex-sim (GPL-3.0) NoiseMixer against
// flex-sim/PROTOCOL.md — a clean-room C++ reimplementation of our own GPL code,
// no verbatim copy (Constitution Principle IV). Audio is 24 kHz mono float in
// [-1,1]; the caller stereo-duplicates for AE's remote-audio format.
//
// TX safety (Principle VI): this only synthesizes RX audio. It never keys or
// implies transmit; SimBackend gates it to silence while keyed.
class NoiseMixer {
public:
    static constexpr int    kSampleRate = 24000;   // AE remote-audio RX rate
    static constexpr int    kFrameLen   = 128;     // samples per mixFrame()

    enum class Channel {
        // signal (wanted — NR must preserve)
        Cw,          // a keyed CW tone at Config::cwHz
        Voice,       // REAL speech: a bundled public-domain clip (Harvard sentences)
                     // looped, so NR has genuine voice to preserve and ASR has real
                     // words to transcribe. 24 kHz mono, no resample.
        // noise (unwanted — NR must remove)
        White,       // flat Gaussian AWGN — the thermal baseline
        Pink,        // 1/f band hiss (Voss-McCartney) — atmospheric floor
        Qrn,         // Poisson-timed impulse crackle (lightning static)
        Powerline,   // mains hum: fundamental + odd harmonics
        Crashes,     // correlated static bursts (storm front)
        Birdie,      // steady carrier heterodyne (a tone for notch tests)
        Hash,        // switching-supply broadband clumps
        Woodpecker,  // pulsed wideband rasp
    };
    // Voice plays a bundled public-domain speech clip (:/demo_voice.wav, Harvard
    // sentences, 24 kHz mono) looped — REAL words, so it exercises NR's voice
    // preservation and can be transcribed by ASR (Copy Assist, #4338). Loaded once,
    // lazily, on first use.

    struct ChannelState {
        bool   enabled  = false;
        double levelDb  = -24.0;   // dBFS-ish; 0 = full reference amplitude
        double hz       = 0.0;     // tonal channels (cw/birdie): pitch
        double rate     = 0.0;     // qrn/crashes: events/sec
        double freq     = 60.0;    // powerline: 50 or 60 Hz
        double prf      = 0.0;     // hash/woodpecker: pulse repetition (Hz)
    };

    struct Notch { double hz; double widthHz; };   // audio-Hz offset from the VFO

    NoiseMixer();

    // ---- channel control ----
    void setEnabled(Channel c, bool on);
    void setLevelDb(Channel c, double db);
    void setKnob(Channel c, const QString& knob, double value);  // hz/rate/freq/prf
    bool anyEnabled() const;
    const ChannelState& channel(Channel c) const;

    // ---- notches (TNF manual + ANF auto) ----
    void setNotches(const std::vector<Notch>& notches);

    // Noise blanker: when on, mixFrame() gates out short impulse spikes (QRN/
    // crashes) — samples that jump well above the running level are attenuated,
    // the way a real NB clips the impulse before it reaches the audio.
    void setNoiseBlank(bool on) { m_nbOn = on; }
    bool noiseBlank() const { return m_nbOn; }
    // ANF detection: the audio-Hz offsets of the active TONAL channels (birdie,
    // cw) — a real Auto-Notch Filter finds these by their narrow signature; the
    // sim knows them directly. Broadband/impulse noise is NOT returned.
    std::vector<Notch> autoNotchTones() const;

    // ---- generation ----
    // One 24 kHz mono float frame (kFrameLen samples) of the summed, notched,
    // soft-clipped mix. Advances internal phase/RNG state.
    QVector<float> mixFrame();

    // The active scene rendered as per-bin dBm for the panadapter, so the
    // waterfall shows what the audio carries. Noise BOTTOMS OUT on floorDbm and
    // rises up (real noise is a floor, not a floating band); notches carve to
    // the floor. centerBin is the VFO (audio 0 Hz); +Hz maps to the right.
    QVector<float> spectrum(int nBins, double floorDbm, double spanHz,
                            int centerBin);

    // stable lowercase name <-> Channel (settings + a UI picker later)
    static QString  name(Channel c);
    static Channel  fromName(const QString& n, bool* ok = nullptr);
    static QVector<Channel> allChannels();

    // Named scenes (storm / night-40m / …) for one-click demo presets. Loading a
    // preset disables every channel, then enables + levels the ones it names.
    void loadPreset(const QString& presetName);      // no-op on unknown name
    static QStringList allPresetNames();

private:
    // --- generators: fill `out` (kFrameLen) at unity reference for one channel ---
    void genCw(const ChannelState&, float* out);
    void genVoice(const ChannelState&, float* out);
    void genWhite(const ChannelState&, float* out);
    void genPink(const ChannelState&, float* out);
    void genQrn(const ChannelState&, float* out);
    void genPowerline(const ChannelState&, float* out);
    void genCrashes(const ChannelState&, float* out);
    void genBirdie(const ChannelState&, float* out);
    void genHash(const ChannelState&, float* out);
    void genWoodpecker(const ChannelState&, float* out);

    double nextGauss();                 // Box-Muller, own RNG
    double nextUniform();               // [0,1) — AUDIO stream; see nextDisplayUniform
    // Separate RNG stream for spectrum()'s per-bin grass.
    //
    // spectrum() is called from inside the audio-generation loop (SimBackend emits
    // a display row every 9th audio frame) and draws once per bin — 1024 draws.
    // Sharing m_rng with the audio generators therefore yanked their random
    // sequence forward mid-stream ~21 times a second, putting a step discontinuity
    // into the pink/gaussian noise at a perfectly regular interval: an audible
    // periodic scratchy buzz. The display's grass has no reason to share the
    // audio's entropy, so it gets its own stream and cannot perturb it.
    double nextDisplayUniform();        // [0,1) — DISPLAY stream
    QVector<float> applyNotch(const QVector<float>& buf, double fHz, double q);

    std::map<Channel, ChannelState> m_ch;

    // phase counters (deterministic tone/buzz generators)
    qint64 m_cwPhase = 0;      // sample counter; long overflows in ~25 h on LLP64
    int    m_cwSpan  = 0;      // current schedule span (persistent, no per-sample rescan)
    double m_cwPos   = 0.0;    // last in-cycle position, for wrap detection
    // CW tone as a CONTINUOUS RADIANS accumulator, same contract as the birdie
    // below: a mid-dah pitch change (setKnob "hz" is live via SimBackend's
    // passthrough) must slide the frequency, never teleport the phase — a
    // phase jump is exactly the click genCw's 5 ms raised-cosine edges exist
    // to prevent. m_cwPhase stays purely as the keying-schedule clock. (#4618)
    double m_cwPhaseRad = 0.0;
    qint64 m_plPhase = 0;      // sample counter; long overflows in ~25 h on LLP64 (#4618)
    // Voice playback: the bundled speech clip's samples (mono float, 24 kHz),
    // loaded once, and the loop read position.
    std::vector<float> m_voiceSamples;
    bool   m_voiceLoaded = false;
    size_t m_voicePos = 0;
    void   loadVoiceClip();       // lazy-load :/demo_voice.wav on first use
    // Birdie phase as a CONTINUOUS RADIANS accumulator (not hz*absolute-time), so
    // changing the pitch (VFO tuning) doesn't teleport the phase and warble.
    double m_birdiePhaseRad = 0.0;
    qint64 m_hashPhase = 0, m_woodPhase = 0;  // sample counters; long overflows in ~25 h on LLP64 (#4618)

    // RNG (own stream; Box-Muller spare)
    std::uint64_t m_rng = 0x5EEDULL;
    // Display-only RNG stream (see nextDisplayUniform). Different seed so the
    // grass does not visibly mirror the audio noise.
    std::uint64_t m_displayRng = 0xD15D1A11ULL;
    bool   m_haveSpare = false;
    double m_spare = 0.0;

    // pink (Voss-McCartney) state
    static constexpr int kPinkRows = 16;
    std::array<double, kPinkRows> m_pinkRows{};
    std::uint32_t m_pinkCtr = 0, m_pinkKey = 0;

    // qrn / crashes envelope state
    double m_qrnEnv = 0.0, m_qrnSign = 1.0;
    double m_crashEnv = 0.0, m_crashLp = 0.0;

    // notches: audio-Hz + per-notch biquad state (carried across frames)
    bool   m_nbOn = false;         // noise blanker engaged
    double m_nbEnv = 0.0;          // SLOW background level for impulse detection
    int    m_nbHold = 0;           // samples left to blank (covers the impulse tail)
    std::vector<double> m_notchHz;
    std::map<long, double> m_notchWidthHz;             // round(hz) -> width
    std::map<long, std::array<double, 4>> m_notchState; // round(hz) -> x1,x2,y1,y2
};

}  // namespace AetherSDR
