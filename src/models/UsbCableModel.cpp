#include "UsbCableModel.h"
#include <QDebug>

namespace AetherSDR {

UsbCableModel::UsbCableModel(QObject* parent)
    : QObject(parent)
{}

QString UsbCableModel::decodeSpaces(const QString& s)
{
    return QString(s).replace(QChar(0x7F), ' ');
}

QString UsbCableModel::normalizeTypeFamily(const QString& type)
{
    if (type == "bcd" || type == "vbcd" || type == "bcd_vbcd") {
        return "bcd";
    }
    return type;
}

// ── Status parsing ──────────────────────────────────────────────────────────

void UsbCableModel::applyStatus(const QString& serialNumber,
                                 const QMap<QString, QString>& kvs)
{
    bool isNew = !m_cables.contains(serialNumber);

    // A type change on an already-known cable is a distinct lifecycle event,
    // not an in-place mutation — mirrors FlexLib's UsbCable.CableType setter,
    // which removes the old cable object and lets the radio's next status
    // reconstruct a brand-new one of the new type (UsbCable.cs / Radio.cs
    // ParseUsbCableStatus). Without this, stale fields from the old type
    // (e.g. cable.band, cable.bits[]) would survive into the new type's page.
    // Compared by family (normalizeTypeFamily) so a BCD sub-type change
    // (bcd <-> vbcd <-> bcd_vbcd) doesn't trip a spurious recreate — FlexLib's
    // setter compares UsbCableType, where all three are the same value.
    UsbCable preservedBase;
    bool isFamilyChange = false;
    if (!isNew && kvs.contains("type") &&
        normalizeTypeFamily(kvs["type"]) != normalizeTypeFamily(m_cables[serialNumber].type)) {
        isFamilyChange = true;
        preservedBase = m_cables[serialNumber];
        m_cables.remove(serialNumber);
        emit cableRemoved(serialNumber);
        isNew = true;
    }

    auto& cable = m_cables[serialNumber];
    cable.serialNumber = serialNumber;

    // Radio-assigned identity fields survive a type-change recreate — FlexLib's
    // Radio.cs ParseUsbCableStatus reuses these across the change rather than
    // blanking them, so a partial retype-confirmation status (missing
    // name=/enable=) doesn't clear the user's cable name/enabled flag until a
    // full status arrives. parseBaseStatus() below overwrites these if the
    // status does carry fresh values.
    if (isFamilyChange) {
        cable.name = preservedBase.name;
        cable.enabled = preservedBase.enabled;
        cable.present = preservedBase.present;
        cable.loggingEnabled = preservedBase.loggingEnabled;
    }

    // Detect type on first status
    if (kvs.contains("type")) {
        cable.type = kvs["type"];
    }

    // Check for bit-level status: "bit" key with value = bit number
    // These arrive as: usb_cable <sn> bit <N> key=val ...
    // RadioModel pre-parses this and calls applyStatus with special
    // "bit_number" key and the bit-specific KVs.
    if (kvs.contains("_bit_number")) {
        int bitNum = kvs["_bit_number"].toInt();
        if (bitNum >= 0 && bitNum < 8) {
            auto& bit = cable.bits[bitNum];
            for (auto it = kvs.begin(); it != kvs.end(); ++it) {
                const QString& k = it.key();
                const QString& v = it.value();
                if (k == "_bit_number") continue;
                if      (k == "enable")       bit.enabled = (v == "1");
                else if (k == "source")       bit.source = v;
                else if (k == "output")       bit.output = v;
                else if (k == "polarity")     bit.activeHigh = (v == "active_high");
                else if (k == "ptt_dependent") bit.pttDependent = (v == "1");
                else if (k == "ptt_delay")    bit.pttDelayMs = v.toInt();
                else if (k == "tx_delay")     bit.txDelayMs = v.toInt();
                else if (k == "band")         bit.band = v;
                else if (k == "low_freq")     bit.lowFreqMhz = v.toDouble();
                else if (k == "high_freq")    bit.highFreqMhz = v.toDouble();
                else if (k == "source_rx_ant") bit.sourceRxAnt = v;
                else if (k == "source_tx_ant") bit.sourceTxAnt = v;
                else if (k == "source_slice") bit.sourceSlice = v;
            }
        }
        emit cableChanged(serialNumber);
        return;
    }

    parseBaseStatus(cable, kvs);

    // Type-specific parsing
    if      (cable.type == "cat")         parseCatStatus(cable, kvs);
    else if (cable.type == "bcd" ||
             cable.type == "vbcd" ||
             cable.type == "bcd_vbcd")    parseBcdStatus(cable, kvs);
    else if (cable.type == "bit")         parseBitStatus(cable, kvs);
    else if (cable.type == "passthrough") parsePassthroughStatus(cable, kvs);
    else if (cable.type == "ldpa") {
        parseLdpaStatus(cable, kvs);
    }

    if (isNew) {
        qDebug() << "UsbCableModel: new cable" << serialNumber << "type:" << cable.type
                 << "name:" << cable.name;
        emit cableAdded(serialNumber);
    } else {
        emit cableChanged(serialNumber);
    }
}

void UsbCableModel::handleRemoved(const QString& serialNumber)
{
    if (m_cables.remove(serialNumber)) {
        qDebug() << "UsbCableModel: cable removed" << serialNumber;
        emit cableRemoved(serialNumber);
    }
}

void UsbCableModel::parseBaseStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "enable")     cable.enabled = (v == "1");
        else if (k == "plugged_in") cable.present = (v == "1");
        else if (k == "name")       cable.name = decodeSpaces(v);
        else if (k == "log")        cable.loggingEnabled = (v == "1");
    }
}

