#pragma once

#include <QObject>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace AetherSDR {

// ATU tune status values (from FlexLib ATUTuneStatus enum).
enum class ATUStatus {
    None,
    NotStarted,
    InProgress,
    Bypass,
    Successful,
    OK,
    FailBypass,
    Fail,
    Aborted,
    ManualBypass
};

// State model for the radio's transmit parameters and internal ATU.
//
// Transmit status arrives via TCP as "transmit rfpower=93 tunepower=38 ..."
// after "sub tx all".  ATU status arrives as "atu status=TUNE_SUCCESSFUL ...".
//
// Commands use "transmit set ..." for power, "transmit tune <0|1>" for tune,
// "xmit <0|1>" for MOX, and "atu ..." for ATU control.
class TransmitModel : public QObject {
    Q_OBJECT

public:
    explicit TransmitModel(QObject* parent = nullptr);

    // ── Transmit getters ────────────────────────────────────────────────────
    int     rfPower()       const { return m_rfPower; }
    int     tunePower()     const { return m_tunePower; }
    bool    isTuning()      const { return m_tune; }
    bool    isMox()         const { return m_mox; }
    bool    isTransmitting() const { return m_transmitting; }
    double  transmitFreq()  const { return m_transmitFreq; }  // MHz, from "transmit freq=..."
    void    setTransmitting(bool tx);

    // ── Mic / monitor / processor getters ─────────────────────────────────
    QString micSelection()          const { return m_micSelection; }
    int     micLevel()              const { return m_micLevel; }
    bool    micAcc()                const { return m_micAcc; }
    bool    speechProcessorEnable() const { return m_speechProcEnable; }
    int     speechProcessorLevel()  const { return m_speechProcLevel; }
    bool    companderOn()           const { return m_companderOn; }
    int     companderLevel()        const { return m_companderLevel; }
    bool    daxOn()                 const { return m_daxOn; }
    bool    sbMonitor()             const { return m_sbMonitor; }
    int     monGainSb()             const { return m_monGainSb; }

    // ── VOX getters ───────────────────────────────────────────────────────
    bool    voxEnable()     const { return m_voxEnable; }
    int     voxLevel()      const { return m_voxLevel; }
    int     voxDelay()      const { return m_voxDelay; }
    bool    micBoost()      const { return m_micBoost; }
    bool    micBias()       const { return m_micBias; }
    bool    metInRx()       const { return m_metInRx; }
    bool    syncCwx()       const { return m_syncCwx; }
    int     amCarrierLevel() const { return m_amCarrierLevel; }
    bool    dexpOn()         const { return m_dexpOn; }
    int     dexpLevel()      const { return m_dexpLevel; }
    int     txFilterLow()    const { return m_txFilterLow; }
    int     txFilterHigh()   const { return m_txFilterHigh; }

    // ── CW getters ──────────────────────────────────────────────────────
    int     cwSpeed()       const { return m_cwSpeed; }
    int     cwPitch()       const { return m_cwPitch; }
    bool    cwBreakIn()     const { return m_cwBreakIn; }
    int     cwDelay()       const { return m_cwDelay; }
    bool    cwSidetone()    const { return m_cwSidetone; }
    bool    cwIambic()      const { return m_cwIambic; }
    int     cwIambicMode()  const { return m_cwIambicMode; }  // 0=A, 1=B
    bool    cwSwapPaddles() const { return m_cwSwapPaddles; }
    bool    cwlEnabled()    const { return m_cwlEnabled; }
    int     monGainCw()     const { return m_monGainCw; }
    int     monPanCw()      const { return m_monPanCw; }

    // ── Interlock / TX settings getters ──────────────────────────────────────
    int     accTxDelay()     const { return m_accTxDelay; }
    int     tx1Delay()       const { return m_tx1Delay; }
    int     tx2Delay()       const { return m_tx2Delay; }
    int     tx3Delay()       const { return m_tx3Delay; }
    int     txDelay()        const { return m_txDelay; }
    int     interlockTimeout() const { return m_interlockTimeout; }
    int     accTxReqPolarity() const { return m_accTxReqPolarity; }
    int     rcaTxReqPolarity() const { return m_rcaTxReqPolarity; }
    int     maxPowerLevel()  const { return m_maxPowerLevel; }
    void    setMaxPowerLevel(int w) { if (m_maxPowerLevel != w) { m_maxPowerLevel = w; emit maxPowerLevelChanged(w); } }
    QString tuneMode()       const { return m_tuneMode; }
    bool    showTxInWaterfall() const { return m_showTxInWaterfall; }

    // ── APD getters ─────────────────────────────────────────────────────────
    bool    apdEnabled()        const { return m_apdEnabled; }
    bool    apdConfigurable()   const { return m_apdConfigurable; }
    bool    apdEqualizerActive()const { return m_apdEqActive; }

