#pragma once

#include <QObject>
#include <QMap>
#include <QString>

#include "core/backends/AmpDelta.h"

namespace AetherSDR {

class PgxlConnection;

// One RF port as the amplifier itself reports it, from the direct port-9008
// status. The radio-proxied "amplifier" object carries none of this — only
// model, serial, ip, state and the antenna map — so it is available just
// while the direct connection is up (see hasPortInfo()).
//
// `live` is whether the amplifier has a band for the port. A PGXL reports
// bandB=0 for a port nothing is driving, which is how an unused port reads.
struct AmpPortInfo {
    bool    live{false};
    QString source;   // "flexX" — the radio's name
    QString band;     // "bandX", already a band name
    QString bias;     // "biasX" with the RADIO_ prefix removed
    bool    ptt{false};

    bool operator==(const AmpPortInfo& o) const {
        return live == o.live && source == o.source
            && band == o.band && bias == o.bias && ptt == o.ptt;
    }
    bool operator!=(const AmpPortInfo& o) const { return !(*this == o); }
};

// State model for an amplifier the radio reports via its "amplifier" API
// (today the 4O3A PGXL; any non-TGXL amp the radio proxies) (#4094).
// Vendor-neutral: emits an operate intent and builds no SmartSDR strings.
// FlexBackend relays it as "amplifier set ... operate=", the only path that works
// over SmartLink (the direct port-9008 PgxlConnection is telemetry-only).
class AmpModel : public QObject {
    Q_OBJECT

public:
    explicit AmpModel(QObject* parent = nullptr) : QObject(parent) {}

    // ---- state ----
    bool    present()   const { return m_present; }
    bool    operate()   const { return m_operate; }
    QString handle()    const { return m_handle; }
    QString ip()        const { return m_ip; }      // for the direct PGXL connection
    QString modelName() const { return m_model; }   // e.g. "PowerGeniusXL"
    // The amplifier's own state word — STANDBY / IDLE / TRANSMIT_A /
    // TRANSMIT_B / FAULT / POWERUP / SELFCHECK, as FlexLib enumerates them.
    QString stateText() const { return m_state; }
    QString alert()     const { return m_alert; }
    const AmpPortInfo& portA() const { return m_portA; }
    const AmpPortInfo& portB() const { return m_portB; }
    bool hasPortInfo()  const { return m_havePortInfo; }
    bool hasDirectConnection() const;

    // Which amplifier output a radio antenna is wired to ("PORTA" / "PORTB"),
    // or empty when the amplifier's map does not name that antenna. The
    // amplifier reports it as "ANT1:PORTA,ANT2:PORTB" on the radio-relayed
    // object; it is the only thing that says which port carries transmit,
    // since `state` only distinguishes the two once RF is already flowing.
    QString outputForAntenna(const QString& antenna) const;

    // Maximum Efficiency Algorithm reported state, upper-cased; empty until
    // reported. Three states (PGXL User Guide §9.4), because the PA bias class
    // follows the mode:
    //   OFF     - disabled.
    //   STANDBY - enabled, PA in class AAB (SSB, AM, PSK) where MEffA does not
    //             apply (§4.3.1). Not a fault.
    //   ACTIVE  - enabled and optimising, class AB.
    QString meffa() const { return m_meffa; }
    bool hasMeffa() const { return !m_meffa.isEmpty(); }
    bool meffaEnabled() const { return hasMeffa() && m_meffa != QLatin1String("OFF"); }
    // True once the whole `setup` write group is known, which is what a write
    // needs. Until then the control cannot be operated without risking the
    // four values it would have to send back.
    // A write carries the fan mode too, so an unknown one would put `fanmode=`
    // (empty) into the group that holds it.
    bool canWriteSetup() const {
        return m_haveSetupGroup && hasMeffa() && !m_fanMode.isEmpty();
    }
    QString fanMode() const { return m_fanMode; }

    // Enable or disable MEffA. The wire takes AUTO or OFF — NOT the ACTIVE /
    // STANDBY / OFF words that status reports back; those describe what the
    // algorithm is doing, not whether it may run. See setMeffaEnabled.
    //
    // Volatile by design, exactly like the vendor
    // utility's indicator: §9.4, "the change is not recorded in the
    // amplifier's configuration memory". Persisting it takes a separate
    // `save`, which this deliberately does not send — a panel toggle should
    // not rewrite the amplifier's stored configuration.
    void setMeffaEnabled(bool on);
    // Same group write, for the fan mode.
    void setFanMode(const QString& mode);

