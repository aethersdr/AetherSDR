#include "FlexSetupStatus.h"

#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {
namespace {
using Fields = QMap<QString, QString>;

std::optional<QString> text(const Fields& fields, const QString& key, int limit = 256)
{
    const auto it = fields.constFind(key);
    if (it == fields.cend() || it->size() > limit) { return {}; }
    QString value = *it;
    if (value.startsWith(u'"') && value.endsWith(u'"') && value.size() >= 2) {
        value = value.mid(1, value.size() - 2);
    }
    value.replace(QChar(0x7f), u' ');
    for (const QChar ch : value) {
        if (!ch.isPrint()) { return {}; }
    }
    return value;
}

std::optional<int> integer(const QString& value, int minimum, int maximum)
{
    bool ok = false;
    const int number = value.toInt(&ok);
    if (!ok || number < minimum || number > maximum) { return {}; }
    return number;
}

std::optional<int> integer(const Fields& fields, const QString& key,
                          int minimum = std::numeric_limits<int>::min(),
                          int maximum = std::numeric_limits<int>::max())
{
    return integer(fields.value(key), minimum, maximum);
}

std::optional<double> number(const Fields& fields, const QString& key)
{
    bool ok = false;
    const double result = fields.value(key).toDouble(&ok);
    if (!ok || !std::isfinite(result)) { return {}; }
    return result;
}

std::optional<bool> boolean(const Fields& fields, const QString& key, int maximum = 1)
{
    const auto value = integer(fields, key, 0, maximum);
    return value ? std::optional<bool>(*value != 0) : std::nullopt;
}

std::optional<QString> choice(const Fields& fields, const QString& key, const QStringList& choices)
{
    const auto value = text(fields, key);
    return value && choices.contains(*value) ? value : std::nullopt;
}

std::optional<bool> polarity(const Fields& fields)
{
    const auto value = choice(fields, QStringLiteral("polarity"),
                              {QStringLiteral("active_high"), QStringLiteral("active_low")});
    return value ? std::optional<bool>(*value == QLatin1String("active_high")) : std::nullopt;
}

bool identifier(const QString& value)
{
    if (value.isEmpty() || value.size() > 128) { return false; }
    return std::all_of(value.begin(), value.end(), [](QChar ch) {
        return ch.unicode() < 128 && (ch.isLetterOrNumber() || ch == u'_' || ch == u'-' || ch == u'/');
    });
}
}

