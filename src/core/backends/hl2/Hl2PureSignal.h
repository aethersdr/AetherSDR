#pragma once

// PureSignal for the Hermes-Lite 2.
//
// A port of the PureSignal control loop in Thetis as MI0BOT runs it on the
// HL2 (Project Files/Source/Console/PSForm.cs, timer1code / timer2code, and
// networkproto1.c for the wire).
//
//   Ported from: https://github.com/mi0bot/OpenHPSDR-Thetis
//                (commit f9ff9f2515b7bf3e5aee5c1a155222a4f836c179)
//   Copyright (C) 2000-2025 the original Thetis authors
//   Copyright (C) 2020-2026 Richard Samphire (MW0LGE)
//   2023-26 HL2 modifications by MI0BOT
//   License: GPL-2.0-or-later, carried here as part of GPL-3.0-or-later
//   AetherSDR. See THIRD_PARTY_LICENSES entry 36.
//
// WDSP's calcc
// stage does the actual calibration and correction; this file is the part
// Thetis keeps in its GUI: which SetPSControl() to send when, and the
// auto-attenuate loop that keeps the feedback level in the window calcc wants.
//
// Two pieces:
//   Hl2PureSignalLogic  the two state machines, pure and clock-free, so
//                       hl2_puresignal_test drives them tick by tick.
//   Hl2PureSignal       a QObject on the HL2 I/O thread that owns the feed
//                       buffers, polls GetPSInfo every 100 ms, runs the logic
//                       and applies what it decides to the TXA channel and,
//                       through a sink, to MetisClient's keyed 0x0a bank.

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
    bool autoCalibrate = false;
    bool autoAttenuate = true;
    bool calibrating = false;    // a calibration is armed (auto or single)
    bool correcting = false;     // info[14]: correction applied to TX
    int feedbackLevel = 0;       // info[4]; 128..181 is the target window
    int calibrationCount = 0;    // info[5]
    int engineState = 0;         // info[15]
    int attenuationDb = 31;      // Thetis ATTOnTX, -28..31; gain = 19 - att
    double maxTx = 0.0;          // GetPSMaxTX
    bool keyed = false;
};

// The status as the "hl2" extension's "puresignal" map: what extensionStatus
// carries and what every puresignal.* verb replies with. The keys are the
// member names above, plus the attenuation range (attMin/attMax) and calcc's
// feedback window (feedbackWindowLow/High), so a client needs no HL2 header.
[[nodiscard]] QVariantMap pureSignalStatusFields(const Hl2PureSignalStatus& s);

class Hl2PureSignalLogic {
public:
    // Thetis eCMDState, in its order.
    enum class Cmd {
        Off, TurnOnAuto, Auto, TurnOnSingle, Single, StayOn, TurnOff, Restore
    };
    // Thetis eAAState.
    enum class Aa { Monitor, SetNewValues, RestoreOperation };
    // calcc.c _calcc_state.
    enum EngineState {
        LReset, LWait, LMoxDelay, LSetup, LCollect, MoxCheck, LCalc, LDelay, LStayOn, LTurnOn
    };

    struct Control {
        bool reset = false, mancal = false, automode = false, turnon = false;
        bool operator==(const Control&) const = default;
    };
    struct Info {
        int feedbackLevel = 0;
        int calibrationCount = 0;
        bool correcting = false;
        int state = LReset;
    };
    struct Output {
        std::vector<Control> controls;   // SetPSControl calls, in order
        std::optional<int> attenuationDb;
    };

    static constexpr int kAttMin = -28;   // MI0BOT: the HL2 goes negative
    static constexpr int kAttMax = 31;
    static constexpr int kFeedbackIdeal100 = 152293;   // 152.293 * 1000
    static constexpr int kSingleCalRetries = 5;

