#pragma once

#include <QObject>
#include <QMap>
#include <QString>

#include "core/backends/AmpDelta.h"

namespace AetherSDR {

// State model for a power amplifier the radio reports through its "amplifier"
// API (today: 4O3A Power Genius XL / PGXL — any non-TGXL amp the radio proxies).
// Extracted from RadioModel (#4094).
//
// Intended to become vendor-neutral: it holds the universal amp state
// (presence / operate / telemetry) and, for now, encapsulates the Flex
// "amplifier set … operate=" relay by emitting commandReady (the same pattern
// TunerModel uses). That relay is RADIO-PROXIED and is the ONLY path that works
// for remote/SmartLink operation — the direct PgxlConnection (port 9008) is
// telemetry-only. A later split moves the Flex decode/encode behind FlexBackend
// as an IRadioBackend extension verb (see #4094); until then this is mixed(flex).
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

    // Apply a normalized amplifier delta from the backend
    // (IRadioBackend::amplifierChanged, decoded by FlexBackend). Owns the state
    // machine: presence latch, operate change-gating, handle matching, removal.
    void applyChanges(const AmpDelta& delta);
    // Bulk clear on radio disconnect/teardown (matches RadioModel's prior reset).
    void reset();

    // Operate/standby command. Emits commandReady with the SmartSDR verb, which
    // the radio relays (the path that works remote). No-op without a handle.
    void setOperate(bool on);

signals:
    void presenceChanged(bool present);                       // amp detected / lost
    void stateChanged();                                      // operate changed
    void telemetryUpdated(const QMap<QString, QString>& kvs); // raw amp KVS for the GUI
    void commandReady(const QString& cmd);                    // → radio command sink

private:
    bool    m_present{false};
    bool    m_operate{false};
    QString m_handle;
    QString m_ip;
    QString m_model;
};

}  // namespace AetherSDR
