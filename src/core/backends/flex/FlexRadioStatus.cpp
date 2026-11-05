#include "FlexRadioStatus.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace AetherSDR {
namespace {
using Fields = QMap<QString, QString>;
constexpr qsizetype kMaxText = 4096;

void field(const Fields& fields, const char* key, std::optional<QString>& value)
{
    const auto it = fields.constFind(QLatin1String(key));
    if (it != fields.cend() && it->size() <= kMaxText && !it->contains(QChar(0))) { value = *it; }
}
void field(const Fields& fields, const char* key, std::optional<int>& value)
{
    const auto it = fields.constFind(QLatin1String(key));
    if (it == fields.cend() || it->size() > 16) { return; }
    bool ok = false;
    const int parsed = it->toInt(&ok);
    if (ok) { value = parsed; }
}
void field(const Fields& fields, const char* key, std::optional<double>& value)
{
    const auto it = fields.constFind(QLatin1String(key));
    if (it == fields.cend() || it->size() > 64) { return; }
    bool ok = false;
    const double parsed = it->toDouble(&ok);
    if (ok && std::isfinite(parsed)) { value = parsed; }
}
void field(const Fields& fields, const char* key, std::optional<bool>& value)
{
    const QString text = fields.value(QLatin1String(key));
    if (text == QLatin1String("0") || text == QLatin1String("1")) { value = text == QLatin1String("1"); }
}
void byteFlag(const Fields& fields, const char* key, std::optional<bool>& value)
{
    std::optional<int> parsed;
    field(fields, key, parsed);
    if (parsed && *parsed >= 0 && *parsed <= 255) { value = *parsed != 0; }
}
void range(std::optional<int>& value, int low, int high)
{
    if (value && (*value < low || *value > high)) { value.reset(); }
}
void clientText(const Fields& fields, const char* key, std::optional<QString>& value)
{
    field(fields, key, value);
    if (value) { value = value->replace(QChar(0x7f), QLatin1Char(' ')).trimmed(); }
}
void address(const Fields& fields, const char* key, std::optional<QString>& value)
{
    field(fields, key, value);
    if (!value) { return; }
    // FlexLib ParseNetParamsStatus clears a present, invalid address. Absence
    // preserves it; an explicit empty value is the DHCP/reset readback.
    const QHostAddress parsed(*value);
    value = parsed.protocol() == QAbstractSocket::IPv4Protocol ? parsed.toString() : QString();
}
} // namespace