    // External APD per-TX-antenna sampler-port assignment (SmartSDR 4.2.18+).
    struct ApdSampler {
        QString     selected{"INTERNAL"};
        QStringList available{"INTERNAL"};
    };
    ApdSampler apdSampler(const QString& txAnt) const { return m_apdSamplers.value(txAnt); }
    // Reset all state to defaults on disconnect — different radio models
    // have different capabilities (APD, max power, pan count, etc.)
    void resetState();

    // ── ATU getters ─────────────────────────────────────────────────────────
    bool      atuEnabled()      const { return m_atuEnabled; }
    ATUStatus atuStatus()       const { return m_atuStatus; }
    bool      memoriesEnabled() const { return m_memoriesEnabled; }
    bool      usingMemory()     const { return m_usingMemory; }

    // ── Profile getters ─────────────────────────────────────────────────────
    QStringList profileList()       const { return m_profileList; }
    QString     activeProfile()     const { return m_activeProfile; }
    QStringList micProfileList()    const { return m_micProfileList; }
    QString     activeMicProfile()  const { return m_activeMicProfile; }
    QStringList micInputList()      const { return m_micInputList; }

    // ── Status parsing (called from RadioModel) ─────────────────────────────
    void applyTransmitStatus(const QMap<QString, QString>& kvs);
    void applyInterlockStatus(const QMap<QString, QString>& kvs);
    void applyAtuStatus(const QMap<QString, QString>& kvs);
    void applyApdStatus(const QMap<QString, QString>& kvs);
    void applyApdSamplerStatus(const QMap<QString, QString>& kvs);
    void setProfileList(const QStringList& profiles);
    void setActiveProfile(const QString& profile);
    void setMicProfileList(const QStringList& profiles);
    void setActiveMicProfile(const QString& profile);
    void setMicInputList(const QStringList& inputs);

    // PTT request coordinator (#2262) — single entry point for "user
    // wants to key/unkey".  When Quindar is enabled and the active TX
    // slice is on a phone mode, runs the engage/disengage tone state
    // machine before the actual MOX flip; otherwise forwards directly
    // to setMox().  All UI and TCI hardware-PTT callers should use this
    // path so Quindar tones happen consistently regardless of source.
    enum class PttSource : uint8_t {
        Mox          = 0,   // GUI/local MOX or PTT
        TciHardware  = 1,   // TCI radio-direct PTT (e.g. Stream Deck plugin)
        Footswitch   = 2,   // future: serial-PTT or other hardware path
        Tune         = 3,   // local TUNE/two-tone carrier
        Dax          = 4,   // external digital-audio PTT path
    };

    // ── Command methods (emit commandReady) ─────────────────────────────────
    void setRfPower(int power);
    void setTunePower(int power);
    void setTuneMode(const QString& mode);
    void startTune(PttSource source = PttSource::Tune);
    void startTwoToneTune(PttSource source = PttSource::Tune);
    void toggleTwoToneTune();
    void stopTune();
    void setMox(bool on);

    void requestPttOn(PttSource source);
    void requestPttOff(PttSource source);
    // Bindings injected once at MainWindow wire-up.  The coordinator
    // needs the Quindar DSP module pointer to drive intro/outro
    // phases, and a callable that returns the active TX slice's mode
    // string for the phone-mode gate.  The mode-getter indirection
    // keeps TransmitModel decoupled from RadioModel/SliceModel so the
    // test executables don't need Qt6::Network linkage.
    void setQuindarTone(class ClientQuindarTone* tone);
    using TxModeGetter = std::function<QString()>;
    void setTxModeGetter(TxModeGetter getter);
    using PttPreflight = std::function<QString(PttSource)>;
    void setPttPreflight(PttPreflight preflight);

    void atuStart();
    void atuBypass();
    void setAtuMemories(bool on);
    void loadProfile(const QString& name);
    void setApdEnabled(bool on);
    void setApdSamplerPort(const QString& txAnt, const QString& port);
    void resetApdEqualizer();

    // ── Mic / monitor / processor commands ────────────────────────────────
    void setMicSelection(const QString& input);
    void setMicLevel(int level);
    void setMicAcc(bool on);
    void setSpeechProcessorEnable(bool on);
    void setSpeechProcessorLevel(int level);
    void setDax(bool on);
    void setSbMonitor(bool on);
    void setMonGainSb(int gain);
    void loadMicProfile(const QString& name);

    // ── VOX commands ────────────────────────────────────────────────────────
    void setVoxEnable(bool on);
    void setVoxLevel(int level);
    void setVoxDelay(int delay);
    void setMicBoost(bool on);
    void setMicBias(bool on);
    void setAmCarrierLevel(int level);
    void setDexp(bool on);
    void setDexpLevel(int level);
    void setTxFilterLow(int hz);
    void setTxFilterHigh(int hz);

