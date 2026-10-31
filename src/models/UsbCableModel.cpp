#include "UsbCableModel.h"
#include <QPointer>
#include <QThread>

namespace AetherSDR {
namespace {
template<typename T> void assign(T& target, const std::optional<T>& value)
{
    if (value) { target = *value; }
}
}

UsbCableModel::UsbCableModel(QObject* parent) : QObject(parent) {}
UsbCableModel::~UsbCableModel()
{
    for (const UsbCable& cable : m_cables) { if (cable.lifetime) { cable.lifetime->retire(); } }
}

QString UsbCableModel::normalizeTypeFamily(const QString& type)
{
    return type == QLatin1String("vbcd") || type == QLatin1String("bcd_vbcd") ? QStringLiteral("bcd") : type;
}

void UsbCableModel::applyObservation(const UsbCableStatusValues& value,
                                      const std::shared_ptr<SetupStatusLifetime>& identity)
{
    if (value.serial.isEmpty() || value.serial.size() > 128 || value.bit < -1 || value.bit > 7) { return; }
    if (value.removed) {
        if (!identity || m_cables.value(value.serial).lifetime == identity) { handleRemoved(value.serial); }
        return;
    }
    if (identity && !identity->current()) { return; }
    auto existing = m_cables.constFind(value.serial);
    if (value.bit >= 0) {
        if (existing == m_cables.cend() || existing->type != QLatin1String("bit")
            || (identity && existing->lifetime != identity)) { return; }
        ++m_changeRevision;
        auto& bit = m_cables[value.serial].bits[value.bit];
        assign(bit.enabled, value.enabled);
        assign(bit.source, value.source);
        assign(bit.sourceRxAnt, value.sourceRxAnt);
        assign(bit.sourceTxAnt, value.sourceTxAnt);
        assign(bit.sourceSlice, value.sourceSlice);
        assign(bit.output, value.output);
        assign(bit.activeHigh, value.activeHigh);
        assign(bit.pttDependent, value.pttDependent);
        assign(bit.pttDelayMs, value.pttDelayMs);
        assign(bit.txDelayMs, value.txDelayMs);
        assign(bit.band, value.band);
        assign(bit.lowFreqMhz, value.lowFreqMhz);
        assign(bit.highFreqMhz, value.highFreqMhz);
        emit cableChanged(value.serial);
        return;
    }
    const bool isNew = existing == m_cables.cend();
    if (isNew && (!value.type || m_cables.size() >= 256)) { return; }
    const bool retype = !isNew && ((identity && identity != existing->lifetime)
        || (value.type && normalizeTypeFamily(*value.type) != normalizeTypeFamily(existing->type)));
    UsbCable cable = isNew ? UsbCable{} : *existing;
    const quint64 revision = ++m_changeRevision;
    if (retype) {
        cable.lifetime->retire();
        const UsbCable base = cable;
        cable = {};
        if (!identity) {
            cable.name = base.name;
            cable.enabled = base.enabled;
            cable.present = base.present;
            cable.loggingEnabled = base.loggingEnabled;
        }
        m_cables.remove(value.serial);
        const QPointer<UsbCableModel> alive(this);
        emit cableRemoved(value.serial);
        if (!alive || revision != m_changeRevision) { return; }
    }
    if (isNew || retype) { cable.lifetime = identity ? identity : std::make_shared<SetupStatusLifetime>(); }
    cable.serialNumber = value.serial;
    assign(cable.type, value.type);
    assign(cable.name, value.name);
    assign(cable.enabled, value.enabled);
    assign(cable.present, value.present);
    assign(cable.loggingEnabled, value.loggingEnabled);
    const QString family = normalizeTypeFamily(cable.type);
    if (family == QLatin1String("cat") || family == QLatin1String("passthrough")) {
        assign(cable.speed, value.speed);
        assign(cable.dataBits, value.dataBits);
        assign(cable.stopBits, value.stopBits);
        assign(cable.parity, value.parity);
        assign(cable.flowControl, value.flowControl);
    }
    if (family == QLatin1String("cat")) { assign(cable.autoReport, value.autoReport); }
    if (family == QLatin1String("bcd")) { assign(cable.activeHigh, value.activeHigh); }
    if (family == QLatin1String("ldpa")) {
        assign(cable.ldpaBand, value.band);
        assign(cable.preamp, value.preamp);
    }
    if (family == QLatin1String("cat") || family == QLatin1String("bcd")
        || family == QLatin1String("bit") || family == QLatin1String("ldpa")) {
        assign(cable.source, value.source);
        assign(cable.sourceRxAnt, value.sourceRxAnt);
        assign(cable.sourceTxAnt, value.sourceTxAnt);
        assign(cable.sourceSlice, value.sourceSlice);
    }
    m_cables.insert(value.serial, cable);
    if (isNew || retype) { emit cableAdded(value.serial); }
    else { emit cableChanged(value.serial); }
}

void UsbCableModel::handleRemoved(const QString& serial)
{
    const auto it = m_cables.find(serial);
    if (it == m_cables.end()) { return; }
    ++m_changeRevision;
    if (it->lifetime) { it->lifetime->retire(); }
    m_cables.erase(it);
    emit cableRemoved(serial);
}

void UsbCableModel::clear()
{
    const QStringList serials = m_cables.keys();
    for (const UsbCable& cable : m_cables) { if (cable.lifetime) { cable.lifetime->retire(); } }
    m_cables.clear();
    const quint64 revision = ++m_changeRevision;
    const QPointer<UsbCableModel> alive(this);
    for (const QString& serial : serials) {
        emit cableRemoved(serial);
        if (!alive || revision != m_changeRevision) { return; }
    }
}

void UsbCableModel::requestSetting(const QString& sn, UsbCableRequest::Field field,
                                  const QString& text, int bit,
                                  const std::shared_ptr<SetupStatusLifetime>& expectedLifetime)
{
    if (QThread::currentThread() != thread()) { return; }
    using Field = UsbCableRequest::Field;
    UsbCableRequest request{sn, bit, field, text};
    request.expectedLifetime = expectedLifetime ? expectedLifetime : m_cables.value(sn).lifetime;
    if (request.expectedLifetime != m_cables.value(sn).lifetime) { return; }
    if (!request.expectedLifetime || !request.expectedLifetime->current()) { return; }
    bool valid = true;
    switch (field) {
    case Field::Enabled: case Field::AutoReport: case Field::Preamp: case Field::PttDependent:
        valid = text == QLatin1String("0") || text == QLatin1String("1");
        request.value = text == QLatin1String("1"); break;
    case Field::ActiveHigh:
        valid = text == QLatin1String("active_high") || text == QLatin1String("active_low");
        request.value = text == QLatin1String("active_high"); break;
    case Field::SerialSpeed: case Field::DataBits: case Field::StopBits:
    case Field::PttDelayMs: case Field::TxDelayMs:
        request.value = text.toInt(&valid); break;
    case Field::LowFrequencyMhz: case Field::HighFrequencyMhz:
        request.value = text.toDouble(&valid); break;
    default: break;
    }
    if (valid && request.valid()) { emit controlRequested(request); }
}

void UsbCableModel::requestRemoval(const QString& sn)
{
    if (QThread::currentThread() != thread()) { return; }
    const auto lifetime = m_cables.value(sn).lifetime;
    if (lifetime && lifetime->current()) {
        emit controlRequested({sn, -1, UsbCableRequest::Field::Remove, std::monostate{}, lifetime});
    }
}

} // namespace AetherSDR