std::optional<SetupStatusObservation> decodeFlexSetupStatus(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<SetupStatusLifetime>& lifetime)
{
    if (!lifetime || !lifetime->current() || object.size() > 256 || fields.size() > 64) { return {}; }
    const QStringList words = object.split(u' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) { return {}; }
    SetupStatusObservation result;
    result.lifetime = lifetime;
    if (words[0] == QLatin1String("xvtr") && words.size() == 2) {
        const auto id = integer(words[1], 0, std::numeric_limits<int>::max());
        if (!id) { return {}; }
        auto& value = result.transverter;
        value.id = *id;
        value.removed = boolean(fields, QStringLiteral("in_use")) == std::optional<bool>(false);
        value.order = integer(fields, QStringLiteral("order"), 0, 16);
        value.name = text(fields, QStringLiteral("name"));
        value.rfFreq = number(fields, QStringLiteral("rf_freq"));
        value.ifFreq = number(fields, QStringLiteral("if_freq"));
        value.loError = number(fields, QStringLiteral("lo_error"));
        value.rxGain = number(fields, QStringLiteral("rx_gain"));
        value.maxPower = number(fields, QStringLiteral("max_power"));
        value.rxOnly = boolean(fields, QStringLiteral("rx_only"));
        value.isValid = boolean(fields, QStringLiteral("is_valid"));
        if (!value.removed && !value.order && !value.name && !value.rfFreq && !value.ifFreq
            && !value.loError && !value.rxGain && !value.maxPower && !value.rxOnly && !value.isValid) { return {}; }
    } else if (words[0] == QLatin1String("usb_cable") && words.size() >= 2) {
        result.kind = SetupStatusObservation::Kind::UsbCable;
        auto& value = result.usbCable;
        if (!identifier(words[1])) { return {}; }
        value.serial = words[1];
        if (words.size() == 3 && words[2] == QLatin1String("removed")) {
            value.removed = true;
            return result;
        }
        if (words.size() == 4 && words[2] == QLatin1String("bit")) {
            const auto bit = integer(words[3], 0, 7);
            if (!bit) { return {}; }
            value.bit = *bit;
        } else if (words.size() != 2) { return {}; }
        value.type = choice(fields, QStringLiteral("type"), {QStringLiteral("cat"), QStringLiteral("bit"),
            QStringLiteral("bcd"), QStringLiteral("vbcd"), QStringLiteral("bcd_vbcd"),
            QStringLiteral("ldpa"), QStringLiteral("passthrough"), QStringLiteral("invalid")});
        value.name = text(fields, QStringLiteral("name"));
        value.enabled = boolean(fields, QStringLiteral("enable"), 255);
        value.present = boolean(fields, QStringLiteral("plugged_in"), 255);
        value.loggingEnabled = boolean(fields, QStringLiteral("log"), 255);
        value.autoReport = boolean(fields, QStringLiteral("auto_report"), 255);
        value.preamp = boolean(fields, QStringLiteral("preamp"), 255);
        value.pttDependent = boolean(fields, QStringLiteral("ptt_dependent"), 255);
        value.activeHigh = polarity(fields);
        value.source = choice(fields, QStringLiteral("source"), {QStringLiteral("None"), QStringLiteral("tx_pan"),
            QStringLiteral("tx_slice"), QStringLiteral("active_slice"), QStringLiteral("tx_ant"),
            QStringLiteral("rx_ant"), QStringLiteral("ordinal_slice")});
        value.sourceRxAnt = text(fields, QStringLiteral("source_rx_ant"));
        value.sourceTxAnt = text(fields, QStringLiteral("source_tx_ant"));
        value.sourceSlice = text(fields, QStringLiteral("source_slice"));
        value.parity = choice(fields, QStringLiteral("parity"), {QStringLiteral("none"), QStringLiteral("odd"), QStringLiteral("even"), QStringLiteral("mark"), QStringLiteral("space")});
        value.flowControl = choice(fields, QStringLiteral("flow_control"), {QStringLiteral("none"), QStringLiteral("rts_cts"), QStringLiteral("dtr_dsr"), QStringLiteral("xon_xoff")});
        value.speed = integer(fields, QStringLiteral("speed"), 0);
        value.dataBits = integer(fields, QStringLiteral("data_bits"), 7, 8);
        value.stopBits = integer(fields, QStringLiteral("stop_bits"), 1, 2);
        value.pttDelayMs = integer(fields, QStringLiteral("ptt_delay"));
        value.txDelayMs = integer(fields, QStringLiteral("tx_delay"));
        value.band = text(fields, QStringLiteral("band"));
        value.output = choice(fields, QStringLiteral("output"), {QStringLiteral("band"), QStringLiteral("freq_range")});
        value.lowFreqMhz = number(fields, QStringLiteral("low_freq"));
        value.highFreqMhz = number(fields, QStringLiteral("high_freq"));
    } else if (words.size() == 3 && words[0] == QLatin1String("radio")
               && words[1] == QLatin1String("filter_sharpness")) {
        result.kind = SetupStatusObservation::Kind::FilterSharpness;
        auto& value = result.sharpness;
        const QString mode = words[2].toLower();
        if (mode == QLatin1String("voice")) { value.mode = FilterSharpnessStatusValues::Mode::Voice; }
        else if (mode == QLatin1String("cw")) { value.mode = FilterSharpnessStatusValues::Mode::Cw; }
        else if (mode == QLatin1String("digital")) { value.mode = FilterSharpnessStatusValues::Mode::Digital; }
        else { return {}; }
        // FlexLib 4.3.16 clamps level independently; auto_level is a byte boolean.
        if (const auto level = integer(fields, QStringLiteral("level"))) { value.level = std::clamp(*level, 0, 3); }
        value.automatic = boolean(fields, QStringLiteral("auto_level"), 255);
        if (!value.level && !value.automatic) { return {}; }
    } else if (words[0] == QLatin1String("license") && words.size() <= 2) {
        result.kind = SetupStatusObservation::Kind::License;
        auto& value = result.license;
        if (words.size() == 1) {
            value.radioId = text(fields, QStringLiteral("radio_id"));
            if (value.radioId) { *value.radioId = value.radioId->toUpper(); }
            value.maximumVersion = text(fields, QStringLiteral("highest_major_version"));
        } else if (words[1] == QLatin1String("subscription")) {
            value.kind = LicenseStatusValues::Kind::Subscription;
            value.name = fields.value(QStringLiteral("name")).toLower();
            if (value.name != QLatin1String("smartsdr+") && value.name != QLatin1String("smartsdr+_early_access")) { return {}; }
            const auto expiration = text(fields, QStringLiteral("expiration"), 64);
            if (!expiration) { return {}; }
            value.expiration = QDateTime::fromString(*expiration, Qt::ISODate);
            if (!value.expiration.isValid()) {
                const QDate date = QDate::fromString(*expiration, Qt::ISODate);
                if (!date.isValid() || date.toString(Qt::ISODate) != *expiration) { return {}; }
                value.expiration = date.startOfDay();
            }
        } else if (words[1] == QLatin1String("feature")) {
            value.kind = LicenseStatusValues::Kind::Feature;
            const auto name = text(fields, QStringLiteral("name"), 128);
            const auto enabled = boolean(fields, QStringLiteral("enabled"));
            if (!name || name->trimmed().isEmpty() || !enabled) { return {}; }
            value.name = name->trimmed().toLower();
            value.enabled = *enabled;
            const auto reason = text(fields, QStringLiteral("reason"));
            value.reason = reason && !reason->trimmed().isEmpty() ? reason->trimmed().toLower() : QStringLiteral("unknown");
        } else { return {}; }
    } else { return {}; }
    return result;
}

std::optional<SetupStatusObservation> FlexSetupStatusSource::decode(
    const QString& object, const QMap<QString, QString>& fields,
    const std::shared_ptr<SetupStatusLifetime>& source)
{
    auto report = decodeFlexSetupStatus(object, fields, source);
    if (!report) { return {}; }
    const std::scoped_lock lock(m_mutex);
    if (source != m_lifetime || !source->current()) { return {}; }
    QString key;
    QString family;
    bool removed = false;
    bool canCreate = true;
    if (report->kind == SetupStatusObservation::Kind::Transverter) {
        key = QStringLiteral("xvtr:%1").arg(report->transverter.id);
        removed = report->transverter.removed;
    } else if (report->kind == SetupStatusObservation::Kind::UsbCable) {
        key = QStringLiteral("usb:") + report->usbCable.serial;
        removed = report->usbCable.removed;
        family = report->usbCable.type.value_or(QString());
        if (family == QLatin1String("vbcd") || family == QLatin1String("bcd_vbcd")) { family = QStringLiteral("bcd"); }
        canCreate = report->usbCable.bit < 0 && !family.isEmpty();
    } else { return report; }
    auto it = m_entities.find(key);
    if (removed) {
        if (it == m_entities.end()) { return {}; }
        report->entityLifetime = it->lifetime;
        it->lifetime->retire();
        m_entities.erase(it);
        return report;
    }
    if (it != m_entities.end() && !family.isEmpty() && family != it->family && report->usbCable.bit < 0) {
        auto& value = report->usbCable;
        if (!value.name) { value.name = it->base.name; }
        if (!value.enabled) { value.enabled = it->base.enabled; }
        if (!value.present) { value.present = it->base.present; }
        if (!value.loggingEnabled) { value.loggingEnabled = it->base.loggingEnabled; }
        it->lifetime->retire();
        m_entities.erase(it);
        it = m_entities.end();
    }
    if (it == m_entities.end()) {
        if (!canCreate || m_entities.size() >= 512) { return {}; }
        it = m_entities.insert(key, {family, std::make_shared<SetupStatusLifetime>(), {}});
    }
    if (report->kind == SetupStatusObservation::Kind::UsbCable && report->usbCable.bit >= 0
        && it->family != QLatin1String("bit")) { return {}; }
    if (report->kind == SetupStatusObservation::Kind::UsbCable && report->usbCable.bit < 0) {
        const auto& value = report->usbCable;
        if (value.name) { it->base.name = value.name; }
        if (value.enabled) { it->base.enabled = value.enabled; }
        if (value.present) { it->base.present = value.present; }
        if (value.loggingEnabled) { it->base.loggingEnabled = value.loggingEnabled; }
    }
    report->entityLifetime = it->lifetime;
    return report;
}

} // namespace AetherSDR
