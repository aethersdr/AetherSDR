#include "DeviceDiagnostics.h"
#include "AppSettings.h"
#include "AudioEngine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QMediaDevices>
#include <QtGlobal>

#include <cmath>

namespace AetherSDR::DeviceDiagnostics {

namespace {

bool isSavedTrue(const QString& key, const QString& defaultValue = QStringLiteral("False"))
{
    return AppSettings::instance().value(key, defaultValue).toString() == QStringLiteral("True");
}

bool sameDevice(const QAudioDevice& lhs, const QAudioDevice& rhs)
{
    return !lhs.isNull() && !rhs.isNull() && lhs.id() == rhs.id();
}

QString sampleFormatName(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::Unknown: return QStringLiteral("Unknown");
    case QAudioFormat::UInt8:   return QStringLiteral("UInt8");
    case QAudioFormat::Int16:   return QStringLiteral("Int16");
    case QAudioFormat::Int32:   return QStringLiteral("Int32");
    case QAudioFormat::Float:   return QStringLiteral("Float");
    default:                    return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QJsonObject formatToJson(const QAudioFormat& format)
{
    QJsonObject obj;
    obj["sample_rate"] = format.sampleRate();
    obj["channel_count"] = format.channelCount();
    obj["sample_format"] = sampleFormatName(format.sampleFormat());
    return obj;
}

bool supportsFormat(const QAudioDevice& device,
                    int sampleRate,
                    int channelCount,
                    QAudioFormat::SampleFormat sampleFormat)
{
    if (device.isNull())
        return false;

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channelCount);
    format.setSampleFormat(sampleFormat);
    return device.isFormatSupported(format);
}

QString idFingerprint(const QByteArray& id)
{
    if (id.isEmpty())
        return {};

    return QString::fromLatin1(
        QCryptographicHash::hash(id, QCryptographicHash::Sha256).toHex().left(12));
}

QJsonObject deviceToJson(const QAudioDevice& device,
                         const QAudioDevice& selected,
                         const QAudioDevice& defaultDevice,
                         const QString& direction)
{
    QJsonObject obj;
    obj["direction"] = direction;
    obj["available"] = !device.isNull();
    if (device.isNull()) {
        obj["description"] = "Unavailable";
        obj["bus_type"] = "Unknown";
        obj["bus_type_source"] = "no device";
        return obj;
    }

    const AudioBusType bus = inferAudioBusType(device.description(), device.id());
    obj["description"] = device.description();
    obj["id_available"] = !device.id().isEmpty();
    obj["id_fingerprint"] = idFingerprint(device.id());
    obj["bus_type"] = bus.type;
    obj["bus_type_source"] = bus.source;
    obj["selected"] = sameDevice(device, selected);
    obj["default"] = sameDevice(device, defaultDevice);
    obj["minimum_sample_rate"] = device.minimumSampleRate();
    obj["maximum_sample_rate"] = device.maximumSampleRate();
    obj["minimum_channel_count"] = device.minimumChannelCount();
    obj["maximum_channel_count"] = device.maximumChannelCount();
    obj["preferred_format"] = formatToJson(device.preferredFormat());

    if (direction == QStringLiteral("output")) {
        obj["supports_24k_float_stereo"] = supportsFormat(device, 24000, 2, QAudioFormat::Float);
        obj["supports_48k_float_stereo"] = supportsFormat(device, 48000, 2, QAudioFormat::Float);
    } else {
        obj["supports_24k_int16_stereo"] = supportsFormat(device, 24000, 2, QAudioFormat::Int16);
        obj["supports_48k_int16_stereo"] = supportsFormat(device, 48000, 2, QAudioFormat::Int16);
        obj["supports_24k_int16_mono"] = supportsFormat(device, 24000, 1, QAudioFormat::Int16);
        obj["supports_48k_int16_mono"] = supportsFormat(device, 48000, 1, QAudioFormat::Int16);
    }

    return obj;
}

QJsonObject basicDeviceToJson(const QAudioDevice& device,
                              const QAudioDevice& selected,
                              const QAudioDevice& defaultDevice,
                              const QString& direction)
{
    QJsonObject obj;
    obj["direction"] = direction;
    obj["available"] = !device.isNull();
    if (device.isNull()) {
        obj["description"] = "Unavailable";
        obj["bus_type"] = "Unknown";
        obj["bus_type_source"] = "no device";
        return obj;
    }

    const AudioBusType bus = inferAudioBusType(device.description(), device.id());
    obj["description"] = device.description();
    obj["id_available"] = !device.id().isEmpty();
    obj["id_fingerprint"] = idFingerprint(device.id());
    obj["bus_type"] = bus.type;
    obj["bus_type_source"] = bus.source;
    obj["selected"] = sameDevice(device, selected);
    obj["default"] = sameDevice(device, defaultDevice);
    return obj;
}

QJsonArray deviceListToJson(const QList<QAudioDevice>& devices,
                            const QAudioDevice& selected,
                            const QAudioDevice& defaultDevice,
                            const QString& direction)
{
    QJsonArray array;
    for (const QAudioDevice& device : devices)
        array.append(deviceToJson(device, selected, defaultDevice, direction));
    return array;
}

bool devicePresent(const QList<QAudioDevice>& devices, const QAudioDevice& selected)
{
    for (const QAudioDevice& device : devices) {
        if (sameDevice(device, selected))
            return true;
    }
    return false;
}

bool deviceIdPresent(const QList<QAudioDevice>& devices, const QByteArray& id)
{
    if (id.isEmpty())
        return false;

    for (const QAudioDevice& device : devices) {
        if (device.id() == id)
            return true;
    }
    return false;
}

QString micRouteName(const QJsonObject& mic)
{
    if (!mic.contains(QStringLiteral("selection"))
        && !mic.contains(QStringLiteral("dax_on"))) {
        return QStringLiteral("Unknown (radio status not received)");
    }

    const QString selection = mic["selection"].toString();
    if (selection.compare(QStringLiteral("PC"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("PC audio input");
    if (mic["dax_on"].toBool())
        return QStringLiteral("Radio DAX audio");
    return QStringLiteral("Radio audio input");
}

} // namespace

QJsonObject buildAudioDevicesSnapshot(const AudioEngine* audio, const QJsonObject& snapshot)
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    const QAudioDevice defaultInput = QMediaDevices::defaultAudioInput();
    const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();

    const QAudioDevice selectedInput = (audio && !audio->inputDevice().isNull())
        ? audio->inputDevice()
        : defaultInput;
    const QAudioDevice selectedOutput = (audio && !audio->outputDevice().isNull())
        ? audio->outputDevice()
        : defaultOutput;
    const QByteArray savedInputId = AppSettings::instance()
        .value(QStringLiteral("AudioInputDeviceId"), QString()).toByteArray();
    const QByteArray savedOutputId = AppSettings::instance()
        .value(QStringLiteral("AudioOutputDeviceId"), QString()).toByteArray();

    QJsonObject obj;
    obj["available"] = audio != nullptr;
    obj["qt_multimedia"] = true;
    obj["input_device_count"] = inputs.size();
    obj["output_device_count"] = outputs.size();
    obj["saved_input_configured"] = !savedInputId.isEmpty();
    obj["saved_output_configured"] = !savedOutputId.isEmpty();
    obj["saved_input_present"] = deviceIdPresent(inputs, savedInputId);
    obj["saved_output_present"] = deviceIdPresent(outputs, savedOutputId);
    obj["selected_input_source"] = (audio && !audio->inputDevice().isNull()) ? "saved selection" : "system default";
    obj["selected_output_source"] = (audio && !audio->outputDevice().isNull()) ? "saved selection" : "system default";
    obj["selected_input_present"] = devicePresent(inputs, selectedInput);
    obj["selected_output_present"] = devicePresent(outputs, selectedOutput);
    obj["selected_input"] = deviceToJson(selectedInput, selectedInput, defaultInput, QStringLiteral("input"));
    obj["selected_output"] = deviceToJson(selectedOutput, selectedOutput, defaultOutput, QStringLiteral("output"));
    obj["default_input"] = deviceToJson(defaultInput, selectedInput, defaultInput, QStringLiteral("input"));
    obj["default_output"] = deviceToJson(defaultOutput, selectedOutput, defaultOutput, QStringLiteral("output"));
    obj["input_devices"] = deviceListToJson(inputs, selectedInput, defaultInput, QStringLiteral("input"));
    obj["output_devices"] = deviceListToJson(outputs, selectedOutput, defaultOutput, QStringLiteral("output"));

    const QJsonObject transmit = snapshot["transmit"].toObject();
    const QJsonObject mic = transmit["mic"].toObject();
    const QJsonObject radio = snapshot["radio"].toObject();
    const QJsonObject remoteAudioRx = radio["remote_audio_rx"].toObject();
    const bool pcAudioEnabled = isSavedTrue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));

