#include "FlexBackend.h"
#include "models/XvtrPolicy.h"

#include <QPointer>
#include <QStringList>
#include <array>
#include <algorithm>
#include <memory>

namespace AetherSDR {
namespace {
QString frontPanelText(const QString& text)
{
    // FlexLib 4.2.18 StringHelper.Sanitize: the front-panel font's allowlist.
    QString result;
    for (const QChar ch : text) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || QStringLiteral(".,/-").contains(ch)) { result.append(ch); }
    }
    return result;
}
QString stationText(const QString& text)
{
    // Station names use the SDK's separate denylist, then escape spaces.
    const QString forbidden = QStringLiteral("*#@!%^&.,;:?\")(+=`'~<>|[{}");
    QString result;
    for (const QChar ch : text) {
        if (!forbidden.contains(ch)) { result.append(ch); }
    }
    return result.replace(QLatin1Char(' '), QChar(0x7f));
}
QString sharpnessMode(SharpnessMode mode)
{
    switch (mode) {
    case SharpnessMode::Voice: return QStringLiteral("voice");
    case SharpnessMode::Cw: return QStringLiteral("cw");
    case SharpnessMode::Digital: return QStringLiteral("digital");
    }
    return {};
}
QString referenceSource(ReferenceSource source)
{
    switch (source) {
    case ReferenceSource::Auto: return QStringLiteral("auto");
    case ReferenceSource::External: return QStringLiteral("external");
    case ReferenceSource::Gpsdo: return QStringLiteral("gpsdo");
    case ReferenceSource::Tcxo: return QStringLiteral("tcxo");
    }
    return {};
}
}

