#pragma once

// PureSignal for the Hermes-Lite 2.
//
// WDSP's calcc stage does the calibration and applies the correction. It
// exposes one control call, SetPSControl(reset, mancal, automode, turnon),
// and one report, GetPSInfo(). What calcc does not decide is when to call
// which, and whether the HL2's feedback gain is right for the calibration it
// is collecting. That is this file.
//
// Three pieces:
//   Hl2PsGain        the HL2's feedback-gain range and the step that moves the
//                    feedback level towards calcc's window.
//   Hl2PsController  the operator's choice (Off, Auto, Single, Restore) turned
//                    into SetPSControl calls, judged against each GetPSInfo
//                    report. Pure and clock-free, so hl2_puresignal_test drives
//                    it report by report.
//   Hl2PureSignal    a QObject on the HL2 I/O thread that owns the feed
//                    buffers, reads GetPSInfo every 100 ms, steps the
//                    controller and applies what it returns to the TXA channel
//                    and, through a sink, to MetisClient's keyed 0x0a bank.
//
// The calcc behaviour the controller leans on (third_party/wdsp calcc.c):
//   - SetPSControl writes all four flags at once.
//   - LRESET clears `reset` and drops the correction unless `turnon` is set;
//     it moves on to LTURNON if `turnon`, else to LWAIT if `automode` or
//     `mancal`, else it stays.
//   - LWAIT clears `mancal`, so a single calibration is a one-shot.
//   - A calibration that succeeds ends in LDELAY, then LSETUP again with
//     `automode`, else LSTAYON (the correction held).
//   - Three failed calibrations in a row, or a collection that cannot fill its
//     top amplitude bucket, end in LRESET.
//   - LSTAYON leaves only for LRESET, on any of `reset`, `automode`, `mancal`.
//   - info[0..7] are copied once per finished calc(): info[7] counts the
//     attempts, info[4] is the feedback level, and info[0] bit 0 says the
//     level was not measured this time (it keeps the previous value).
//   - pscc() is the only place the state machine moves.

#include "core/dsp/WdspChannel.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <complex>
#include <functional>
#include <optional>
#include <vector>

class QTimer;

namespace AetherSDR::hl2 {

class Hl2TxDsp;

// What the GUI sees. Plain values, copied across the thread boundary.
struct Hl2PureSignalStatus {
    bool available = false;      // board, rate and receiver count allow it
    QString unavailableReason;   // why not, when !available
    bool enabled = false;
    bool autoCalibrate = false;  // the operator's Auto switch
    bool autoAttenuate = true;
    bool calibrating = false;    // Auto or Single is running
    bool singleCalibrating = false;  // Single is running, retries included
    bool correcting = false;     // info[14]: correction applied to TX
    int feedbackLevel = 0;       // info[4]; 128 < fb <= 181 is the target window
    int calibrationCount = 0;    // info[5]: successful calibrations
    int engineState = 0;         // info[15]: calcc's state
    int attenuationDb = 31;      // keyed feedback attenuation; gain = 19 - att
    int feedbackLimit = 0;       // Hl2PsGain::limit at the att it was measured at;
                                 // +2 is laid over it from Hl2PsOverload
    double maxTx = 0.0;          // GetPSMaxTX
    bool keyed = false;
};

// The status as the "hl2" extension's "puresignal" map: what extensionStatus
// carries and what every puresignal.* verb replies with. The keys are the
// member names above, plus the attenuation range (attMin/attMax) and calcc's
// feedback window (feedbackWindowLow/High), so a client needs no HL2 header.
[[nodiscard]] QVariantMap pureSignalStatusFields(const Hl2PureSignalStatus& s);

// The feedback path's gain on the HL2, and how far to move it.
//
// While keyed, the AD9866 LNA in front of the feedback DDC runs at
// 19 - att dB. att 31 gives -12 dB, the bottom of the LNA's -12..+48 dB range;
// att -28 gives +47 dB. Below 0, att is extra gain for a weak coupler.
//
// calcc reports the feedback level on a scale where 256 is the configured
// hardware peak (kHwPeak). It calibrates best with the level between half
// that peak (128, -6 dB) and 1/sqrt(2) of it (181, -3 dB).
struct Hl2PsGain {
    static constexpr int kAttMin = -28;
    static constexpr int kAttMax = 31;
    static constexpr int kWindowLow = 128;    // exclusive
    static constexpr int kWindowHigh = 181;   // inclusive
    static constexpr int kScale = 256;
    // Largest single move. A level that is far out is often a bad measurement
    // (a short over, a hot spot); one step of at most this much, then the
    // next attempt measures again.
    static constexpr int kMaxStepDb = 10;