void UsbCableModel::parseCatStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "speed")          cable.speed = v.toInt();
        else if (k == "data_bits")      cable.dataBits = v.toInt();
        else if (k == "parity")         cable.parity = v;
        else if (k == "stop_bits")      cable.stopBits = v.toInt();
        else if (k == "flow_control")   cable.flowControl = v;
        else if (k == "source")         cable.source = v;
        else if (k == "source_rx_ant")  cable.sourceRxAnt = v;
        else if (k == "source_tx_ant")  cable.sourceTxAnt = v;
        else if (k == "source_slice")   cable.sourceSlice = v;
        else if (k == "auto_report")    cable.autoReport = (v == "1");
    }
}

void UsbCableModel::parseBcdStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "polarity")       cable.activeHigh = (v == "active_high");
        else if (k == "source")         cable.source = v;
        else if (k == "source_rx_ant")  cable.sourceRxAnt = v;
        else if (k == "source_tx_ant")  cable.sourceTxAnt = v;
        else if (k == "source_slice")   cable.sourceSlice = v;
    }
}

void UsbCableModel::parseBitStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    // Cable-level Bit properties (source, etc.) come without "bit N" prefix
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "source")         cable.source = v;
        else if (k == "source_rx_ant")  cable.sourceRxAnt = v;
        else if (k == "source_tx_ant")  cable.sourceTxAnt = v;
        else if (k == "source_slice")   cable.sourceSlice = v;
    }
    // Per-bit status is handled via the _bit_number path in applyStatus()
}

void UsbCableModel::parsePassthroughStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "speed")          cable.speed = v.toInt();
        else if (k == "data_bits")      cable.dataBits = v.toInt();
        else if (k == "parity")         cable.parity = v;
        else if (k == "stop_bits")      cable.stopBits = v.toInt();
        else if (k == "flow_control")   cable.flowControl = v;
    }
}

void UsbCableModel::parseLdpaStatus(UsbCable& cable, const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if (k == "band") {
            cable.ldpaBand = v;  // "2" or "4"
        } else if (k == "preamp") {
            cable.preamp = (v == "1");
        } else if (k == "source") {
            // AetherSDR extension beyond the FlexLib authority (Principle I):
            // FlexLib's UsbLdpaCable.cs writes source= on set but does NOT
            // parse it back from status (only band/preamp are read). We read it
            // in case the radio echoes it, but the value is unverified against
            // the reference client — harmless if the radio never sends it. See
            // the CAT/BCD/Bit paths, where FlexLib does round-trip source=.
            cable.source = v;
        } else if (k == "source_rx_ant") {
            cable.sourceRxAnt = v;
        } else if (k == "source_tx_ant") {
            cable.sourceTxAnt = v;
        } else if (k == "source_slice") {
            cable.sourceSlice = v;
        }
    }
}

// ── Command builders ────────────────────────────────────────────────────────

void UsbCableModel::sendSet(const QString& sn, const QString& key, const QString& value)
{
    emit commandReady(QString("usb_cable set %1 %2=%3").arg(sn, key, value));
}

void UsbCableModel::sendSetBit(const QString& sn, int bit, const QString& key, const QString& value)
{
    emit commandReady(QString("usb_cable setbit %1 %2 %3=%4").arg(sn).arg(bit).arg(key, value));
}

void UsbCableModel::sendRemove(const QString& sn)
{
    emit commandReady(QString("usb_cable remove %1").arg(sn));
}

} // namespace AetherSDR
