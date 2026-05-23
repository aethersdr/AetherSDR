#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QVariantMap>

namespace MasterSDR {

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

    const QMap<QString, UsbCable>& cables() const { return m_cables; }

    // Status parsing — called from RadioModel::onStatusReceived
    void applyStatus(const QString& serialNumber, const QMap<QString, QString>& kvs);
    void handleRemoved(const QString& serialNumber);

    // Command builders
    void sendSet(const QString& sn, const QString& key, const QString& value);
    void sendSetBit(const QString& sn, int bit, const QString& key, const QString& value);
    void sendRemove(const QString& sn);

signals:
    void cableAdded(const QString& serialNumber);
    void cableRemoved(const QString& serialNumber);
    void cableChanged(const QString& serialNumber);
    void commandReady(const QString& cmd);

private:
    void parseBaseStatus(UsbCable& cable, const QMap<QString, QString>& kvs);
    void parseCatStatus(UsbCable& cable, const QMap<QString, QString>& kvs);
    void parseBcdStatus(UsbCable& cable, const QMap<QString, QString>& kvs);
    void parseBitStatus(UsbCable& cable, const QMap<QString, QString>& kvs);
    void parsePassthroughStatus(UsbCable& cable, const QMap<QString, QString>& kvs);

    static QString decodeSpaces(const QString& s);

    QMap<QString, UsbCable> m_cables;
};

} // namespace MasterSDR
