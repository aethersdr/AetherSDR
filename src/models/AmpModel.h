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

// State model for a power amplifier the radio reports through its "amplifier"
// API (today: 4O3A Power Genius XL / PGXL — any non-TGXL amp the radio proxies).
// Extracted from RadioModel (#4094).
//
// Vendor-neutral: it holds the universal amp state (presence / operate /
// telemetry) and emits a neutral operate *intent* — it builds no SmartSDR
// strings. FlexBackend translates the intent into the radio-proxied
// "amplifier set … operate=" relay via its invokeExtension("flex", …) path
// (#4094); that relay is the ONLY path that works for remote/SmartLink (the
// direct PgxlConnection on port 9008 is telemetry-only). The Flex handle is a
// backend detail supplied by RadioModel through invokeExtension's vendor arg.
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