    // ── CW commands ─────────────────────────────────────────────────────────
    void setCwSpeed(int wpm);
    void setCwPitch(int hz);
    void setCwBreakIn(bool on);
    void setCwDelay(int ms);
    void setCwSidetone(bool on);
    void setCwIambic(bool on);
    void setCwIambicMode(int mode);   // 0=A, 1=B
    void setCwSwapPaddles(bool on);
    void setCwlEnabled(bool on);
    void setMonGainCw(int gain);
    void setMonPanCw(int pan);

signals:
    void stateChanged();
    void tuneChanged(bool tuning);
    void moxChanged(bool mox);
    void atuStateChanged();
    void profileListChanged();
    void micStateChanged();
    void micProfileListChanged();
    void micInputListChanged();
    void phoneStateChanged();       // VOX or CW property changed
    // Fires only when txFilterLow / txFilterHigh actually change.  Use this
    // instead of phoneStateChanged for slot work that should NOT run on
    // every VOX/CW/dexp/mic-boost/etc. status update.
    void txFilterCutoffChanged(int lowHz, int highHz);
    void apdStateChanged();
    void apdSamplerChanged(const QString& txAnt);
    void apdEqualizerResetReceived();
    void maxPowerLevelChanged(int maxWatts);
    void commandReady(const QString& cmd);
    void pttBlocked(const QString& message);
    // Quindar active-phase signal (#2262).  Emitted on the GUI thread
    // immediately when intro/outro starts and again when each finishes,
    // sized from the tone's current duration.  Used by the strip's
    // QUIN chip to flash bright while a tone is playing — replaces an
    // earlier 30 Hz poll of ClientQuindarTone::phase().
    void quindarActiveChanged(bool active);

private:
    static ATUStatus parseAtuTuneStatus(const QString& s);
    bool isPhoneModeForQuindar() const;
    bool runPttPreflight(PttSource source, bool resyncMoxOnBlock = true);
    void cancelPendingQuindarOff();

    // PTT coordinator state (#2262)
    class ClientQuindarTone* m_quindarTone{nullptr};
    TxModeGetter             m_txModeGetter;
    PttPreflight             m_pttPreflight;
    QTimer*                  m_pendingMoxOffTimer{nullptr};
    bool                     m_quindarOutroInFlight{false};

    // APD state
    bool m_apdEnabled{false};
    bool m_apdConfigurable{false};
    bool m_apdEqActive{false};
    QHash<QString, ApdSampler> m_apdSamplers;  // keyed by ANT1/ANT2/XVTA/XVTB

    // Transmit state
    int    m_rfPower{100};
    int    m_tunePower{10};
    bool   m_tune{false};
    bool   m_mox{false};
    double m_transmitFreq{0.0};   // MHz — last reported "transmit freq=..."
    bool m_transmitting{false};

    // Mic / monitor / processor state
    QString m_micSelection{"MIC"};
    int     m_micLevel{50};
    bool    m_micAcc{false};
    bool    m_speechProcEnable{false};
    int     m_speechProcLevel{0};
    bool    m_companderOn{false};
    int     m_companderLevel{0};
    bool    m_daxOn{false};
    bool    m_sbMonitor{false};
    int     m_monGainSb{50};

    // VOX / phone state
    bool m_voxEnable{false};
    int  m_voxLevel{50};
    int  m_voxDelay{50};      // raw 0–100, actual ms = value × 20
    bool m_micBoost{false};
    bool m_micBias{false};
    bool m_metInRx{false};
    bool m_syncCwx{true};
    int  m_amCarrierLevel{48};  // 0–100
    bool m_dexpOn{false};       // downward expander (noise gate)
    int  m_dexpLevel{0};        // noise gate level (0–100)
    int  m_txFilterLow{50};     // TX filter low cut (Hz)
    int  m_txFilterHigh{3300};  // TX filter high cut (Hz)

    // CW state
    int  m_cwSpeed{20};       // 5–100 WPM
    int  m_cwPitch{600};      // 100–6000 Hz
    bool m_cwBreakIn{false};
    int  m_cwDelay{500};      // 0–2000 ms
    bool m_cwSidetone{true};
    bool m_cwIambic{true};
    int  m_cwIambicMode{0};   // 0=A, 1=B
    bool m_cwSwapPaddles{false};
    bool m_cwlEnabled{false};
    int  m_monGainCw{50};
    int  m_monPanCw{50};

    // Interlock / TX settings
    int     m_accTxDelay{0};
    int     m_tx1Delay{0};
    int     m_tx2Delay{0};
    int     m_tx3Delay{0};
    int     m_txDelay{0};
    int     m_interlockTimeout{0};
    int     m_accTxReqPolarity{0};
    int     m_rcaTxReqPolarity{0};
    int     m_maxPowerLevel{100};
    QString m_tuneMode{"single_tone"};
    bool    m_showTxInWaterfall{false};

    // ATU state
    bool      m_atuEnabled{false};
    ATUStatus m_atuStatus{ATUStatus::None};
    bool      m_memoriesEnabled{false};
    bool      m_usingMemory{false};

    // TX profiles
    QStringList m_profileList;
    QString     m_activeProfile;

    // Mic profiles
    QStringList m_micProfileList;
    QString     m_activeMicProfile;
    QStringList m_micInputList;
};

} // namespace AetherSDR