    // Thetis' checkbox and buttons. Each is a request the next tick acts on.
    void setAutoCalibrate(bool on);
    void requestSingleCalibration() { m_singleCalOn = true; }
    void requestOff() { m_off = true; m_autoOn = false; }
    void requestRestore() { m_restoreOn = true; }
    void setAutoAttenuate(bool on) { m_autoAttenuate = on; }
    void setAttenuationDb(int att) { m_att = clampAtt(att); }
    // Back to Thetis' start-up state: OFF, nothing armed.
    void resetToOff();

    // One 100 ms tick: the command machine (timer1code), then auto-attenuate
    // (timer2code), on the same info.
    Output tick(const Info& info);

    [[nodiscard]] Cmd command() const noexcept { return m_cmd; }
    [[nodiscard]] Aa autoAttenuateState() const noexcept { return m_aa; }
    [[nodiscard]] int attenuationDb() const noexcept { return m_att; }
    [[nodiscard]] bool autoCalibrate() const noexcept { return m_autoOn; }
    [[nodiscard]] bool autoAttenuate() const noexcept { return m_autoAttenuate; }
    [[nodiscard]] bool calibrating() const noexcept
    {
        return m_cmd == Cmd::Auto || m_cmd == Cmd::TurnOnAuto
            || m_cmd == Cmd::Single || m_cmd == Cmd::TurnOnSingle;
    }

    [[nodiscard]] static int clampAtt(int att) noexcept
    {
        return att < kAttMin ? kAttMin : (att > kAttMax ? kAttMax : att);
    }
    // PureSignal.NeedToRecalibrate_HL2.
    [[nodiscard]] static bool needToRecalibrate(int fb, int att) noexcept
    {
        return fb > 181 || (fb <= 128 && att > kAttMin);
    }
    [[nodiscard]] static bool feedbackInRange(int fb) noexcept { return fb > 128 && fb <= 181; }
    // timer2code's step for the HL2, rounded away from zero like Thetis 2.10.3.12.
    [[nodiscard]] static int attenuationStepDb(int fb) noexcept;
    // 0x0a C4 while keyed: ((31 - att) & 0x3F) | 0x40 is ccRxGain(19 - att).
    [[nodiscard]] static int lnaGainDbForAtt(int att) noexcept { return 19 - att; }

private:
    void runCommand(const Info& info, Output& out);
    void runAutoAttenuate(const Info& info, Output& out);

    Cmd m_cmd = Cmd::Off;
    Aa m_aa = Aa::Monitor;
    bool m_autoOn = false;
    bool m_singleCalOn = false;
    bool m_off = false;
    bool m_restoreOn = false;
    bool m_performingSingleCal = false;
    int m_singleCalRetries = 0;
    bool m_autoAttenuate = true;
    int m_att = kAttMax;            // lowest gain: auto-attenuate raises it
    int m_deltaDb = 0;
    bool m_saveAuto = false;
    bool m_saveSingle = false;
    int m_lastCalibrationCount = 0;
};

class Hl2PureSignal : public QObject {
    Q_OBJECT

public:
    // Thetis defaults for the HL2 (PSForm / clsHardwareSpecific).
    static constexpr double kHwPeak = 0.233;
    static constexpr double kMoxDelaySec = 0.2;
    static constexpr double kLoopDelaySec = 0.0;
    static constexpr double kTxDelaySec = 150e-9;
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
    [[nodiscard]] const Hl2PureSignalLogic& logic() const noexcept { return m_logic; }

public slots:
    void setEnabled(bool on);
    void setKeyed(bool keyed);
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
    void flushFeed();
    void pumpIdle(WdspChannel* ch);
    void applyAttenuation(int att);
    void publish();

    Hl2TxDsp* m_tx;
    QTimer* m_timer = nullptr;
    Hl2PureSignalLogic m_logic;
    std::function<void(int)> m_txGainSink;
    bool m_enabled = false;
    bool m_wantAuto = false;      // the operator's Auto switch
    bool m_keyed = false;
    WdspChannel::PureSignalInfo m_lastInfo;
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
