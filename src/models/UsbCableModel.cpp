#include "UsbCableModel.h"
#include <QDebug>

namespace MasterSDR {

UsbCableModel::UsbCableModel(QObject* parent)
    : QObject(parent)
{}

QString UsbCableModel::decodeSpaces(const QString& s)
{
    return QString(s).replace(QChar(0x7F), ' ');
}

// ── Status parsing ──────────────────────────────────────────────────────────

void UsbCableModel::applyStatus(const QString& serialNumber,
                                 const QMap<QString, QString>& kvs)
{
    bool isNew = !m_cables.contains(serialNumber);
    auto& cable = m_cables[serialNumber];
    cable.serialNumber = serialNumber;

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

} // namespace MasterSDR