    void applySetupGroup(const QMap<QString, QString>& kvs);

private:
    // Sends the whole `setup` group with these two values substituted. One
    // place, so a fan-mode change and a MEffA change cannot disagree about
    // what a write looks like.
    void writeSetupGroup(const QString& meffa, const QString& fanMode);
    // The word a `setup` write may carry for MEffA — AUTO or OFF, never the
    // ACTIVE / STANDBY the status frame reports. See the definition.
    QString meffaWriteWord() const;

public:

    // Direct PGXL connection (port 9008) — telemetry only, no commands.
    void setDirectConnection(PgxlConnection* conn);

    // Apply a normalized amplifier delta from the backend
    // (IRadioBackend::amplifierChanged, decoded by FlexBackend). Owns the state
    // machine: presence latch, operate change-gating, handle matching, removal.
    void applyChanges(const AmpDelta& delta);
    // Bulk clear on radio disconnect/teardown (matches RadioModel's prior reset).
    void reset();

    // Operate/standby command. Emits the neutral operateRequested intent, which
    // RadioModel routes to FlexBackend for wire translation. No-op without a
    // handle (i.e. no amp present).
    void setOperate(bool on);

signals:
    void presenceChanged(bool present);                       // amp detected / lost
    void stateChanged();                                      // operate changed
    void telemetryUpdated(const QMap<QString, QString>& kvs); // raw amp KVS for the GUI
    // Neutral operate intent (on/off). RadioModel translates it to the Flex
    // "amplifier set <handle> operate=" wire via IRadioBackend::invokeExtension.
    void operateRequested(bool on);
    // Either port's reported band, bias, source or keying moved.
    void portsChanged();
    // The amplifier's state word changed.
    void ampStateChanged(const QString& state);
    // Alert text; empty means cleared.
    void alertChanged(const QString& text);
    // The antenna → output map moved (see outputForAntenna).
    void antennaMapChanged();
    // Forward power (W) and SWR from the amplifier's own port-9008 status, which
    // works when the radio publishes no AMP meters. Sourced from `fwd`, never
    // `peakfwd`: the device latches peakfwd without decay (an idle PGXL reports
    // non-zero), and peak-hold belongs to the gauge.
    void directMetersChanged(float fwdWatts, float swr);
    // The Maximum Efficiency Algorithm's reported state: ACTIVE, STANDBY or
    // OFF. Empty until the amplifier has reported one. See meffa().
    void meffaChanged(const QString& state);

private:
    bool    m_present{false};
    bool    m_operate{false};
    QString m_handle;
    QString m_ip;
    QString m_model;
    QString m_state;
    QString m_alert;
    AmpPortInfo m_portA;
    AmpPortInfo m_portB;
    float       m_directFwdWatts{0.0f};
    float       m_directSwr{1.0f};

    // The `setup` write group, exactly as the vendor utility sends it:
    //   setup nickname=<n> meffa=<m> ledintens=<i> fanmode=<f> authcode=<a>
    //
    // A write carries ALL FIVE. nickname, ledintens and authcode come from
    // `setup read`; meffa and fanmode come from the status frame, which is the
    // only place they appear — `setup read` does not report them. Changing one
    // therefore means holding the other four, which is what these are for.
    QString m_setupNickname;
    QString m_setupLedIntens;
    QString m_setupAuthCode;
    bool    m_haveSetupGroup{false};
    QString m_meffa;
    // The settable word we last commanded (AUTO / OFF), empty once the
    // amplifier's own report agrees with it. See meffaWriteWord().
    QString m_meffaIntent;
    QString m_fanMode;
    bool        m_havePortInfo{false};
    QMap<QString, QString> m_antennaOutputs;   // "ANT1" -> "PORTA"
    PgxlConnection* m_directConn{nullptr};

    // Reads the per-port block and the state word out of a direct status
    // frame. Both arrive in the same payload.
    void applyDirectStatus(const QMap<QString, QString>& kvs);
    // The state word reaches the model on both paths; this is the one place
    // that records it and re-derives which port is keyed from it.
    void applyStateWord(const QString& state);
};

}  // namespace AetherSDR