    [[nodiscard]] static int clampAtt(int att) noexcept
    {
        return att < kAttMin ? kAttMin : (att > kAttMax ? kAttMax : att);
    }
    [[nodiscard]] static bool inWindow(int fb) noexcept
    {
        return fb > kWindowLow && fb <= kWindowHigh;
    }
    // dB to add to att so `fb` lands on the window's geometric centre,
    // sqrt(128 * 181) = 152.2: rounded, at most kMaxStepDb either way. No
    // level (fb <= 0) needs all the gain one step gives; a level above the
    // scale is clipped, not measured, and needs as much less.
    [[nodiscard]] static int stepDb(int fb) noexcept;
    // Whether the attenuation range has run out with the level still outside
    // the window. +1: the most attenuation the HL2 has and the feedback is
    // still above the window: too strong to calibrate, and only an attenuator
    // in the feedback path (e.g. after an amplifier's sampler) brings it in.
    // -1: full gain and a measured level still too weak (coupler or sampler
    // missing or too loose). 0: within range. fb <= 0 is no measurement yet,
    // so it never reads as too weak. A level above kScale is still a
    // measurement (calcc extrapolates it), so it is +1 as well; whether the
    // receive input is overdriven is the gateware's ADC overload report, see
    // Hl2PsOverload.
    [[nodiscard]] static int limit(int att, int fb) noexcept
    {
        if (att >= kAttMax && fb > kWindowHigh) { return 1; }
        if (att <= kAttMin && fb > 0 && fb <= kWindowLow) { return -1; }
        return 0;
    }
    // 0x0a C4 while keyed: ccRxGain(19 - att).
    [[nodiscard]] static int lnaGainDbForAtt(int att) noexcept { return 19 - att; }
};

// The attenuation calcc's feedback level was measured at. info[4] keeps the
// last attempt's level until the next attempt finishes, so right after a gain
// move, and for as long as Hold lasts after a manual one, it describes another
// attenuation. limit() is only answered for a level measured at the
// attenuation in force now; otherwise 0 until a fresh attempt (#7087 review).
class Hl2PsMeasuredAt {
public:
    // One GetPSInfo read. attNow is the attenuation in force while that
    // attempt ran, i.e. before this tick's controller step moves it.
    // The first read after reset() is a baseline, not an attempt: calcc's
    // info[] survives PureSignal off (LRESET leaves it), so that read can
    // carry the previous session's level. No real attempt finishes within the
    // first tick after an enable; calcc's MOX delay alone is 200 ms.
    void observe(bool feedbackFresh, int attempts, int attNow) noexcept
    {
        if (feedbackFresh && m_attempts && attempts != *m_attempts) { m_att = attNow; }
        m_attempts = attempts;
    }
    [[nodiscard]] int limit(int attNow, int fb) const noexcept
    {
        return m_att && *m_att == attNow ? Hl2PsGain::limit(attNow, fb) : 0;
    }
    void reset() noexcept
    {
        m_att.reset();
        m_attempts.reset();
    }

private:
    std::optional<int> m_att;
    std::optional<int> m_attempts;   // empty: the next read is a baseline
};

// The red "receive input overdriven" state: the HL2's own ADC overload report
// (DATA[24], &clip_cnt: set means at least 3 clip events in the interval)
// while PureSignal is on and the radio is keyed, when the converter sees the
// feedback. Only a telemetry window that was keyed from start to end decides
// it, so a strong signal on receive just before the key does not count. It
// holds through unkey, so the operator can still read it, and clears on the
// next keyed window without overload, or when PureSignal goes off.
struct Hl2PsOverload {
    [[nodiscard]] static bool next(bool current, bool psEnabled, bool keyedWholeWindow,
                                   bool overloadInWindow) noexcept
    {
        if (!psEnabled) { return false; }
        return keyedWholeWindow ? overloadInWindow : current;
    }
};

class Hl2PsController {
public:
    enum class Mode {
        Off,        // calcc held in LRESET, no correction
        Auto,       // calcc calibrates over after over
        Single,     // one calibration, then Hold
        Hold,       // a correction applied and kept (LSTAYON)
        Restoring,  // a saved correction loading, then Hold
    };
    // calcc.c _calcc_state, in its order.
    enum EngineState {
        LReset, LWait, LMoxDelay, LSetup, LCollect, MoxCheck, LCalc, LDelay, LStayOn, LTurnOn
    };

    struct Flags {   // one SetPSControl call
        bool reset = false, mancal = false, automode = false, turnon = false;
        bool operator==(const Flags&) const = default;
    };
    struct Report {  // one GetPSInfo read
        int state = LReset;
        bool correcting = false;
        int feedbackLevel = 0;
        bool feedbackFresh = false;
        int attempts = 0;
    };
    struct Step {    // what to apply after a report
        std::optional<Flags> flags;
        std::optional<int> attenuationDb;
    };

    // A Single that calcc gives up on is started this many times in all,
    // then the controller goes to Off rather than loop on a bad setup.
    static constexpr int kSingleAttempts = 3;
    // Gain moves one Single may make. Each one restarts the calibration; a
    // level that keeps swinging across the window would otherwise never let
    // it finish.
    static constexpr int kSingleGainMoves = 5;

    // Each selection starts calcc from LRESET with the flags for that mode,
    // sent on the next step().
    void selectOff();
    void selectAuto();
    void selectSingle();
    // After PSRestoreCorr(), which sets calcc's `turnon` itself. The flags
    // sent again here clear a `mancal` or `automode` left from before, which
    // would otherwise take calcc straight out of LSTAYON.
    void selectRestoring();
    // A new TXA channel: calcc starts in LRESET with nothing set. The
    // attenuation is the radio's and is kept.
    void reset();

