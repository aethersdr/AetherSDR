#include "FlexBackend.h"

namespace AetherSDR {
namespace {
bool token(const QString& value)
{
    if (!controlTextValid(value) || value.size() > 64) { return false; }
    for (const QChar ch : value) {
        if (ch.unicode() > 127 || (!ch.isLetterOrNumber() && ch != QLatin1Char('_')
            && ch != QLatin1Char('-') && ch != QLatin1Char('/'))) { return false; }
    }
    return true;
}
}

ReceiveDispatch FlexBackend::requestSliceControl(int sliceId, const SliceControlRequest& request)
{
    using Field = SliceControlRequest::Field;
    if (sliceId < 0 || !request.valid()) { return ReceiveDispatch::Unsupported; }
    QString key;
    QString value;
    // FlexLib 4.2.18 Slice.cs: keep independent registers independent.
    switch (request.field) {
    case Field::TxAntenna:
        key = QStringLiteral("txant"); value = std::get<QString>(request.value); break;
    case Field::DaxChannel:
        key = QStringLiteral("dax"); value = QString::number(std::get<int>(request.value)); break;
    case Field::RttyMark:
        key = QStringLiteral("rtty_mark"); value = QString::number(std::get<int>(request.value)); break;
    case Field::RttyShift:
        key = QStringLiteral("rtty_shift"); value = QString::number(std::get<int>(request.value)); break;
    case Field::DiglOffset:
        key = QStringLiteral("digl_offset"); value = QString::number(std::get<int>(request.value)); break;
    case Field::DiguOffset:
        key = QStringLiteral("digu_offset"); value = QString::number(std::get<int>(request.value)); break;
    case Field::TxSlice:
        key = QStringLiteral("tx"); value = QString::number(int(std::get<bool>(request.value))); break;
    case Field::ActiveSlice:
        key = QStringLiteral("active"); value = QString::number(int(std::get<bool>(request.value))); break;
    case Field::Record:
        key = QStringLiteral("record"); value = QString::number(int(std::get<bool>(request.value))); break;
    case Field::Play:
        key = QStringLiteral("play"); value = QString::number(int(std::get<bool>(request.value))); break;
    case Field::FmToneMode:
        key = QStringLiteral("fm_tone_mode"); value = std::get<QString>(request.value); break;
    case Field::FmToneValue:
        key = QStringLiteral("fm_tone_value"); value = QString::number(std::get<double>(request.value), 'g', 12); break;
    case Field::RepeaterDirection:
        key = QStringLiteral("repeater_offset_dir"); value = std::get<QString>(request.value); break;
    case Field::RepeaterOffset:
        key = QStringLiteral("fm_repeater_offset_freq"); value = QString::number(std::get<double>(request.value) / 1.0e6, 'f', 6); break;
    case Field::TxOffset:
        key = QStringLiteral("tx_offset_freq"); value = QString::number(std::get<double>(request.value) / 1.0e6, 'f', 6); break;
    case Field::FmDeviation:
        key = QStringLiteral("fm_deviation"); value = QString::number(std::get<int>(request.value)); break;
    case Field::RfGain:
        key = QStringLiteral("rfgain"); value = QString::number(std::get<int>(request.value)); break;
    case Field::Diversity:
        key = QStringLiteral("diversity"); value = QString::number(int(std::get<bool>(request.value))); break;
    case Field::EscEnabled:
        key = QStringLiteral("esc"); value = std::get<bool>(request.value) ? QStringLiteral("on") : QStringLiteral("off"); break;
    case Field::EscGain:
        key = QStringLiteral("esc_gain"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
    case Field::EscPhase:
        key = QStringLiteral("esc_phase_shift"); value = QString::number(std::get<double>(request.value), 'f', 6); break;
    case Field::Rit: case Field::Xit: {
        const IncrementalTuning tuning = std::get<IncrementalTuning>(request.value);
        const QString prefix = request.field == Field::Rit ? QStringLiteral("rit") : QStringLiteral("xit");
        sendSlice(QStringLiteral("slice set %1 %2_on=%3 %2_freq=%4")
                      .arg(sliceId).arg(prefix).arg(int(tuning.enabled)).arg(tuning.hz));
        return ReceiveDispatch::Dispatched;
    }
    case Field::FmRxToneValue: case Field::FmDtcs: case Field::RepeaterRecall:
    case Field::Count: return ReceiveDispatch::Unsupported;
    }
    if (std::holds_alternative<QString>(request.value) && !token(value)) {
        return ReceiveDispatch::Unsupported;
    }
    sendSlice(QStringLiteral("slice set %1 %2=%3").arg(sliceId).arg(key, value));
    return ReceiveDispatch::Dispatched;
}

ReceiveDispatch FlexBackend::requestTransmitControl(const TransmitControlRequest& request,
                                                     TunePowerContext)
{
    using Field = TransmitControlRequest::Field;
    if (!request.valid()) { return ReceiveDispatch::Unsupported; }
    QString command;
    // These settings never replace setKeying/setTune/setAtu's fenced writer.
    switch (request.field) {
    case Field::RfPower:
        command = QStringLiteral("transmit set rfpower=") + QString::number(std::get<int>(request.value)); break;
    case Field::TunePower:
        command = QStringLiteral("transmit set tunepower=") + QString::number(std::get<int>(request.value)); break;
    case Field::TuneMode:
        command = QStringLiteral("transmit set tune_mode=") + std::get<QString>(request.value); break;
    case Field::MicInput:
        command = QStringLiteral("mic input ") + std::get<QString>(request.value); break;
    case Field::MicGain:
        command = QStringLiteral("transmit set miclevel=") + QString::number(std::get<int>(request.value)); break;
    case Field::MicAccessory:
        command = QStringLiteral("mic acc ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::Dax:
        command = QStringLiteral("transmit set dax=") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::MicBoost:
        command = QStringLiteral("mic boost ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::MicBias:
        command = QStringLiteral("mic bias ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::AmCarrier:
        command = QStringLiteral("transmit set am_carrier=") + QString::number(std::get<int>(request.value)); break;
    case Field::ExpanderEnabled:
        command = QStringLiteral("transmit set compander=") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::ExpanderLevel:
        command = QStringLiteral("transmit set compander_level=") + QString::number(std::get<int>(request.value)); break;
    case Field::CwSpeed:
        command = QStringLiteral("cw wpm ") + QString::number(std::get<int>(request.value)); break;
    case Field::CwPitch:
        command = QStringLiteral("cw pitch ") + QString::number(std::get<int>(request.value)); break;
    case Field::CwBreakIn:
        command = QStringLiteral("cw break_in ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::CwDelay:
        command = QStringLiteral("cw break_in_delay ") + QString::number(std::get<int>(request.value)); break;
    case Field::CwSidetone:
        command = QStringLiteral("cw sidetone ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::CwIambic:
        command = QStringLiteral("cw iambic ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::CwIambicMode:
        command = QStringLiteral("cw mode ") + QString::number(std::get<int>(request.value)); break;
    case Field::CwSwap:
        command = QStringLiteral("cw swap ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::CwlEnabled:
        command = QStringLiteral("cw cwl_enabled ") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::CwMonitorGain:
        command = QStringLiteral("transmit set mon_gain_cw=") + QString::number(std::get<int>(request.value)); break;
    case Field::CwMonitorPan:
        command = QStringLiteral("transmit set mon_pan_cw=") + QString::number(std::get<int>(request.value)); break;
    case Field::ApdEnabled:
        command = QStringLiteral("apd enable=") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::AtuMemories:
        command = QStringLiteral("atu set memories_enabled=") + QString::number(int(std::get<bool>(request.value))); break;
    case Field::ProcessorEnabled: case Field::ProcessorLevel: {
        const ProcessorSetting setting = std::get<ProcessorSetting>(request.value);
        if (request.field == Field::ProcessorLevel && setting.level > 2) {
            return ReceiveDispatch::Unsupported;
        }
        command = request.field == Field::ProcessorEnabled
            ? QStringLiteral("transmit set speech_processor_enable=%1").arg(int(setting.enabled))
            : QStringLiteral("transmit set speech_processor_level=%1").arg(setting.level);
        break;
    }
    case Field::VoxEnabled: case Field::VoxLevel: case Field::VoxDelay: {
        const VoxSetting setting = std::get<VoxSetting>(request.value);
        if (request.field == Field::VoxEnabled) {
            command = QStringLiteral("transmit set vox_enable=%1").arg(int(setting.enabled));
        } else if (request.field == Field::VoxLevel) {
            command = QStringLiteral("transmit set vox_level=%1").arg(setting.level);
        } else {
            command = QStringLiteral("transmit set vox_delay=%1").arg(setting.delay);
        }
        break;
    }
    case Field::MonitorEnabled: case Field::MonitorLevel: {
        const MonitorSetting setting = std::get<MonitorSetting>(request.value);
        command = request.field == Field::MonitorEnabled
            ? QStringLiteral("transmit set mon=%1").arg(int(setting.enabled))
            : QStringLiteral("transmit set mon_gain_sb=%1").arg(setting.level);
        break;
    }
    case Field::Filter: {
        const TxPassband filter = std::get<TxPassband>(request.value);
        command = QStringLiteral("transmit set filter_low=%1 filter_high=%2")
                      .arg(filter.lowHz).arg(filter.highHz);
        break;
    }
    case Field::TxProfile: case Field::MicProfile:
        command = QStringLiteral("profile %1 load \"%2\"")
                      .arg(request.field == Field::TxProfile ? QStringLiteral("tx") : QStringLiteral("mic"),
                           std::get<QString>(request.value));
        break;
    case Field::ApdSampler: {
        const ApdSamplerSetting sampler = std::get<ApdSamplerSetting>(request.value);
        if (!token(sampler.antenna) || !token(sampler.port)) { return ReceiveDispatch::Unsupported; }
        command = QStringLiteral("apd sampler tx_ant=%1 sample_port=%2")
                      .arg(sampler.antenna.toUpper(), sampler.port.toUpper());
        break;
    }
    case Field::ApdReset: command = QStringLiteral("apd reset"); break;
    case Field::AtuClear: command = QStringLiteral("atu clear"); break;
    case Field::Count: return ReceiveDispatch::Unsupported;
    }
    if (request.field == Field::MicInput && !token(std::get<QString>(request.value))) {
        return ReceiveDispatch::Unsupported;
    }
    if (request.field == Field::TuneMode
        && std::get<QString>(request.value) != QLatin1String("single_tone")
        && std::get<QString>(request.value) != QLatin1String("two_tone")) {
        return ReceiveDispatch::Unsupported;
    }
    send(command);
    return ReceiveDispatch::Dispatched;
}
} // namespace AetherSDR
