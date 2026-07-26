#pragma once

#include <QMetaType>

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

// Owns one complete WDSP channel and hides WDSP's process-global numeric
// channel table. Construction, reconfiguration, and filter changes are control-
// thread operations; processIq() is the allocation-free real-time operation.
class WdspChannel final
{
public:
    enum class Direction
    {
        Receive,
        Transmit
    };

    enum class Mode
    {
        Lsb,
        Usb,
        Dsb,
        Cwl,
        Cwu,
        Fm,
        Am,
        Digu,
        Spec,
        Digl,
        Sam,
        Drm,
        Wbfm
    };

    struct Config
    {
        Direction direction = Direction::Receive;
        std::size_t inputBlockSize = 1024;
        std::size_t dspBlockSize = 1024;
        int inputSampleRate = 48000;
        int dspSampleRate = 48000;
        int outputSampleRate = 48000;
        Mode mode = Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        int agcMode = 3;
        double maximumAgcGainDb = 120.0;
        // Output level difference between very weak and very strong signals.
        // WDSP defaults this to 0, which is TOTAL compression — every signal
        // and the noise floor between them come out at the same level, so the
        // ceiling is applied to noise and the result clips. pihpsdr sets 35.
        int agcSlopeDb = 35;
        // Gain applied when the AGC is OFF. Never setting it leaves WDSP's
        // default, which is why "AGC off" was the loudest and worst-clipping
        // setting of all rather than the quietest.
        double agcFixedGainDb = 10.0;
        // Channel mute envelope, in seconds. WDSP applies these when a channel
        // starts and stops, and they are the anti-click mechanism: an abrupt DSP
        // mute clicks on every transition, which on a full-duplex radio means
        // every T/R change. Values match both reference clients (Thetis
        // cmaster.c, pihpsdr receiver.c) — leaving them at zero, as this did,
        // disables the ramp entirely and is invisible until you go hunting for
        // the click.
        double muteDelayUpSec = 0.010;
        double muteSlewUpSec = 0.025;
        double muteDelayDownSec = 0.000;
        double muteSlewDownSec = 0.010;
        // Bandpass filter length and phase mode — the selectivity/latency
        // trade. More coefficients sharpen the skirt and add delay;
        // minimum-phase trades linear phase for lower latency. 2048 is WDSP's
        // own default (max(2048, dsp_size)), so this changes nothing on its own
        // — it makes the value explicit and tunable instead of implicit.
        // pihpsdr runs 8192 by comparison.
        int filterTaps = 2048;
        bool minimumPhase = false;
        bool blockForOutput = false;
    };

    enum class ProcessResult
    {
        Ok,
        Underrun,
        Busy,
        InvalidBuffer,
        AllocationViolation,
        EngineError
    };

    static std::unique_ptr<WdspChannel> create(const Config& config,
                                               std::string* error = nullptr) noexcept;

    ~WdspChannel();

    WdspChannel(const WdspChannel&) = delete;
    WdspChannel& operator=(const WdspChannel&) = delete;
    WdspChannel(WdspChannel&&) = delete;
    WdspChannel& operator=(WdspChannel&&) = delete;

    ProcessResult processIq(std::span<const float> inputI,
                            std::span<const float> inputQ,
                            std::span<float> outputLeft,
                            std::span<float> outputRight) noexcept;

    // The caller must stop feeding processIq() before a control operation.
    bool reconfigure(const Config& config, std::string* error = nullptr) noexcept;
    bool setMode(Mode mode) noexcept;
    bool setFilter(double lowHz, double highHz) noexcept;
    // Runtime RX AGC change. agcMode is the WDSP RXA AGC mode (0 off, 1 long,
    // 2 slow, 3 medium, 4 fast); maximumGainDb is the AGC "top", the ceiling on
    // how much gain the AGC may apply. Receive channels only — returns false on
    // a transmit channel, on a non-finite ceiling, or if a control operation is
    // already in flight. Control-path work, guarded exactly like setMode(); it
    // must not be called from the processIq() callback.
    bool setAgc(int agcMode, double maximumGainDb) noexcept;
    // RX frequency shift in Hz, relative to the tuned (NCO) frequency. A
    // single-DDC backend uses this to move the slice inside the passband
    // without moving the NCO, which is what keeps the panadapter still while
    // the operator tunes. 0 disables the stage. Receive channels only.
    bool setShift(double shiftHz) noexcept;
    // WDSP meter readout in dBFS-relative units. Meter is the RXA meter type
    // (0 = S peak, 1 = S average, 2 = ADC peak, 3 = ADC average, 4 = AGC gain).
    // Read-only and cheap — safe to call from a timer. Returns a large negative
    // value on a transmit channel, which has no RXA meters.
    enum class Meter { SignalPeak = 0, SignalAverage = 1,
                       AdcPeak = 2, AdcAverage = 3, AgcGain = 4 };
    [[nodiscard]] double meter(Meter which) const noexcept;

    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] std::size_t outputBlockSize() const noexcept;
    [[nodiscard]] int channelIdForTest() const noexcept { return m_channelId; }

    static uint64_t allocationSequenceForTest() noexcept;
    static uint64_t outstandingAllocationsForTest() noexcept;

private:
    explicit WdspChannel(int channelId, const Config& config) noexcept;

    static bool validateConfig(const Config& config, std::string* error) noexcept;
    static int wdspMode(Mode mode) noexcept;
    static std::size_t computeOutputBlockSize(const Config& config) noexcept;

    void open() noexcept;
    void close() noexcept;
    bool beginControlOperation() noexcept;
    void endControlOperation() noexcept;

    int m_channelId = -1;
    Config m_config;
    // Fixed for a given Config; cached at open()/reconfigure() so the real-time
    // processIq() buffer-size check does not repeat a divide every block.
    std::size_t m_outputBlockSize = 0;
    double m_shiftHz = 0.0;
    // These two coordinate the real-time processIq() against control-thread
    // operations. The handshake is Dekker-style — each side stores its own flag
    // then reads the other's — which is only correct under sequential
    // consistency, so every access below uses memory_order_seq_cst. Do NOT relax
    // these to acquire/release: acq_rel does not order a store then a load of a
    // different atomic across threads, and both sides could then proceed at once.
    std::atomic<unsigned> m_callbacksInFlight {0};
    std::atomic<bool> m_controlOperation {false};
    bool m_open = false;
};

// Mode crosses a thread boundary as a queued Q_ARG (Hl2Backend marshals control
// verbs onto its I/O thread). An unregistered type there does not fail loudly --
// invokeMethod just warns and DROPS the call, which would silently break mode
// switching.
Q_DECLARE_METATYPE(WdspChannel::Mode)
