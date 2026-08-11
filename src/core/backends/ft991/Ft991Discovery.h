#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QObject>
#include <QString>

class QTimer;

namespace AetherSDR::ft991 {

// FT-991 candidate enumeration, shaped to feed the same picker as the Flex,
// HL2 and Colibri sweeps: RadioInfo with family="ft991", synthetic LocalHost
// address (a serial device has no endpoint to dial), identity "ft991-<port>".
//
// A CAT radio cannot be probed without opening its port — and opening ports
// speculatively is not this class's call to make (a port may belong to a
// rotator, a keyer, anything). So this is a HEURISTIC listing: serial ports
// whose description/manufacturer matches the FT-991's Silicon Labs CP210x
// USB bridge (skipping the CP2105's "Standard" half — CAT lives on the
// "Enhanced" COM port). Listing is an offer, not an action: connect verifies
// the radio with "ID;" before anything else is said, so a wrong guess costs
// one refused connect, never a mis-driven device.
class Ft991Discovery : public QObject {
    Q_OBJECT

public:
    explicit Ft991Discovery(QObject* parent = nullptr);
    ~Ft991Discovery() override;

    // Begin periodic polls. Safe to call twice; restarts the cadence.
    void start(int intervalMs = 5000);
    void stop();
    void pollNow();

    static QString serialForPort(const QString& portName);

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private:
    QTimer* m_timer = nullptr;
    QHash<QString, RadioInfo> m_seen;   // keyed by serial
};

}  // namespace AetherSDR::ft991