ReceiveDispatch FlexBackend::requestRadioSetup(const RadioSetupRequest& request,
                                               const RadioSetupCompletion& completion,
                                               const RadioControlFence& current)
{
    using Field = RadioSetupRequest::Field;
    if (!request.valid() || !m_setupSink || (current && !current())) { return ReceiveDispatch::Unsupported; }
    const auto integer = [&] { return QString::number(std::get<int>(request.value)); };
    const auto boolean = [&] { return QString::number(int(std::get<bool>(request.value))); };
    QString command;
    // FlexLib 4.2.18 Radio.cs setters and corresponding radio/interlock status.
    switch (request.field) {
    case Field::Callsign: command = QStringLiteral("radio callsign ") + frontPanelText(std::get<QString>(request.value).toUpper()); break;
    case Field::ClientStation: {
        const QString name = stationText(std::get<QString>(request.value));
        if (name.isEmpty()) { return ReceiveDispatch::Unsupported; }
        command = QStringLiteral("client station ") + name;
        break;
    }
    case Field::PrivateIpPolicy: command = QStringLiteral("radio set enforce_private_ip_connections=") + boolean(); break;
    case Field::NetworkMtu: command = QStringLiteral("client set enforce_network_mtu=1 network_mtu=") + integer(); break;
    case Field::Dhcp: command = QStringLiteral("radio static_net_params reset"); break;
    case Field::StaticNetwork: {
        const StaticNetwork network = std::get<StaticNetwork>(request.value);
        command = QStringLiteral("radio static_net_params ip=%1 gateway=%2 netmask=%3")
                      .arg(network.address.toString(), network.gateway.toString(), network.netmask.toString());
        break;
    }
    case Field::AccTxDelayMs: command = QStringLiteral("interlock acc_tx_delay=") + integer(); break;
    case Field::TxDelayMs: command = QStringLiteral("interlock tx_delay=") + integer(); break;
    case Field::Tx1DelayMs: command = QStringLiteral("interlock tx1_delay=") + integer(); break;
    case Field::Tx2DelayMs: command = QStringLiteral("interlock tx2_delay=") + integer(); break;
    case Field::Tx3DelayMs: command = QStringLiteral("interlock tx3_delay=") + integer(); break;
    case Field::TimeoutMs: command = QStringLiteral("interlock timeout=") + integer(); break;
    case Field::MaximumPowerPercent: command = QStringLiteral("transmit set max_power_level=") + integer(); break;
    case Field::ShowTxInWaterfall: command = QStringLiteral("transmit set show_tx_in_waterfall=") + boolean(); break;
    case Field::MeterDuringReceive: command = QStringLiteral("transmit set met_in_rx=") + boolean(); break;
    case Field::SyncCwx: command = QStringLiteral("cw synccwx ") + boolean(); break;
    case Field::RttyMarkDefaultHz: command = QStringLiteral("radio set rtty_mark_default=") + integer(); break;
    case Field::CalibrationFrequencyMhz:
        command = QStringLiteral("radio set cal_freq=") + QString::number(std::get<double>(request.value), 'f', 6); break;
    case Field::FrequencyCorrectionPpb: command = QStringLiteral("radio set freq_error_ppb=") + integer(); break;
    case Field::ReferenceSource:
        command = QStringLiteral("radio oscillator ") + referenceSource(std::get<ReferenceSource>(request.value)); break;
    case Field::MuteLocalWhenRemote: command = QStringLiteral("radio set mute_local_audio_when_remote=") + boolean(); break;
    case Field::BinauralReceive: command = QStringLiteral("radio set binaural_rx=") + boolean(); break;
    case Field::SharpnessLevel: {
        const SharpnessLevel setting = std::get<SharpnessLevel>(request.value);
        command = QStringLiteral("radio filter_sharpness %1 level=%2").arg(sharpnessMode(setting.mode)).arg(setting.level); break;
    }
    case Field::SharpnessAuto: {
        const SharpnessAuto setting = std::get<SharpnessAuto>(request.value);
        command = QStringLiteral("radio filter_sharpness %1 auto_level=%2").arg(sharpnessMode(setting.mode)).arg(int(setting.enabled)); break;
    }
    case Field::LowLatencyDigital: command = QStringLiteral("radio set low_latency_digital_modes=") + boolean(); break;
    case Field::LineoutGain: command = QStringLiteral("mixer lineout gain ") + integer(); break;
    case Field::LineoutMute: command = QStringLiteral("mixer lineout mute ") + boolean(); break;
    case Field::HeadphoneGain: command = QStringLiteral("mixer headphone gain ") + integer(); break;
    case Field::HeadphoneMute: command = QStringLiteral("mixer headphone mute ") + boolean(); break;
    case Field::FrontSpeakerMute: command = QStringLiteral("mixer front_speaker mute ") + boolean(); break;
    case Field::StartFrequencyCalibration: {
        // Each prelude must be acknowledged before the next write. Start's
        // acknowledgement is not completion; pll_done owns the result.
        const QPointer<FlexBackend> alive(this);
        const auto sink = m_setupSink;
        const auto finished = std::make_shared<bool>(false);
        const auto complete = [finished, completion](const RadioSetupReply& reply) {
            if (*finished) { return; }
            *finished = true;
            if (completion) { completion(reply); }
        };
        const auto referenceReplied = std::make_shared<bool>(false);
        const RadioSetupCompletion referenceReply =
            [alive, sink, current, complete, referenceReplied](const RadioSetupReply& reference) {
            if (*referenceReplied) { return; }
            *referenceReplied = true;
            if (!alive || (current && !current())) {
                complete({RadioSetupReply::Kind::Cancelled, -1, QStringLiteral("Original calibration session ended.")});
                return;
            }
            if (reference.kind != RadioSetupReply::Kind::Accepted) { complete(reference); return; }
            const auto resetReplied = std::make_shared<bool>(false);
            const RadioSetupCompletion resetReply =
                [alive, sink, current, complete, resetReplied](const RadioSetupReply& reset) {
                if (*resetReplied) { return; }
                *resetReplied = true;
                if (!alive || (current && !current())) {
                    complete({RadioSetupReply::Kind::Cancelled, -1, QStringLiteral("Original calibration session ended.")});
                    return;
                }
                if (reset.kind != RadioSetupReply::Kind::Accepted) { complete(reset); return; }
                if (!sink(QStringLiteral("radio pll_start"), complete)) {
                    complete({RadioSetupReply::Kind::Refused, -1, QStringLiteral("Calibration start not dispatched.")});
                }
            };
            if (!sink(QStringLiteral("radio set freq_error_ppb=0"), resetReply)) {
                complete({RadioSetupReply::Kind::Refused, -1, QStringLiteral("Calibration reset not dispatched.")});
            }
        };
        return sink(QStringLiteral("radio set cal_freq=")
                        + QString::number(std::get<double>(request.value), 'f', 6), referenceReply)
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    case Field::Count: return ReceiveDispatch::Unsupported;
    }
    const auto sink = m_setupSink;
    if (current && !current()) { return ReceiveDispatch::Unsupported; }
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestTxBand(const TxBandRequest& request,
                                           const RadioSetupCompletion& completion)
{
    using Field = TxBandRequest::Field;
    if (!request.valid() || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    QString key;
    QString verb = QStringLiteral("interlock");
    switch (request.field) {
    case Field::RfPowerPercent: verb = QStringLiteral("transmit"); key = QStringLiteral("rfpower"); break;
    case Field::TunePowerPercent: verb = QStringLiteral("transmit"); key = QStringLiteral("tunepower"); break;
    case Field::Inhibit: verb = QStringLiteral("transmit"); key = QStringLiteral("inhibit"); break;
    case Field::HardwareAlc: verb = QStringLiteral("transmit"); key = QStringLiteral("hwalc_enabled"); break;
    case Field::AccessoryTx: key = QStringLiteral("acc_tx_enabled"); break;
    case Field::RcaTxRequest: key = QStringLiteral("rca_txreq_enable"); break;
    case Field::AccessoryTxRequest: key = QStringLiteral("acc_txreq_enable"); break;
    case Field::Tx1: key = QStringLiteral("tx1_enabled"); break;
    case Field::Tx2: key = QStringLiteral("tx2_enabled"); break;
    case Field::Tx3: key = QStringLiteral("tx3_enabled"); break;
    case Field::Count: return ReceiveDispatch::Unsupported;
    }
    const int value = std::holds_alternative<int>(request.value)
        ? std::get<int>(request.value) : int(std::get<bool>(request.value));
    const auto sink = m_setupSink;
    return sink(QStringLiteral("%1 bandset %2 %3=%4").arg(verb).arg(request.bandId).arg(key).arg(value), completion)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestTransverter(const TransverterRequest& request,
                                                const RadioSetupCompletion& completion)
{
    using Field = TransverterRequest::Field;
    if (!request.valid() || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    QString command;
    if (request.field == Field::Create) { command = QStringLiteral("xvtr create"); }
    else if (request.field == Field::Remove) { command = QStringLiteral("xvtr remove %1").arg(request.id); }
    else {
        QString key;
        QString value;
        switch (request.field) {
        case Field::Name:
            key = QStringLiteral("name"); value = std::get<QString>(request.value).left(4);
            for (const QChar ch : value) {
                if (ch.isSpace() || ch == QLatin1Char('=') || ch == QLatin1Char('|')) { return ReceiveDispatch::Unsupported; }
            }
            break;
        case Field::RfFrequencyMhz: key = QStringLiteral("rf_freq"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
        case Field::IfFrequencyMhz: key = QStringLiteral("if_freq"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
        case Field::LoErrorMhz: key = QStringLiteral("lo_error"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
        case Field::RxGainDb: key = QStringLiteral("rx_gain"); value = QString::number(std::get<double>(request.value), 'f', 2); break;
        case Field::MaximumPowerDbm:
            key = QStringLiteral("max_power");
            value = QString::number(XvtrPolicy::clampMaxPowerDbm(std::get<double>(request.value),
                request.powerIfMhz, m_modelProvider ? m_modelProvider() : QString()), 'f', 2); break;
        case Field::RxOnly: key = QStringLiteral("rx_only"); value = QString::number(int(std::get<bool>(request.value))); break;
        case Field::Create: case Field::Remove: case Field::Count: return ReceiveDispatch::Unsupported;
        }
        command = QStringLiteral("xvtr set %1 %2=%3").arg(request.id).arg(key, value);
    }
    const auto sink = m_setupSink;
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestUsbCable(const UsbCableRequest& request)
{
    using Field = UsbCableRequest::Field;
    if (!request.valid() || !m_setupSink) { return ReceiveDispatch::Unsupported; }
    const auto token = [](const QString& text) {
        if (text.isEmpty() || text.size() > 128) { return false; }
        for (const QChar ch : text) {
            if (ch.unicode() > 127 || (!ch.isLetterOrNumber()
                && ch != QLatin1Char('_') && ch != QLatin1Char('-') && ch != QLatin1Char('/'))) { return false; }
        }
        return true;
    };
    if (!token(request.serial)) { return ReceiveDispatch::Unsupported; }
    const auto sink = m_setupSink;
    if (request.field == Field::Remove) {
        return sink(QStringLiteral("usb_cable remove %1").arg(request.serial), {})
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    const bool bitField = request.field == Field::Enabled || request.field == Field::Source
        || request.field == Field::SourceRxAntenna || request.field == Field::SourceTxAntenna
        || request.field == Field::SourceSlice || request.field == Field::ActiveHigh
        || request.field == Field::Band || request.field == Field::Output
        || request.field == Field::LowFrequencyMhz || request.field == Field::HighFrequencyMhz
        || request.field == Field::PttDependent || request.field == Field::PttDelayMs
        || request.field == Field::TxDelayMs;
    if (request.bit >= 0 && !bitField) { return ReceiveDispatch::Unsupported; }
    const bool requiresBit = request.field == Field::Output
        || request.field == Field::LowFrequencyMhz || request.field == Field::HighFrequencyMhz
        || request.field == Field::PttDependent || request.field == Field::PttDelayMs
        || request.field == Field::TxDelayMs;
    if (request.bit < 0 && requiresBit) { return ReceiveDispatch::Unsupported; }
    QString key;
    QString value;
    const auto boolean = [&] { return QString::number(int(std::get<bool>(request.value))); };
    switch (request.field) {
    case Field::Type:
        key = QStringLiteral("type"); value = std::get<QString>(request.value);
        if (!QStringList{"cat", "bcd", "vbcd", "bcd_vbcd", "bit", "ldpa", "passthrough"}.contains(value)) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::Name:
        key = QStringLiteral("name"); value = std::get<QString>(request.value);
        if (value.contains(QLatin1Char('|'))) { return ReceiveDispatch::Unsupported; }
        value.replace(QLatin1Char(' '), QChar(0x7f)); break;
    case Field::Enabled: key = QStringLiteral("enable"); value = boolean(); break;
    case Field::AutoReport: key = QStringLiteral("auto_report"); value = boolean(); break;
    case Field::Source:
        key = QStringLiteral("source"); value = std::get<QString>(request.value);
        if (!QStringList{"None", "tx_pan", "tx_slice", "active_slice", "tx_ant", "rx_ant", "ordinal_slice"}.contains(value)) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::SerialSpeed: {
        key = QStringLiteral("speed"); const int speed = std::get<int>(request.value);
        constexpr std::array speeds{300, 600, 1200, 2400, 4800, 9600, 14400, 19200,
                                   38400, 57600, 115200, 230400, 460800, 921600};
        if (std::find(speeds.begin(), speeds.end(), speed) == speeds.end()) { return ReceiveDispatch::Unsupported; }
        value = QString::number(speed); break;
    }
    case Field::DataBits: {
        key = QStringLiteral("data_bits"); const int bits = std::get<int>(request.value);
        if (bits != 7 && bits != 8) { return ReceiveDispatch::Unsupported; }
        value = QString::number(bits); break;
    }
    case Field::StopBits: {
        key = QStringLiteral("stop_bits"); const int bits = std::get<int>(request.value);
        if (bits != 1 && bits != 2) { return ReceiveDispatch::Unsupported; }
        value = QString::number(bits); break;
    }
    case Field::Parity:
        key = QStringLiteral("parity"); value = std::get<QString>(request.value);
        if (!QStringList{"none", "odd", "even", "mark", "space"}.contains(value)) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::FlowControl:
        key = QStringLiteral("flow_control"); value = std::get<QString>(request.value);
        if (!QStringList{"none", "rts_cts", "dtr_dsr", "xon_xoff"}.contains(value)) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::ActiveHigh:
        key = QStringLiteral("polarity"); value = std::get<bool>(request.value)
            ? QStringLiteral("active_high") : QStringLiteral("active_low"); break;
    case Field::Band:
        key = QStringLiteral("band"); value = std::get<QString>(request.value);
        if (!value.isEmpty() && !token(value)) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::Preamp: key = QStringLiteral("preamp"); value = boolean(); break;
    case Field::SourceRxAntenna: key = QStringLiteral("source_rx_ant"); value = std::get<QString>(request.value); break;
    case Field::SourceTxAntenna: key = QStringLiteral("source_tx_ant"); value = std::get<QString>(request.value); break;
    case Field::SourceSlice: key = QStringLiteral("source_slice"); value = std::get<QString>(request.value); break;
    case Field::Output:
        key = QStringLiteral("output"); value = std::get<QString>(request.value);
        if (value != QLatin1String("band") && value != QLatin1String("freq_range")) { return ReceiveDispatch::Unsupported; }
        break;
    case Field::LowFrequencyMhz: key = QStringLiteral("low_freq"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
    case Field::HighFrequencyMhz: key = QStringLiteral("high_freq"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
    case Field::PttDependent: key = QStringLiteral("ptt_dependent"); value = boolean(); break;
    case Field::PttDelayMs: key = QStringLiteral("ptt_delay"); value = QString::number(std::get<int>(request.value)); break;
    case Field::TxDelayMs: key = QStringLiteral("tx_delay"); value = QString::number(std::get<int>(request.value)); break;
    case Field::Remove: case Field::Count: return ReceiveDispatch::Unsupported;
    }
    if (std::holds_alternative<QString>(request.value)
        && request.field != Field::Name && request.field != Field::Band
        && !token(value)) { return ReceiveDispatch::Unsupported; }
    const QString command = request.bit < 0
        ? QStringLiteral("usb_cable set %1 %2=%3").arg(request.serial, key, value)
        : QStringLiteral("usb_cable setbit %1 %2 %3=%4").arg(request.serial).arg(request.bit).arg(key, value);
    return sink(command, {}) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
