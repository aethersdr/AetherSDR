#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QObject>
#include <QString>

class QTimer;

namespace AetherSDR::colibri {

// ColibriNANO enumeration, shaped to feed the same picker as Flex and HL2
// discovery: it emits RadioInfo with family="colibri" so ConnectionPanel's
// existing onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it
// unchanged. The "address" is LocalHost — synthetic, never dialed, exactly
// like the sim's demo entry: a USB device has no endpoint to dial.
//
// Polling is SUSPENDED while a device is open (ColibriLib::deviceInUse): the
// vendor library does not document FTDI bus enumeration as safe under a
// running stream, so the last-seen entries are simply kept until the device
// is released.
//
// The library reports no per-device serial, so identity is the enumeration
// index: "colibrinano-<n>". Stable enough for one receiver on one PC, which
// is this device's actual deployment shape.
class ColibriDiscovery : public QObject {
    Q_OBJECT

public:
    explicit ColibriDiscovery(QObject* parent = nullptr);
    ~ColibriDiscovery() override;

    // Begin periodic polls. Safe to call twice; restarts the cadence. The
    // library is loaded lazily on the first poll — a PC with no DLL installed
    // just discovers nothing, once, quietly.
    void start(int intervalMs = 5000);
    void stop();
    void pollNow();

    static QString serialForIndex(int index);

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private:
    QTimer* m_timer = nullptr;
    QHash<QString, RadioInfo> m_seen;   // keyed by serial
    bool m_libUnavailableLogged = false;
};

}  // namespace AetherSDR::colibri