    void setAutoAttenuate(bool on) { m_autoAttenuate = on; }
    void setAttenuationDb(int att) { m_att = Hl2PsGain::clampAtt(att); }

    // One GetPSInfo report. Flags still waiting go out first, and the report
    // is not judged: it was read before calcc saw them.
    Step step(const Report& r);

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] int attenuationDb() const noexcept { return m_att; }
    [[nodiscard]] bool autoAttenuate() const noexcept { return m_autoAttenuate; }
    [[nodiscard]] bool calibrating() const noexcept
    {
        return m_mode == Mode::Auto || m_mode == Mode::Single;
    }

private:
    void select(Mode m);
    [[nodiscard]] static Flags flagsFor(Mode m) noexcept;
    [[nodiscard]] std::optional<int> gainMove(const Report& r);

    Mode m_mode = Mode::Off;
    std::optional<Flags> m_pending;
    bool m_autoAttenuate = true;
    int m_att = Hl2PsGain::kAttMax;   // lowest gain: auto-attenuate raises it
    std::optional<int> m_lastAttempts;
    bool m_sawActive = false;         // Single: calcc has left LRESET
    bool m_collected = false;         // calcc has collected since the last flags
    int m_singleStarts = 0;
    int m_singleGainMoves = 0;
};

class Hl2PureSignal : public QObject {
    Q_OBJECT

public:
    // The HL2's TX DAC reference peak on DDC3 at full drive: the level calcc
    // scales its feedback against (256 on the feedback scale).
    static constexpr double kHwPeak = 0.233;
    // How long calcc waits after the key goes down, or after a reset, before
    // it collects: the PA and the feedback gain written on the MOX edge
    // settle well within it.
    static constexpr double kMoxDelaySec = 0.2;
    // No extra wait between calibrations in Auto.
    static constexpr double kLoopDelaySec = 0.0;
    // The HL2's feedback DDC lags its TX reference DDC by this much; calcc
    // delays the reference to line the pairs up.
    static constexpr double kTxDelaySec = 150e-9;
    // How often GetPSInfo is read. A calibration takes seconds, so a tenth of
    // a second follows it closely and costs little.
    static constexpr int kTickMs = 100;
    static constexpr std::size_t kFeedChunk = 512;   // complex samples per pscc()

    // `tx` must live on this object's thread and outlive it.
    explicit Hl2PureSignal(Hl2TxDsp* tx, QObject* parent = nullptr);
    ~Hl2PureSignal() override;

    // Where the keyed feedback gain goes: MetisClient::setPureSignalTxGainDb
    // on the same thread. Set once, before the first tick.
    void setTxGainSink(std::function<void(int db)> sink) { m_txGainSink = std::move(sink); }

    // I/O thread, from the EP6 fan-out: DDC3 (TX DAC reference) and DDC2
    // (feedback) of the same datagram. Ignored unless enabled and keyed.
    void feed(const std::vector<std::complex<float>>& txRef,
              const std::vector<std::complex<float>>& feedback);

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }

public slots:
    void setEnabled(bool on);
    void setKeyed(bool keyed);
    // calcc is told the rate the feedback DDCs actually run at (the receive
    // rate), so its MOX and TX delays convert to the right sample counts.
    void setFeedbackRateHz(int hz);
    void setAutoCalibrate(bool on);
    void calibrateOnce();
    void resetCorrection();
    void setAutoAttenuate(bool on);
    void setAttenuationDb(int att);
    void saveCorrection(const QString& path);
    void restoreCorrection(const QString& path);
    void setAvailability(bool available, const QString& reason);

signals:
    void statusChanged(const AetherSDR::hl2::Hl2PureSignalStatus& status);

private:
    void onTick();
    WdspChannel* channel();
    void seedChannel(WdspChannel* ch);
    void startController();
    void flushFeed();
    void pumpIdle(WdspChannel* ch);
    void applyAttenuation(int att);
    void publish();

    Hl2TxDsp* m_tx;
    QTimer* m_timer = nullptr;
    Hl2PsController m_ctl;
    std::function<void(int)> m_txGainSink;
    bool m_enabled = false;
    // The operator's Auto switch. Single, Reset and Restore turn it off: each
    // replaces continuous calibration with a correction the operator chose,
    // and a rebuilt channel or the next enable must not start Auto over it.
    bool m_wantAuto = false;
    bool m_keyed = false;
    WdspChannel::PureSignalInfo m_lastInfo;
    Hl2PsMeasuredAt m_measured;   // which attenuation m_lastInfo's level belongs to
    bool m_available = false;
    QString m_unavailableReason;
    int m_feedbackRateHz = 48000;
    unsigned long long m_seededGeneration = 0;
    std::vector<double> m_txBuf;
    std::vector<double> m_rxBuf;
    std::size_t m_fill = 0;
    Hl2PureSignalStatus m_last;
    bool m_havePublished = false;
};

}  // namespace AetherSDR::hl2

Q_DECLARE_METATYPE(AetherSDR::hl2::Hl2PureSignalStatus)