    QJsonObject rxRoute;
    rxRoute["output"] = pcAudioEnabled ? "PC audio" : "Radio audio";
    rxRoute["source"] = "PcAudioEnabled app setting";
    rxRoute["selected_output_device"] = selectedOutput.description();
    rxRoute["radio_lineout_available"] = true;
    rxRoute["remote_audio_rx_stream_id"] = remoteAudioRx["stream_id"];
    rxRoute["remote_audio_rx_stream_id_known"] = remoteAudioRx["stream_id_known"];
    rxRoute["remote_audio_rx_create_pending"] = remoteAudioRx["create_pending"];
    rxRoute["remote_audio_rx_remove_requested"] = remoteAudioRx["remove_requested"];
    rxRoute["remote_audio_rx_status_seen"] = remoteAudioRx["status_seen"];
    rxRoute["remote_audio_rx_owned_by_us"] = remoteAudioRx["owned_by_us"];
    rxRoute["remote_audio_rx_expected"] = remoteAudioRx["stream_expected"];
    obj["rx_route"] = rxRoute;

    QJsonObject txRoute;
    txRoute["input"] = micRouteName(mic);
    txRoute["source"] = "transmit mic/DAX state";
    if (mic.contains(QStringLiteral("selection")))
        txRoute["mic_selection"] = mic["selection"].toString();
    if (mic.contains(QStringLiteral("dax_on")))
        txRoute["dax_on"] = mic["dax_on"].toBool();
    txRoute["selected_input_device"] = selectedInput.description();
    // Surface the active TX slice's id, mode, and per-slice DAX channel here
    // so the bundle's TX route summary has the same context a triager would
    // otherwise have to cross-reference from the per-slice section. WSJT-X /
    // JTDX issues frequently come down to a mismatch between the audio device
    // they're configured to use (DAX RESERVED CH N) and the TX slice's
    // `slice dax=M` — having both visible in one line makes that mismatch
    // obvious without re-reading every slice block. (#2885)
    QJsonValue txSliceId = QJsonValue::Null;
    QJsonValue txSliceMode = QJsonValue::Null;
    QJsonValue txSliceDaxChannel = QJsonValue::Null;
    const QJsonArray slices = snapshot["slices"].toArray();
    for (const QJsonValue& value : slices) {
        const QJsonObject slice = value.toObject();
        if (slice["tx_slice"].toBool()) {
            txSliceId = slice["slice_id"];
            txSliceMode = slice["mode"];
            txSliceDaxChannel = slice["digital"].toObject().value("dax_channel");
            break;
        }
    }
    txRoute["tx_slice_id"] = txSliceId;
    txRoute["tx_slice_mode"] = txSliceMode;
    txRoute["tx_slice_dax_channel"] = txSliceDaxChannel;
    obj["tx_route"] = txRoute;