std::optional<RadioStatusObservation> decodeFlexRadioStatus(
    const QString& object, const QMap<QString, QString>& kvs,
    const std::shared_ptr<RadioStatusLifetime>& lifetime, quint32 localHandle)
{
    if (object.size() > 128 || !lifetime || !lifetime->active.load(std::memory_order_acquire)
        || !lifetime->deliveryOpen.load(std::memory_order_acquire)) { return std::nullopt; }
    RadioStatusObservation out;
    out.lifetime = lifetime;
    out.localHandle = localHandle;
    if (object == QLatin1String("radio")) {
        RadioDelta& d = out.radio;
        // Identity / capability
        field(kvs, "model", d.model);
        field(kvs, "slices", d.slicesAvailable);
        field(kvs, "callsign", d.callsign);
        field(kvs, "nickname", d.nickname);
        field(kvs, "region", d.region);
        field(kvs, "radio_options", d.radioOptions);
        field(kvs, "bands", d.bandsRaw);   // optional radio-declared bands (gateway/non-Flex); validated in RadioModel
        // Global flags
        field(kvs, "remote_on_enabled", d.remoteOnEnabled);
        field(kvs, "mf_enable", d.multiFlexEnabled);
        field(kvs, "enforce_private_ip_connections", d.enforcePrivateIp);
        field(kvs, "binaural_rx", d.binauralRx);
        field(kvs, "full_duplex_enabled", d.fullDuplex);
        field(kvs, "mute_local_audio_when_remote", d.muteLocalWhenRemote);
        field(kvs, "auto_save", d.autoSave);
        field(kvs, "low_latency_digital_modes", d.lowLatencyDigital);
        field(kvs, "tnf_enabled", d.tnfEnabled);
        // Calibration / defaults
        field(kvs, "freq_error_ppb", d.freqErrorPpb);
        field(kvs, "cal_freq", d.calFreqMhz);
        field(kvs, "rtty_mark_default", d.rttyMarkDefault);
        // Audio outputs
        field(kvs, "lineout_gain", d.lineoutGain);
        field(kvs, "lineout_mute", d.lineoutMute);
        field(kvs, "headphone_gain", d.headphoneGain);
        field(kvs, "headphone_mute", d.headphoneMute);
        field(kvs, "front_speaker_mute", d.frontSpeakerMute);
        // DAX-IQ capacity
        field(kvs, "daxiq_capacity", d.daxiqCapacity);
        field(kvs, "daxiq_available", d.daxiqAvailable);
        range(d.slicesAvailable, 0, 1024);
        range(d.lineoutGain, 0, 100);
        range(d.headphoneGain, 0, 100);
        range(d.daxiqCapacity, 0, 1024);
        range(d.daxiqAvailable, 0, 1024);
        range(d.rttyMarkDefault, 0, 10000);
        if (d.calFreqMhz && *d.calFreqMhz < 0) { d.calFreqMhz.reset(); }
        field(kvs, "pll_done", out.calibrationDone);
    } else if (object == QLatin1String("radio oscillator")) {
        out.kind = RadioStatusObservation::Kind::Oscillator;
        field(kvs, "state", out.oscillator.state);
        field(kvs, "setting", out.oscillator.setting);
        if (out.oscillator.setting) {
            const QString setting = out.oscillator.setting->toLower();
            if (setting == QLatin1String("auto") || setting == QLatin1String("tcxo")
                || setting == QLatin1String("external") || setting == QLatin1String("gpsdo")
                || setting == QLatin1String("gnss")) { out.oscillator.setting = setting; }
            else { out.oscillator.setting.reset(); }
        }
        byteFlag(kvs, "locked", out.oscillator.locked);
        byteFlag(kvs, "ext_present", out.oscillator.externalPresent);
        byteFlag(kvs, "tcxo_present", out.oscillator.tcxoPresent);
        std::optional<bool> gps, gnss;
        byteFlag(kvs, "gpsdo_present", gps);
        byteFlag(kvs, "gnss_present", gnss);
        if (gps || gnss) { out.oscillator.gpsPresent = gps.value_or(false) || gnss.value_or(false); }
    } else if (object == QLatin1String("radio static_net_params")) {
        out.kind = RadioStatusObservation::Kind::StaticNetwork;
        address(kvs, "ip", out.network.ip);
        address(kvs, "netmask", out.network.netmask);
        address(kvs, "gateway", out.network.gateway);
    } else {
        static const QRegularExpression clientPattern(
            QStringLiteral("^client\\s+(0x[0-9a-fA-F]{1,8})(?:\\s+(connected|disconnected))?$"));
        const QRegularExpressionMatch match = clientPattern.match(object);
        if (!match.hasMatch()) { return std::nullopt; }
        bool ok = false;
        out.client.handle = match.captured(1).mid(2).toUInt(&ok, 16);
        if (!ok || out.client.handle == 0) { return std::nullopt; }
        out.kind = RadioStatusObservation::Kind::Client;
        if (match.captured(2) == QLatin1String("disconnected")) {
            out.client.action = ClientStatusValues::Action::Disconnected;
            std::optional<bool> forced, duplicate, wanFailed;
            field(kvs, "forced", forced);
            field(kvs, "duplicate_client_id", duplicate);
            field(kvs, "wan_validation_failed", wanFailed);
            out.client.forced = forced.value_or(false);
            out.client.duplicateClientId = duplicate.value_or(false);
            out.client.wanValidationFailed = wanFailed.value_or(false);
        } else {
            if (match.captured(2) == QLatin1String("connected") || kvs.contains(QStringLiteral("connected"))) {
                out.client.action = ClientStatusValues::Action::Connected;
            }
            clientText(kvs, "client_id", out.client.clientId);
            clientText(kvs, "program", out.client.program);
            clientText(kvs, "station", out.client.station);
            field(kvs, "local_ptt", out.client.localPtt);
            static const QRegularExpression guid(QStringLiteral(
                "^\\{?[0-9a-fA-F]{8}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{12}\\}?$"));
            for (const char* key : {"ip", "client_ip", "remote_ip", "name"}) {
                std::optional<QString> source;
                clientText(kvs, key, source);
                if (source && !source->isEmpty() && !guid.match(*source).hasMatch()) {
                    out.client.source = source;
                    break;
                }
            }
        }
    }
    return out;
}
} // namespace AetherSDR
