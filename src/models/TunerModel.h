#pragma once

#include <QObject>
#include <QMap>
#include <QString>

#include "core/backends/TunerDelta.h"

namespace AetherSDR {

class TgxlConnection;

// State model for a 4o3a Tuner Genius XL (TGXL) connected via the FlexRadio.
//
// Status arrives via TCP as "atu <handle> key=val ..." after "sub atu all".
// Commands use the "tgxl" prefix:
//   tgxl set handle=<H> mode=<0|1>       — operate/standby
//   tgxl set handle=<H> bypass=<0|1>     — bypass on/off
//   tgxl autotune handle=<H>             — initiate auto-tune
//
// Direct TGXL connection (port 9010) enables manual Pi network relay control:
//   tune relay=<0|1|2> move=<+1|-1>      — adjust C1/L/C2 one step
class TunerModel : public QObject {
    Q_OBJECT

public:
    explicit TunerModel(QObject* parent = nullptr);

    // Getters
    QString handle()    const { return m_handle; }
    QString modelName() const { return m_model; }
    QString serialNum() const { return m_serialNum; }
    QString tgxlIp()    const { return m_tgxlIp; }
    bool    isOperate() const { return m_operate; }
    bool    isBypass()  const { return m_bypass; }
    bool    isTuning()  const { return m_tuning; }
    int     relayC1()   const { return m_relayC1; }
    int     relayL()    const { return m_relayL; }
    int     relayC2()   const { return m_relayC2; }
    int     antennaA()  const { return m_antennaA; }  // 0-indexed: 0=ANT1, 1=ANT2, 2=ANT3, -1=unknown
    float   fwdPower()  const { return m_fwdPower; }  // forward power in watts (from direct TGXL status)
    float   swr()       const { return m_swr; }       // SWR ratio (from direct TGXL status)
    // True for the TGXL 3x1 (antenna-switch) model. Reported two ways: the
    // radio-relayed status calls it "one_by_three", the direct info reply
    // calls it "3way". Either is proof of the switch.
    bool    hasAntennaSwitch() const { return m_oneByThree || m_threeWay; }
    // True once the radio has relayed a TGXL handle. Without one the operate /
    // bypass relay commands have no address to go to, so the UI offers only the
    // states the direct port-9010 channel can reach.
    bool    hasRadioRelay() const { return !m_handle.isEmpty(); }
    bool    isPresent() const { return !m_handle.isEmpty() || m_directPresence; }
    bool    hasDirectConnection() const;

    // Apply a normalized tuner delta decoded by the backend
    // (IRadioBackend::tunerChanged). Change-gated; emits tuningChanged /
    // antennaAChanged on their edges and stateChanged once if anything moved.
    void applyChanges(const TunerDelta& delta);

    // Set the tuner handle (extracted from the status object name).
    void setHandle(const QString& handle);

    // Direct TGXL connection for manual relay control (#469)
    void setDirectConnection(TgxlConnection* conn);

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2; `steps` is a signed,
    // relative step count (one wheel notch is 1, a drag is however far it
    // went). Direct-connection only — the radio relay has no equivalent.
    void adjustRelay(int relay, int steps);

    // Command methods — emit neutral intents (operate/bypass/autotune) that
    // RadioModel translates to the Flex TGXL relay via invokeExtension. The
    // direct port-9010 fast-path (autoTune when a direct conn is up, and the
    // antenna/relay methods below) stays local and does not go through the seam.
    void setOperate(bool on);
    void setBypass(bool on);
    void autoTune();

    // Advance the operate/bypass state machine one click: OPERATE → BYPASS →
    // STANDBY → OPERATE. Lives here rather than in the widgets because two
    // controls drive it — the tuner applet's button and the status-bar TUN
    // indicator — and a second copy of the rules is a second thing to get
    // wrong (the status bar's copy silently did nothing on a direct-only
    // tuner, #4553). Falls back to OPERATE ↔ STANDBY when there is no radio
    // relay, since bypass has no direct-connection equivalent.
    void cycleOperateState();

    // Antenna switch (TGXL 3x1): ant = 1, 2, or 3 (1-indexed for command).
    // Direct-connection only, and only on a tuner that reports the switch.
    void setAntennaA(int ant);

signals:
    void stateChanged();               // any property changed
    void tuningChanged(bool tuning);   // tuning started/stopped
    void antennaAChanged(int antA);    // antenna port changed (0-indexed)
    void metersChanged(float fwdPower, float swr);  // fwd power/SWR from direct TGXL
    void presenceChanged(bool present); // tuner detected / lost
    void directConnectionChanged(bool connected);
    // Neutral relay intents. RadioModel translates each to the Flex TGXL wire
    // ("tgxl set handle=<h> mode=/bypass=", "tgxl autotune handle=<h>") via
    // IRadioBackend::invokeExtension. autotuneRequested also carries the TX
    // interlock gate in RadioModel (was a commandReady string-sniff).
    void operateRequested(bool on);
    void bypassRequested(bool on);
    void autotuneRequested();

private:
    QString m_handle;
    QString m_model;
    QString m_serialNum;
    QString m_tgxlIp;
    bool    m_operate{false};
    bool    m_bypass{false};
    bool    m_tuning{false};
    int     m_relayC1{0};
    int     m_relayL{0};
    int     m_relayC2{0};
    int     m_antennaA{-1};   // 0-indexed antenna port (-1 = unknown)
    float   m_fwdPower{0.0f};  // forward power in watts (from direct TGXL status)
    float   m_swr{1.0f};      // SWR ratio (from direct TGXL status)
    bool    m_oneByThree{false}; // TGXL 3x1 model, from the relayed one_by_three=1
    bool    m_threeWay{false};   // TGXL 3x1 model, from the direct info reply 3way=1

    // Which of the direct channel's three message kinds a key/value set came
    // from. Most fields mean the same thing in all three, but "state" is read
    // ONLY from the status reply — the unsolicited push is itself named
    // "state", and a key of that name elsewhere is not the operate flag.
    enum class DirectSource { StatePush, StatusReply, InfoReply };

    // Apply a key/value set from the direct port-9010 channel. Change-gated;
    // emits the same edge signals as applyChanges. One path so a field carried
    // by only some of the three message kinds still reaches the UI — the relay
    // positions arrive on the status poll, which used to drop them (#4551).
    void applyDirectKvs(const QMap<QString, QString>& kvs, DirectSource source);

    TgxlConnection* m_directConn{nullptr};
    bool            m_directPresence{false};
};

} // namespace AetherSDR