    QJsonObject volumes;
    volumes["pc_audio_enabled"] = pcAudioEnabled;
    volumes["pc_audio_muted"] = isSavedTrue(QStringLiteral("PcAudioMuted"));
    volumes["pc_master_volume_pct"] = AppSettings::instance().value("MasterVolume", "100").toInt();
    volumes["pc_mic_gain_pct"] = AppSettings::instance().value("PcMicGain", 100).toInt();

    const QJsonObject radioAudio = radio["audio_outputs"].toObject();
    volumes["radio_lineout_gain"] = radioAudio["lineout_gain"];
    volumes["radio_lineout_mute"] = radioAudio["lineout_mute"];
    volumes["radio_headphone_gain"] = radioAudio["headphone_gain"];
    volumes["radio_headphone_mute"] = radioAudio["headphone_mute"];
    volumes["radio_front_speaker_mute"] = radioAudio["front_speaker_mute"];

    if (audio) {
        volumes["engine_rx_volume_pct"] = qRound(audio->rxVolume() * 100.0f);
        volumes["engine_rx_muted"] = audio->isMuted();
        volumes["engine_rx_boost"] = audio->rxBoost();
        obj["rx_streaming"] = audio->isRxStreaming();
        obj["tx_streaming"] = audio->isTxStreaming();
        obj["dax_tx_mode"] = audio->isDaxTxMode();
        obj["dax_tx_use_radio_route"] = audio->daxTxUseRadioRoute();
    } else {
        volumes["engine_rx_volume_pct"] = QJsonValue();
        volumes["engine_rx_muted"] = QJsonValue();
        volumes["engine_rx_boost"] = QJsonValue();
        obj["rx_streaming"] = QJsonValue();
        obj["tx_streaming"] = QJsonValue();
        obj["dax_tx_mode"] = QJsonValue();
        obj["dax_tx_use_radio_route"] = QJsonValue();
    }

