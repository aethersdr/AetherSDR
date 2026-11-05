#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QVariantMap>
#include "core/backends/UsbCableRequest.h"
#include "core/backends/SetupStatusObservation.h"

namespace AetherSDR {

// Per-bit configuration for Bit cables (8 bits, indices 0-7)
struct UsbCableBit {
    bool    enabled{false};
    QString source;           // None, tx_pan, tx_slice, active_slice, etc.
    QString output;           // "band" or "freq_range"
    bool    activeHigh{true}; // polarity
    bool    pttDependent{false};
    int     pttDelayMs{0};
    int     txDelayMs{0};
    QString band;             // e.g. "20"
    double  lowFreqMhz{0.0};
    double  highFreqMhz{0.0};
    QString sourceRxAnt;
    QString sourceTxAnt;
    QString sourceSlice;
};

// Represents a single USB cable (any type) connected to the radio
struct UsbCable {
    std::shared_ptr<SetupStatusLifetime> lifetime;
    QString serialNumber;     // unique ID from status object name
    QString type;             // cat, bcd, vbcd, bcd_vbcd, bit, ldpa, passthrough, invalid
    bool    enabled{false};
    bool    present{false};   // plugged_in (read-only)
    QString name;
    bool    loggingEnabled{false};

    // Serial parameters (CAT + Passthrough)
    int     speed{9600};
    int     dataBits{8};
    QString parity{"none"};
    int     stopBits{1};
    QString flowControl{"none"};

    // Source (CAT + BCD + Bit)
    QString source;           // frequency source
    QString sourceRxAnt;
    QString sourceTxAnt;
    QString sourceSlice;

    // CAT-specific
    bool    autoReport{true};

    // BCD-specific
    bool    activeHigh{true}; // polarity

    // LDPA-specific
    QString ldpaBand;         // "2" or "4"
    bool    preamp{false};

    // Bit cable — 8 independent bits
    UsbCableBit bits[8];
};

// Manages USB cable state reported by the radio.
// Cables appear/disappear via status messages when USB adapters are
// plugged in or removed. The type= field determines the cable class.
class UsbCableModel : public QObject {
    Q_OBJECT

public:
    explicit UsbCableModel(QObject* parent = nullptr);
    ~UsbCableModel() override;

    const QMap<QString, UsbCable>& cables() const { return m_cables; }

    void applyObservation(const UsbCableStatusValues& observation,
                          const std::shared_ptr<SetupStatusLifetime>& identity = {});
    void handleRemoved(const QString& serialNumber);
    void clear();

    // Parse editor text into a typed intent; no protocol property names.
    void requestSetting(const QString& sn, UsbCableRequest::Field field,
                        const QString& value, int bit = -1,
                        const std::shared_ptr<SetupStatusLifetime>& expectedLifetime = {});
    void requestRemoval(const QString& sn);

signals:
    void cableAdded(const QString& serialNumber);
    void cableRemoved(const QString& serialNumber);
    void cableChanged(const QString& serialNumber);
    void controlRequested(const AetherSDR::UsbCableRequest& request);

private:
    quint64 m_changeRevision{0};
    static QString normalizeTypeFamily(const QString& type);

    QMap<QString, UsbCable> m_cables;
};

} // namespace AetherSDR
