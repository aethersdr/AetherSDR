#include "FlexBackend.h"

#include <QStringList>
#include <QString>
#include <variant>

namespace AetherSDR {
namespace {
bool panToken(const QString& id)
{
    bool ok = false;
    const uint value = id.toUInt(&ok, 0);
    if (!ok || value == 0 || id.size() > 10) { return false; }
    for (const QChar ch : id) {
        const QChar lower = ch.toLower();
        if (ch.unicode() > 127 || (!ch.isDigit() && lower != QLatin1Char('x')
            && (lower < QLatin1Char('a') || lower > QLatin1Char('f')))) { return false; }
    }
    return true;
}
QString spotText(QString text)
{
    return text.replace(QLatin1Char(' '), QChar(0x7f));
}
}

ReceiveDispatch FlexBackend::requestSpot(const SpotRequest& request, const QString& panId)
{
    if (!request.valid() || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    QString command;
    switch (request.action) {
    case SpotAction::Add:
        // FlexLib 4.2.18 Radio.RequestSpot; the desktop handles tune/mode itself.
        command = QStringLiteral("spot add callsign=%1 rx_freq=%2 tx_freq=%2 source=%3 spotter_callsign=%4 trigger_action=none lifetime_seconds=%5")
                      .arg(spotText(request.callsign), QString::number(request.frequencyMhz, 'f', 6),
                           spotText(request.source), spotText(request.spotter),
                           QString::number(request.lifetimeSeconds));
        if (!request.comment.isEmpty()) { command += QStringLiteral(" comment=") + spotText(request.comment); }
        if (!request.color.isEmpty()) { command += QStringLiteral(" color=") + request.color; }
        break;
    case SpotAction::Remove: command = QStringLiteral("spot remove %1").arg(request.index); break;
    case SpotAction::Clear: command = QStringLiteral("spot clear"); break;
    case SpotAction::Trigger:
        if (!panToken(panId)) { return ReceiveDispatch::Unsupported; }
        command = QStringLiteral("spot trigger %1 pan=%2").arg(request.index).arg(panId);
        break;
    case SpotAction::Count: return ReceiveDispatch::Unsupported;
    }
    const auto sink = m_setupSink;
    return sink(command, {}) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestPanControl(const QString& panId, const PanControlRequest& request)
{
    if (!request.valid() || !panToken(panId) || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    QString field;
    QString value;
    switch (request.field) {
    case PanControlField::BandZoom: field = QStringLiteral("band_zoom"); break;
    case PanControlField::SegmentZoom: field = QStringLiteral("segment_zoom"); break;
    case PanControlField::ReceiveAntenna:
        field = QStringLiteral("rxant"); value = std::get<QString>(request.value); break;
    case PanControlField::NoiseFloorRange:
        // QString::number ignores the locale: the radio needs a '.' decimal.
        field = QStringLiteral("noise_floor_range");
        value = QString::number(std::get<double>(request.value), 'f', 1);
        break;
    case PanControlField::Count: return ReceiveDispatch::Unsupported;
    }
    if (std::holds_alternative<bool>(request.value)) { value = QString::number(int(std::get<bool>(request.value))); }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("display pan set %1 %2=%3").arg(panId, field, value), {})
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestSlicePreset(int sliceId, const SlicePresetRequest& request)
{
    if (sliceId < 0 || !request.valid() || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    QStringList fields;
    if (request.stepHz) { fields << QStringLiteral("step=%1").arg(*request.stepHz); }
    if (request.direction) {
        const double offset = *request.offsetMhz;
        const double signedOffset = *request.direction == QLatin1String("up") ? offset
            : *request.direction == QLatin1String("down") ? -offset : 0.0;
        fields << QStringLiteral("repeater_offset_dir=%1").arg(*request.direction)
               << QStringLiteral("fm_repeater_offset_freq=%1").arg(offset, 0, 'f', 6)
               << QStringLiteral("tx_offset_freq=%1").arg(signedOffset, 0, 'f', 6);
    }
    if (request.toneMode) { fields << QStringLiteral("fm_tone_mode=%1").arg(*request.toneMode); }
    if (request.toneHz) { fields << QStringLiteral("fm_tone_value=%1").arg(*request.toneHz, 0, 'f', 1); }
    const auto sink = m_setupSink;
    return sink(QStringLiteral("slice set %1 %2").arg(sliceId).arg(fields.join(QLatin1Char(' '))), {})
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