    obj["volumes"] = volumes;
    return obj;
}

QJsonObject buildAudioStartupSnapshot(const AudioEngine* audio, const QJsonObject& snapshot)
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    const QAudioDevice defaultInput = QMediaDevices::defaultAudioInput();
    const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();

    const QAudioDevice selectedInput = (audio && !audio->inputDevice().isNull())
        ? audio->inputDevice()
        : defaultInput;
    const QAudioDevice selectedOutput = (audio && !audio->outputDevice().isNull())
        ? audio->outputDevice()
        : defaultOutput;
    const QByteArray savedInputId = AppSettings::instance()
        .value(QStringLiteral("AudioInputDeviceId"), QString()).toByteArray();
    const QByteArray savedOutputId = AppSettings::instance()
        .value(QStringLiteral("AudioOutputDeviceId"), QString()).toByteArray();

    QJsonObject obj;
    obj["available"] = audio != nullptr;
    obj["qt_multimedia"] = true;
    obj["input_device_count"] = inputs.size();
    obj["output_device_count"] = outputs.size();
    obj["saved_input_configured"] = !savedInputId.isEmpty();
    obj["saved_output_configured"] = !savedOutputId.isEmpty();
    obj["saved_input_present"] = deviceIdPresent(inputs, savedInputId);
    obj["saved_output_present"] = deviceIdPresent(outputs, savedOutputId);
    obj["selected_input_source"] = (audio && !audio->inputDevice().isNull()) ? "saved selection" : "system default";
    obj["selected_output_source"] = (audio && !audio->outputDevice().isNull()) ? "saved selection" : "system default";
    obj["selected_input_present"] = devicePresent(inputs, selectedInput);
    obj["selected_output_present"] = devicePresent(outputs, selectedOutput);
    obj["selected_input"] = basicDeviceToJson(selectedInput, selectedInput, defaultInput, QStringLiteral("input"));
    obj["selected_output"] = basicDeviceToJson(selectedOutput, selectedOutput, defaultOutput, QStringLiteral("output"));
    obj["default_input"] = basicDeviceToJson(defaultInput, selectedInput, defaultInput, QStringLiteral("input"));
    obj["default_output"] = basicDeviceToJson(defaultOutput, selectedOutput, defaultOutput, QStringLiteral("output"));

