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

private:
    bool    m_present{false};
    bool    m_operate{false};
    QString m_handle;
    QString m_ip;
    QString m_model;
};

}  // namespace AetherSDR