    const QJsonObject transmit = snapshot["transmit"].toObject();
    const QJsonObject mic = transmit["mic"].toObject();
    const bool pcAudioEnabled = isSavedTrue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));

    QJsonObject rxRoute;
    rxRoute["output"] = pcAudioEnabled ? "PC audio" : "Radio audio";
    rxRoute["source"] = "PcAudioEnabled app setting";
    rxRoute["selected_output_device"] = selectedOutput.description();
    obj["rx_route"] = rxRoute;

    QJsonObject txRoute;
    txRoute["input"] = micRouteName(mic);
    txRoute["source"] = "transmit mic/DAX state";
    if (mic.contains(QStringLiteral("selection")))
        txRoute["mic_selection"] = mic["selection"].toString();
    if (mic.contains(QStringLiteral("dax_on")))
        txRoute["dax_on"] = mic["dax_on"].toBool();
    txRoute["selected_input_device"] = selectedInput.description();
    obj["tx_route"] = txRoute;

    QJsonObject volumes;
    volumes["pc_audio_enabled"] = pcAudioEnabled;
    volumes["pc_audio_muted"] = isSavedTrue(QStringLiteral("PcAudioMuted"));
    volumes["pc_master_volume_pct"] = AppSettings::instance().value("MasterVolume", "100").toInt();
    volumes["pc_mic_gain_pct"] = AppSettings::instance().value("PcMicGain", 100).toInt();

    if (audio) {
        volumes["engine_rx_volume_pct"] = qRound(audio->rxVolume() * 100.0f);
        volumes["engine_rx_muted"] = audio->isMuted();
        volumes["engine_rx_boost"] = audio->rxBoost();
        obj["rx_streaming"] = audio->isRxStreaming();
        obj["tx_streaming"] = audio->isTxStreaming();
        obj["dax_tx_mode"] = audio->isDaxTxMode();
        obj["dax_tx_use_radio_route"] = audio->daxTxUseRadioRoute();
    } else {
        volumes["engine_rx_volume_pct"] = QJsonValue();
        volumes["engine_rx_muted"] = QJsonValue();
        volumes["engine_rx_boost"] = QJsonValue();
        obj["rx_streaming"] = QJsonValue();
        obj["tx_streaming"] = QJsonValue();
        obj["dax_tx_mode"] = QJsonValue();
        obj["dax_tx_use_radio_route"] = QJsonValue();
    }

    obj["volumes"] = volumes;
    return obj;
}

void annotateSliceAudioRoutes(QJsonObject* snapshot, const QJsonObject& audioDevices)
{
    if (!snapshot)
        return;

    const QJsonObject rxRoute = audioDevices["rx_route"].toObject();
    const QJsonObject selectedOutput = audioDevices["selected_output"].toObject();

    QJsonArray updatedSlices;
    const QJsonArray slices = (*snapshot)["slices"].toArray();
    for (const QJsonValue& value : slices) {
        QJsonObject slice = value.toObject();
        QJsonObject audio = slice["audio"].toObject();
        audio["rx_route"] = rxRoute["output"].toString("Unknown");
        audio["rx_route_source"] = rxRoute["source"].toString();
        audio["pc_audio_enabled"] = audioDevices["volumes"].toObject()["pc_audio_enabled"].toBool();
        audio["selected_output_device"] = selectedOutput["description"].toString("Unavailable");
        audio["selected_output_bus_type"] = selectedOutput["bus_type"].toString("Unknown");
        slice["audio"] = audio;
        updatedSlices.append(slice);
    }

    (*snapshot)["slices"] = updatedSlices;
}

} // namespace AetherSDR::DeviceDiagnostics
