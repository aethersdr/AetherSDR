#include "SliceTroubleshootingDialog.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/DeviceDiagnostics.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QStringList>
#include <utility>

namespace AetherSDR {

namespace {

QString yesNo(bool value)
{
    return value ? "Yes" : "No";
}

QString orPlaceholder(const QString& value, const QString& placeholder = "n/a")
{
    return value.trimmed().isEmpty() ? placeholder : value;
}

QString joinArray(const QJsonArray& array, const QString& emptyText = "(none)")
{
    QStringList values;
    values.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (value.isString())
            values << value.toString();
        else if (value.isDouble())
            values << QString::number(value.toDouble());
        else if (value.isBool())
            values << (value.toBool() ? "true" : "false");
    }
    return values.isEmpty() ? emptyText : values.join(", ");
}

QString formatNumber(const QJsonValue& value, int decimals = 2, const QString& suffix = {})
{
    if (!value.isDouble())
        return QString("n/a");

    QString text = QString::number(value.toDouble(), 'f', decimals);
    if (!suffix.isEmpty())
        text += suffix;
    return text;
}

QString formatMeterBullet(const QJsonObject& meter, const QString& prefix = "- ")
{
    const QString valueText = meter["has_value"].toBool()
        ? formatNumber(meter["value"], 2)
        : QString("n/a");
    const QString unit = meter["unit"].toString();
    const QString range = QString("[%1, %2]")
        .arg(QString::number(meter["low"].toDouble(), 'f', 2))
        .arg(QString::number(meter["high"].toDouble(), 'f', 2));

    QString line = QString("%1[%2] `%3/%4 %5` = `%6%7` range `%8`")
        .arg(prefix)
        .arg(meter["index"].toInt())
        .arg(orPlaceholder(meter["source"].toString()))
        .arg(meter["source_index"].toInt())
        .arg(orPlaceholder(meter["name"].toString()))
        .arg(valueText)
        .arg(unit.isEmpty() ? QString() : QString(" %1").arg(unit))
        .arg(range);

    const QString description = meter["description"].toString().trimmed();
    if (!description.isEmpty())
        line += QString(" — %1").arg(description);
    return line;
}

QString formatPanBullet(const QJsonObject& pan)
{
    return QString(
        "- `%1` active `%2`, center `%3 MHz`, bandwidth `%4 MHz`, RF gain `%5`, "
        "preamp `%6`, WNB `%7` (`%8`), waterfall `%9`")
        .arg(orPlaceholder(pan["pan_id"].toString()))
        .arg(yesNo(pan["active"].toBool()))
        .arg(formatNumber(pan["center_mhz"], 6))
        .arg(formatNumber(pan["bandwidth_mhz"], 6))
        .arg(formatNumber(pan["rf_gain"], 0))
        .arg(orPlaceholder(pan["preamp"].toString()))
        .arg(yesNo(pan["wnb_active"].toBool()))
        .arg(formatNumber(pan["wnb_level"], 0))
        .arg(orPlaceholder(pan["waterfall_id"].toString()));
}

QString formatXvtrBullet(const QJsonObject& xvtr)
{
    return QString(
        "- `%1` idx `%2`, order `%3`, RF `%4 MHz`, IF `%5 MHz`, offset `%6 MHz`, "
        "valid `%7`, has is_valid `%8`, rx only `%9`, max power `%10`")
        .arg(orPlaceholder(xvtr["name"].toString()))
        .arg(xvtr["index"].toInt())
        .arg(xvtr["order"].toInt())
        .arg(formatNumber(xvtr["rf_freq_mhz"], 6))
        .arg(formatNumber(xvtr["if_freq_mhz"], 6))
        .arg(formatNumber(xvtr["offset_mhz"], 6))
        .arg(yesNo(xvtr["is_valid"].toBool()))
        .arg(yesNo(xvtr["has_is_valid"].toBool()))
        .arg(yesNo(xvtr["rx_only"].toBool()))
        .arg(formatNumber(xvtr["max_power"], 2));
}

QString formatClientBullet(const QJsonObject& client)
{
    return QString("- `%1`: program `%2`, owns TX `%3`, local PTT `%4`, TX ant `%5`, TX freq `%6 MHz`")
        .arg(orPlaceholder(client["role"].toString()))
        .arg(orPlaceholder(client["program"].toString()))
        .arg(yesNo(client["owns_tx"].toBool()))
        .arg(yesNo(client["local_ptt"].toBool()))
        .arg(orPlaceholder(client["tx_antenna"].toString()))
        .arg(formatNumber(client["tx_freq_mhz"], 6));
}

QString formatBoolValue(const QJsonValue& value)
{
    return value.isBool() ? yesNo(value.toBool()) : QStringLiteral("n/a");
}

QString formatPercentValue(const QJsonValue& value)
{
    return value.isDouble() ? QString("%1%").arg(qRound(value.toDouble())) : QStringLiteral("n/a");
}

QString formatAudioDeviceShort(const QJsonObject& device)
{
    const QString description = orPlaceholder(device["description"].toString(), "Unavailable");
    const QString bus = orPlaceholder(device["bus_type"].toString(), "Unknown");
    return QString("`%1` (bus `%2`)").arg(description, bus);
}

QString formatAudioSupport(const QJsonObject& device)
{
    if (device["direction"].toString() == QStringLiteral("output")) {
        return QString("24k float `%1`, 48k float `%2`")
            .arg(yesNo(device["supports_24k_float_stereo"].toBool()))
            .arg(yesNo(device["supports_48k_float_stereo"].toBool()));
    }

    return QString("24k stereo `%1`, 48k stereo `%2`, 24k mono `%3`, 48k mono `%4`")
        .arg(yesNo(device["supports_24k_int16_stereo"].toBool()))
        .arg(yesNo(device["supports_48k_int16_stereo"].toBool()))
        .arg(yesNo(device["supports_24k_int16_mono"].toBool()))
        .arg(yesNo(device["supports_48k_int16_mono"].toBool()));
}

QString formatAudioDeviceBullet(const QJsonObject& device, const QString& prefix = "- ")
{
    QStringList flags;
    if (device["selected"].toBool())
        flags << "selected";
    if (device["default"].toBool())
        flags << "default";
    if (flags.isEmpty())
        flags << "available";

    const QJsonObject preferred = device["preferred_format"].toObject();
    return QString("%1`%2` (%3): bus `%4`, rates `%5-%6 Hz`, channels `%7-%8`, preferred `%9 Hz/%10 ch/%11`, %12")
        .arg(prefix)
        .arg(orPlaceholder(device["description"].toString(), "Unavailable"))
        .arg(flags.join(", "))
        .arg(orPlaceholder(device["bus_type"].toString(), "Unknown"))
        .arg(device["minimum_sample_rate"].toInt())
        .arg(device["maximum_sample_rate"].toInt())
        .arg(device["minimum_channel_count"].toInt())
        .arg(device["maximum_channel_count"].toInt())
        .arg(preferred["sample_rate"].toInt())
        .arg(preferred["channel_count"].toInt())
        .arg(orPlaceholder(preferred["sample_format"].toString(), "Unknown"))
        .arg(formatAudioSupport(device));
}

QString formatControlDeviceBullet(const QJsonObject& device)
{
    return QString("- `%1`: active `%2`, active for slice `%3`, bus `%4`, target slice `%5`, detail `%6`")
        .arg(orPlaceholder(device["type"].toString()))
        .arg(yesNo(device["active"].toBool()))
        .arg(yesNo(device["active_for_current_slice"].toBool()))
        .arg(orPlaceholder(device["bus_type"].toString(), "Unknown"))
        .arg(device.contains("target_slice_id") && device["target_slice_id"].isDouble()
                 ? QString::number(device["target_slice_id"].toInt())
                 : QStringLiteral("n/a"))
        .arg(orPlaceholder(device["detail"].toString(), "n/a"));
}

QString formatMidiBindingBullet(const QJsonObject& binding, const QString& prefix = "  - ")
{
    return QString("%1`%2` -> `%3` (`%4`, channel `%5`, relative `%6`, inverted `%7`, scope `%8`)")
        .arg(prefix)
        .arg(orPlaceholder(binding["source"].toString()))
        .arg(orPlaceholder(binding["param_name"].toString(), binding["param_id"].toString()))
        .arg(orPlaceholder(binding["message_type"].toString()))
        .arg(orPlaceholder(binding["channel"].toString()))
        .arg(yesNo(binding["relative"].toBool()))
        .arg(yesNo(binding["inverted"].toBool()))
        .arg(orPlaceholder(binding["scope"].toString(), "unknown"));
}

QString formatTxBandBullet(const QJsonObject& band)
{
    return QString("- `%1` (ID `%2`): RF `%3`, tune `%4`, inhibit `%5`, HWALC `%6`, "
                   "ACC TX `%7`, ACC TX Req `%8`, RCA TX Req `%9`, TX1 `%10`, TX2 `%11`, TX3 `%12`")
        .arg(orPlaceholder(band["band_name"].toString()))
        .arg(band["band_id"].toInt())
        .arg(formatNumber(band["rf_power"], 0))
        .arg(formatNumber(band["tune_power"], 0))
        .arg(yesNo(band["inhibit"].toBool()))
        .arg(yesNo(band["hw_alc"].toBool()))
        .arg(yesNo(band["acc_tx"].toBool()))
        .arg(yesNo(band["acc_tx_req"].toBool()))
        .arg(yesNo(band["rca_tx_req"].toBool()))
        .arg(yesNo(band["tx1"].toBool()))
        .arg(yesNo(band["tx2"].toBool()))
        .arg(yesNo(band["tx3"].toBool()));
}

QString nr2GainMethodName(int id)
{
    static const char* kNames[] = {"Linear", "Log", "Gamma", "Trained"};
    return (id >= 0 && id < 4) ? kNames[id] : QString("Unknown (%1)").arg(id);
}

QString nr2NpeMethodName(int id)
{
    static const char* kNames[] = {"OSMS", "MMSE", "NSTAT"};
    return (id >= 0 && id < 3) ? kNames[id] : QString("Unknown (%1)").arg(id);
}

QString nr4MethodName(int id)
{
    static const char* kNames[] = {"SPP-MMSE", "Brandt", "Martin"};
    return (id >= 0 && id < 3) ? kNames[id] : QString("Unknown (%1)").arg(id);
}

QJsonObject buildClientDspSnapshot(const AudioEngine* audio)
{
    auto& settings = AppSettings::instance();

    const int nr2GainMethod = settings.value("NR2GainMethod", "2").toInt();
    const int nr2NpeMethod = settings.value("NR2NpeMethod", "0").toInt();
    const int nr4MethodId = settings.value("NR4NoiseEstimationMethod", "0").toInt();

    QJsonObject nr2;
    nr2["enabled"] = audio ? audio->nr2Enabled() : false;
    nr2["gain_method_id"] = nr2GainMethod;
    nr2["gain_method"] = nr2GainMethodName(nr2GainMethod);
    nr2["npe_method_id"] = nr2NpeMethod;
    nr2["npe_method"] = nr2NpeMethodName(nr2NpeMethod);
    nr2["ae_filter"] = settings.value("NR2AeFilter", "True").toString() == "True";
    nr2["gain_max"] = settings.value("NR2GainMax", "1.50").toFloat();
    nr2["gain_smooth"] = settings.value("NR2GainSmooth", "0.85").toFloat();
    nr2["qspp"] = settings.value("NR2Qspp", "0.20").toFloat();

    QJsonObject nr4;
    nr4["enabled"] = audio ? audio->nr4Enabled() : false;
    nr4["noise_method_id"] = nr4MethodId;
    nr4["noise_method"] = nr4MethodName(nr4MethodId);
    nr4["adaptive_noise"] = settings.value("NR4AdaptiveNoise", "True").toString() == "True";
    nr4["reduction_db"] = settings.value("NR4ReductionAmount", "10.0").toFloat();
    nr4["smoothing_pct"] = settings.value("NR4SmoothingFactor", "0.0").toFloat();
    nr4["whitening_pct"] = settings.value("NR4WhiteningFactor", "0.0").toFloat();
    nr4["masking_depth"] = settings.value("NR4MaskingDepth", "0.50").toFloat();
    nr4["suppression_strength"] = settings.value("NR4SuppressionStrength", "0.50").toFloat();

    QJsonObject rn2;
    rn2["enabled"] = audio ? audio->rn2Enabled() : false;
    rn2["parameters"] = "none";

    QJsonObject dfnr;
    dfnr["enabled"] = audio ? audio->dfnrEnabled() : false;
    dfnr["atten_limit_db"] = settings.value("DfnrAttenLimit", "100").toFloat();
    dfnr["post_filter_beta"] = settings.value("DfnrPostFilterBeta", "0.0").toFloat();

    QJsonObject clientDsp;
    clientDsp["available"] = audio != nullptr;
    clientDsp["nr2"] = nr2;
    clientDsp["nr4"] = nr4;
    clientDsp["rn2"] = rn2;
    clientDsp["dfnr"] = dfnr;
    return clientDsp;
}

} // namespace

SliceTroubleshootingDialog::SliceTroubleshootingDialog(RadioModel* model,
                                                       AudioEngine* audio,
                                                       QWidget* parent,
                                                       SnapshotProvider controlDevicesProvider)
    : QDialog(parent)
    , m_model(model)
    , m_audio(audio)
    , m_controlDevicesProvider(std::move(controlDevicesProvider))
{
    setWindowTitle("Slice Troubleshooting");
    setMinimumSize(920, 720);
    resize(980, 760);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    auto* intro = new QLabel(
        "Capture AetherSDR's current in-memory radio, panadapter, slice, and meter state. "
        "Use <b>Issue Summary</b> for GitHub reports and <b>JSON</b> for AI-assisted troubleshooting. "
        "This dialog does not re-query the radio.");
    intro->setWordWrap(true);
    intro->setStyleSheet("QLabel { color: #c8d8e8; }");
    root->addWidget(intro);

    auto* tabs = new QTabWidget;
    m_summaryView = new QPlainTextEdit;
    m_summaryView->setReadOnly(true);
    m_summaryView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_summaryView->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #d6e4f2;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "}");
    tabs->addTab(m_summaryView, "Issue Summary");

    m_jsonView = new QPlainTextEdit;
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_jsonView->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #9ed4ff;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "}");
    tabs->addTab(m_jsonView, "JSON");
    root->addWidget(tabs, 1);

    auto* footer = new QHBoxLayout;
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
    footer->addWidget(m_statusLabel, 1);

    auto* refreshBtn = new QPushButton("Refresh Snapshot");
    auto* copySummaryBtn = new QPushButton("Copy Summary");
    auto* copyJsonBtn = new QPushButton("Copy JSON");
    auto* exportJsonBtn = new QPushButton("Export JSON...");
    auto* closeBtn = new QPushButton("Close");

    footer->addWidget(refreshBtn);
    footer->addWidget(copySummaryBtn);
    footer->addWidget(copyJsonBtn);
    footer->addWidget(exportJsonBtn);
    footer->addWidget(closeBtn);
    root->addLayout(footer);

    connect(refreshBtn, &QPushButton::clicked, this, [this] { refreshSnapshot(); });
    connect(copySummaryBtn, &QPushButton::clicked, this, [this] { copySummary(); });
    connect(copyJsonBtn, &QPushButton::clicked, this, [this] { copyJson(); });
    connect(exportJsonBtn, &QPushButton::clicked, this, [this] { exportJson(); });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    refreshSnapshot();
}

void SliceTroubleshootingDialog::refreshSnapshot()
{
    if (m_model)
        m_snapshot = m_model->troubleshootingSnapshot();
    else
        m_snapshot = QJsonObject{};

    QJsonObject ui;
    ui["controls_locked"] = ::ControlsLock::isLocked();
    m_snapshot["ui"] = ui;
    m_snapshot["client_dsp"] = buildClientDspSnapshot(m_audio);

    const QJsonObject audioDevices = DeviceDiagnostics::buildAudioDevicesSnapshot(m_audio, m_snapshot);
    m_snapshot["audio_devices"] = audioDevices;
    DeviceDiagnostics::annotateSliceAudioRoutes(&m_snapshot, audioDevices);

    m_snapshot["control_devices"] = m_controlDevicesProvider
        ? m_controlDevicesProvider()
        : QJsonObject{
            {"available", false},
            {"note", "No control device provider registered."},
            {"devices", QJsonArray{}}
        };
    m_snapshot["schema_version"] = 3;

    m_summaryText = buildSummary(m_snapshot);
    m_jsonText = QString::fromUtf8(QJsonDocument(m_snapshot).toJson(QJsonDocument::Indented));

    m_summaryView->setPlainText(m_summaryText);
    m_jsonView->setPlainText(m_jsonText);

    const QJsonObject counts = m_snapshot["counts"].toObject();
    setStatusMessage(QString("Snapshot refreshed: %1 slice(s), %2 global meter(s), %3 total meter(s).")
                         .arg(counts["slices"].toInt())
                         .arg(counts["global_meters"].toInt())
                         .arg(counts["meters_total"].toInt()));
}

void SliceTroubleshootingDialog::copySummary()
{
    QApplication::clipboard()->setText(m_summaryText);
    setStatusMessage("Issue summary copied to clipboard.");
}

void SliceTroubleshootingDialog::copyJson()
{
    QApplication::clipboard()->setText(m_jsonText);
    setStatusMessage("JSON copied to clipboard.");
}

void SliceTroubleshootingDialog::exportJson()
{
    const QString defaultPath = QDir::homePath() + QString("/aethersdr-slice-troubleshooting-%1.json")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Export Slice Troubleshooting JSON",
        defaultPath,
        "JSON Files (*.json)");

    if (path.isEmpty())
        return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatusMessage(QString("Unable to write `%1`.").arg(path));
        return;
    }

    if (file.write(m_jsonText.toUtf8()) < 0 || !file.commit()) {
        setStatusMessage(QString("Failed to export JSON to `%1`.").arg(path));
        return;
    }

    setStatusMessage(QString("Exported JSON to `%1`.").arg(path));
}

void SliceTroubleshootingDialog::setStatusMessage(const QString& message)
{
    m_statusLabel->setText(message);
}

QString SliceTroubleshootingDialog::buildSummary(const QJsonObject& snapshot)
{
    QStringList lines;

    const QJsonObject app = snapshot["app"].toObject();
    const QJsonObject ui = snapshot["ui"].toObject();
    const QJsonObject radio = snapshot["radio"].toObject();
    const QJsonObject network = radio["network"].toObject();
    const QJsonObject ownership = radio["ownership"].toObject();
    const QJsonObject oscillator = radio["oscillator"].toObject();
    const QJsonObject audioOutputs = radio["audio_outputs"].toObject();
    const QJsonObject remoteAudioRx = radio["remote_audio_rx"].toObject();
    const QJsonObject filterSharpness = radio["filter_sharpness"].toObject();
    const QJsonObject telemetry = radio["telemetry"].toObject();
    const QJsonObject amplifier = radio["amplifier"].toObject();
    const QJsonObject transmit = snapshot["transmit"].toObject();
    const QJsonObject txPower = transmit["power"].toObject();
    const QJsonObject mic = transmit["mic"].toObject();
    const QJsonObject vox = transmit["vox"].toObject();
    const QJsonObject cw = transmit["cw"].toObject();
    const QJsonObject interlock = transmit["interlock"].toObject();
    const QJsonObject atu = transmit["atu"].toObject();
    const QJsonObject apd = transmit["apd"].toObject();
    const QJsonObject profiles = transmit["profiles"].toObject();
    const QJsonObject counts = snapshot["counts"].toObject();
    const QJsonObject clientDsp = snapshot["client_dsp"].toObject();
    const QJsonObject nr2 = clientDsp["nr2"].toObject();
    const QJsonObject nr4 = clientDsp["nr4"].toObject();
    const QJsonObject rn2 = clientDsp["rn2"].toObject();
    const QJsonObject dfnr = clientDsp["dfnr"].toObject();
    const QJsonObject audioDevices = snapshot["audio_devices"].toObject();
    const QJsonObject audioVolumes = audioDevices["volumes"].toObject();
    const QJsonObject rxRoute = audioDevices["rx_route"].toObject();
    const QJsonObject txRoute = audioDevices["tx_route"].toObject();
    const QJsonObject controlDevices = snapshot["control_devices"].toObject();

    lines << "# Slice Troubleshooting Snapshot";
    lines << "";
    lines << "> Captured from AetherSDR's in-memory models and cached meter values.";
    lines << "> This report does not query the radio directly.";
    lines << "> Client DSP enable state comes from the live audio engine; parameter values come from saved app settings.";
    lines << "> Privacy filter is enabled: radio name, nickname, callsign, serials, MAC/IPs, GPS data, and client station names are omitted.";
    lines << "";
    lines << QString("- Captured: `%1`").arg(orPlaceholder(snapshot["captured_at"].toString()));
    lines << QString("- App: `%1 %2` on `%3` (`Qt %4`, `%5`)")
                 .arg(orPlaceholder(app["name"].toString(), "AetherSDR"))
                 .arg(orPlaceholder(app["version"].toString()))
                 .arg(orPlaceholder(app["os"].toString()))
                 .arg(orPlaceholder(app["qt_version"].toString()))
                 .arg(orPlaceholder(app["cpu_arch"].toString()));
    lines << QString("- Radio: model `%1`, software `%2`, protocol `%3`")
                 .arg(orPlaceholder(radio["model"].toString()))
                 .arg(orPlaceholder(radio["software_version"].toString()))
                 .arg(orPlaceholder(radio["protocol_version"].toString()));
    lines << QString("- Connection: `%1`, connected `%2`, network `%3`, RTT `%4 ms` (max `%5 ms`)")
                 .arg(orPlaceholder(radio["transport"].toString()))
                 .arg(yesNo(radio["connected"].toBool()))
                 .arg(orPlaceholder(network["quality"].toString()))
                 .arg(network["last_ping_rtt_ms"].toInt())
                 .arg(network["max_ping_rtt_ms"].toInt());
    lines << QString("- Ownership: TX owned by us `%1`, multiple clients present `%2`")
                 .arg(yesNo(ownership["tx_owned_by_us"].toBool()))
                 .arg(yesNo(ownership["multiple_clients_present"].toBool()));
    lines << QString("- Profiles: global count `%1` (active set `%2`), TX count `%3` (active set `%4`), mic count `%5` (active set `%6`)")
                 .arg(radio["global_profile_count"].toInt())
                 .arg(yesNo(radio["active_global_profile_set"].toBool()))
                 .arg(profiles["tx_profile_count"].toInt())
                 .arg(yesNo(profiles["active_tx_profile_set"].toBool()))
                 .arg(profiles["mic_profile_count"].toInt())
                 .arg(yesNo(profiles["active_mic_profile_set"].toBool()));
    lines << QString("- Counts: `%1` slice(s), `%2` panadapter(s), `%3` total meter(s), `%4` slice meter(s)")
                 .arg(counts["slices"].toInt())
                 .arg(counts["panadapters"].toInt())
                 .arg(counts["meters_total"].toInt())
                 .arg(counts["slice_meters"].toInt());
    lines << "";

    lines << "## App UI State";
    lines << QString("- Global LCK: `%1`")
                 .arg(yesNo(ui["controls_locked"].toBool()));
    lines << "";

    lines << "## Global Radio State";
    lines << QString("- Region / Options: `%1` / `%2`")
                 .arg(orPlaceholder(radio["region"].toString()))
                 .arg(orPlaceholder(radio["radio_options"].toString()));
    lines << QString("- Antennas: `%1`").arg(joinArray(radio["antenna_list"].toArray()));
    lines << QString("- Owned slice IDs: `%1`").arg(joinArray(radio["owned_slice_ids"].toArray()));
    lines << QString("- Audio outputs: lineout `%1` (mute `%2`), headphones `%3` (mute `%4`), front speaker mute `%5`")
                 .arg(audioOutputs["lineout_gain"].toInt())
                 .arg(yesNo(audioOutputs["lineout_mute"].toBool()))
                 .arg(audioOutputs["headphone_gain"].toInt())
                 .arg(yesNo(audioOutputs["headphone_mute"].toBool()))
                 .arg(yesNo(audioOutputs["front_speaker_mute"].toBool()));
    lines << QString("- Remote audio RX: stream `%1`, expected `%2`, pending `%3`, status seen `%4`, owned by us `%5`, compression `%6`")
                 .arg(orPlaceholder(remoteAudioRx["stream_id"].toString()))
                 .arg(yesNo(remoteAudioRx["stream_expected"].toBool()))
                 .arg(yesNo(remoteAudioRx["create_pending"].toBool()))
                 .arg(yesNo(remoteAudioRx["status_seen"].toBool()))
                 .arg(yesNo(remoteAudioRx["owned_by_us"].toBool()))
                 .arg(orPlaceholder(remoteAudioRx["compression"].toString()));
    lines << QString("- Remote audio route note: %1")
                 .arg(orPlaceholder(remoteAudioRx["routing_note"].toString()));
    lines << QString("- Oscillator: setting `%1`, locked `%2`, ext `%3`, TCXO `%4`")
                 .arg(orPlaceholder(oscillator["setting"].toString()))
                 .arg(yesNo(oscillator["locked"].toBool()))
                 .arg(yesNo(oscillator["ext_present"].toBool()))
                 .arg(yesNo(oscillator["tcxo_present"].toBool()));
    lines << QString("- RX globals: binaural `%1`, mute local when remote `%2`, low-latency digital `%3`, "
                      "enforce private IP `%4`, remote on `%5`, multiFLEX `%6`, full duplex `%7`")
                 .arg(yesNo(radio["binaural_rx"].toBool()))
                 .arg(yesNo(radio["mute_local_audio_when_remote"].toBool()))
                 .arg(yesNo(radio["low_latency_digital_modes"].toBool()))
                 .arg(yesNo(radio["enforce_private_ip_connections"].toBool()))
                 .arg(yesNo(radio["remote_on_enabled"].toBool()))
                 .arg(yesNo(radio["mf_enable"].toBool()))
                 .arg(yesNo(radio["full_duplex_enabled"].toBool()));
    lines << QString("- Filter sharpness: voice `%1` (auto `%2`), CW `%3` (auto `%4`), digital `%5` (auto `%6`)")
                 .arg(filterSharpness["voice_level"].toInt())
                 .arg(yesNo(filterSharpness["voice_auto"].toBool()))
                 .arg(filterSharpness["cw_level"].toInt())
                 .arg(yesNo(filterSharpness["cw_auto"].toBool()))
                 .arg(filterSharpness["digital_level"].toInt())
                 .arg(yesNo(filterSharpness["digital_auto"].toBool()));
    lines << QString("- Telemetry: PA `%1 C`, supply `%2 V`, TX forward `%3 W`, TX SWR `%4`, "
                      "ALC `%5`, mic `%6` dBFS, comp `%7` dB")
                 .arg(formatNumber(telemetry["pa_temp_c"]))
                 .arg(formatNumber(telemetry["supply_volts"]))
                 .arg(formatNumber(telemetry["tx_forward_power_w"]))
                 .arg(formatNumber(telemetry["tx_swr"]))
                 .arg(formatNumber(telemetry["alc"]))
                 .arg(formatNumber(telemetry["mic_level_dbfs"]))
                 .arg(formatNumber(telemetry["comp_level_db"]));
    lines << QString("- Amplifier: present `%1`, model `%2`, handle `%3`, operate `%4`")
                 .arg(yesNo(amplifier["present"].toBool()))
                 .arg(orPlaceholder(amplifier["model"].toString()))
                 .arg(orPlaceholder(amplifier["handle"].toString()))
                 .arg(yesNo(amplifier["operate"].toBool()));
    lines << "";

    lines << "## Client-Side DSP";
    if (!clientDsp["available"].toBool()) {
        lines << "- Live audio engine state is unavailable in this snapshot.";
    } else {
        lines << QString("- NR2: enabled `%1`, method `%2` (`%3`), NPE `%4` (`%5`), AE filter `%6`, "
                          "reduction depth `%7`, smoothing `%8`, voice threshold `%9`")
                     .arg(yesNo(nr2["enabled"].toBool()))
                     .arg(nr2["gain_method"].toString())
                     .arg(nr2["gain_method_id"].toInt())
                     .arg(nr2["npe_method"].toString())
                     .arg(nr2["npe_method_id"].toInt())
                     .arg(yesNo(nr2["ae_filter"].toBool()))
                     .arg(formatNumber(nr2["gain_max"], 2))
                     .arg(formatNumber(nr2["gain_smooth"], 2))
                     .arg(formatNumber(nr2["qspp"], 2));
        lines << QString("- NR4: enabled `%1`, method `%2` (`%3`), adaptive noise `%4`, reduction `%5 dB`, "
                          "smoothing `%6%`, whitening `%7%`, masking depth `%8`, suppression `%9`")
                     .arg(yesNo(nr4["enabled"].toBool()))
                     .arg(nr4["noise_method"].toString())
                     .arg(nr4["noise_method_id"].toInt())
                     .arg(yesNo(nr4["adaptive_noise"].toBool()))
                     .arg(formatNumber(nr4["reduction_db"], 1))
                     .arg(formatNumber(nr4["smoothing_pct"], 0))
                     .arg(formatNumber(nr4["whitening_pct"], 0))
                     .arg(formatNumber(nr4["masking_depth"], 2))
                     .arg(formatNumber(nr4["suppression_strength"], 2));
        lines << QString("- RN2: enabled `%1`, parameters `%2`")
                     .arg(yesNo(rn2["enabled"].toBool()))
                     .arg(orPlaceholder(rn2["parameters"].toString()));
        lines << QString("- DFNR: enabled `%1`, attenuation limit `%2 dB`, post-filter beta `%3`")
                     .arg(yesNo(dfnr["enabled"].toBool()))
                     .arg(formatNumber(dfnr["atten_limit_db"], 0))
                     .arg(formatNumber(dfnr["post_filter_beta"], 2));
    }
    lines << "";

    lines << "## Audio Devices";
    if (!audioDevices["available"].toBool()) {
        lines << "- Live audio engine state is unavailable; Qt Multimedia device enumeration is still included below.";
    }
    lines << QString("- RX route: `%1` (source `%2`), PC audio enabled `%3`, PC mute `%4`, master `%5`, engine volume `%6`, engine mute `%7`, RX stream `%8`")
                 .arg(orPlaceholder(rxRoute["output"].toString(), "Unknown"))
                 .arg(orPlaceholder(rxRoute["source"].toString()))
                 .arg(yesNo(audioVolumes["pc_audio_enabled"].toBool()))
                 .arg(yesNo(audioVolumes["pc_audio_muted"].toBool()))
                 .arg(formatPercentValue(audioVolumes["pc_master_volume_pct"]))
                 .arg(formatPercentValue(audioVolumes["engine_rx_volume_pct"]))
                 .arg(formatBoolValue(audioVolumes["engine_rx_muted"]))
                 .arg(formatBoolValue(audioDevices["rx_streaming"]));
    lines << QString("- Radio stream route: remote_audio_rx `%1`, expected `%2`, create pending `%3`, remove requested `%4`, status seen `%5`, owned by us `%6`")
                 .arg(orPlaceholder(rxRoute["remote_audio_rx_stream_id"].toString()))
                 .arg(formatBoolValue(rxRoute["remote_audio_rx_expected"]))
                 .arg(formatBoolValue(rxRoute["remote_audio_rx_create_pending"]))
                 .arg(formatBoolValue(rxRoute["remote_audio_rx_remove_requested"]))
                 .arg(formatBoolValue(rxRoute["remote_audio_rx_status_seen"]))
                 .arg(formatBoolValue(rxRoute["remote_audio_rx_owned_by_us"]));
    lines << QString("- TX input route: `%1` (mic `%2`, DAX `%3`), PC mic gain `%4`, TX stream `%5`, DAX TX mode `%6`, DAX radio route `%7`")
                 .arg(orPlaceholder(txRoute["input"].toString(), "Unknown"))
                 .arg(orPlaceholder(txRoute["mic_selection"].toString()))
                 .arg(yesNo(txRoute["dax_on"].toBool()))
                 .arg(formatPercentValue(audioVolumes["pc_mic_gain_pct"]))
                 .arg(formatBoolValue(audioDevices["tx_streaming"]))
                 .arg(formatBoolValue(audioDevices["dax_tx_mode"]))
                 .arg(formatBoolValue(audioDevices["dax_tx_use_radio_route"]));
    // TX slice context — colocated with the global mic/DAX state so DAX-channel
    // mismatches with WSJT-X / JTDX are visible without cross-referencing the
    // per-slice section. (#2885)
    const QString txSliceIdText = txRoute["tx_slice_id"].isDouble()
        ? QString::number(txRoute["tx_slice_id"].toInt())
        : QStringLiteral("n/a");
    const QString txSliceDaxText = txRoute["tx_slice_dax_channel"].isDouble()
        ? QString::number(txRoute["tx_slice_dax_channel"].toInt())
        : QStringLiteral("n/a");
    lines << QString("- TX slice context: slice `%1`, mode `%2`, slice DAX channel `%3`")
                 .arg(txSliceIdText)
                 .arg(orPlaceholder(txRoute["tx_slice_mode"].toString()))
                 .arg(txSliceDaxText);
    lines << QString("- Selected input: %1 (source `%2`, present `%3`)")
                 .arg(formatAudioDeviceShort(audioDevices["selected_input"].toObject()))
                 .arg(orPlaceholder(audioDevices["selected_input_source"].toString()))
                 .arg(yesNo(audioDevices["selected_input_present"].toBool()));
    lines << QString("- Selected output: %1 (source `%2`, present `%3`)")
                 .arg(formatAudioDeviceShort(audioDevices["selected_output"].toObject()))
                 .arg(orPlaceholder(audioDevices["selected_output_source"].toString()))
                 .arg(yesNo(audioDevices["selected_output_present"].toBool()));
    lines << QString("- Radio output levels: lineout `%1` (mute `%2`), headphones `%3` (mute `%4`), front speaker mute `%5`")
                 .arg(audioVolumes["radio_lineout_gain"].toInt())
                 .arg(yesNo(audioVolumes["radio_lineout_mute"].toBool()))
                 .arg(audioVolumes["radio_headphone_gain"].toInt())
                 .arg(yesNo(audioVolumes["radio_headphone_mute"].toBool()))
                 .arg(yesNo(audioVolumes["radio_front_speaker_mute"].toBool()));

    lines << "- Input device list:";
    const QJsonArray inputDevices = audioDevices["input_devices"].toArray();
    if (inputDevices.isEmpty()) {
        lines << "  - None reported by Qt Multimedia.";
    } else {
        for (const QJsonValue& value : inputDevices)
            lines << formatAudioDeviceBullet(value.toObject(), "  - ");
    }

    lines << "- Output device list:";
    const QJsonArray outputDevices = audioDevices["output_devices"].toArray();
    if (outputDevices.isEmpty()) {
        lines << "  - None reported by Qt Multimedia.";
    } else {
        for (const QJsonValue& value : outputDevices)
            lines << formatAudioDeviceBullet(value.toObject(), "  - ");
    }
    lines << "";

    lines << "## Control Devices";
    if (!controlDevices["available"].toBool()) {
        lines << QString("- %1").arg(orPlaceholder(controlDevices["note"].toString(), "Control device state is unavailable."));
    }
    lines << QString("- Target scope: `%1`, target slice `%2`, active slice available `%3`, active device count `%4`")
                 .arg(orPlaceholder(controlDevices["target_scope"].toString(), "active_slice"))
                 .arg(controlDevices.contains("target_slice_id") && controlDevices["target_slice_id"].isDouble()
                          ? QString::number(controlDevices["target_slice_id"].toInt())
                          : QStringLiteral("n/a"))
                 .arg(yesNo(controlDevices["active_slice_available"].toBool()))
                 .arg(controlDevices["active_device_count"].toInt());
    const QJsonArray controlDeviceList = controlDevices["devices"].toArray();
    if (controlDeviceList.isEmpty()) {
        lines << "- No control device entries are available.";
    } else {
        for (const QJsonValue& value : controlDeviceList) {
            const QJsonObject device = value.toObject();
            lines << formatControlDeviceBullet(device);
            const QJsonArray bindings = device["bindings"].toArray();
            if (!bindings.isEmpty()) {
                lines << "  - MIDI bindings:";
                for (const QJsonValue& binding : bindings)
                    lines << formatMidiBindingBullet(binding.toObject(), "    - ");
            }
        }
    }
    lines << "";

    lines << "## Clients";
    if (ownership["clients"].toArray().isEmpty()) {
        lines << "- No client metadata is currently tracked.";
    } else {
        for (const QJsonValue& value : ownership["clients"].toArray())
            lines << formatClientBullet(value.toObject());
    }
    lines << "";

    lines << "## Panadapters";
    if (snapshot["panadapters"].toArray().isEmpty()) {
        lines << "- None";
    } else {
        for (const QJsonValue& value : snapshot["panadapters"].toArray())
            lines << formatPanBullet(value.toObject());
    }
    lines << "";

    lines << "## XVTRs";
    if (snapshot["xvtrs"].toArray().isEmpty()) {
        lines << "- None";
    } else {
        for (const QJsonValue& value : snapshot["xvtrs"].toArray())
            lines << formatXvtrBullet(value.toObject());
    }
    lines << "";

    lines << "## Transmit State";
    lines << QString("- Power: RF `%1`, tune `%2`, max `%3`, tune mode `%4`, show TX in waterfall `%5`")
                 .arg(txPower["rf_power"].toInt())
                 .arg(txPower["tune_power"].toInt())
                 .arg(txPower["max_power_level"].toInt())
                 .arg(orPlaceholder(txPower["tune_mode"].toString()))
                 .arg(yesNo(txPower["show_tx_in_waterfall"].toBool()));
    lines << QString("- TX state: tuning `%1`, MOX `%2`, transmitting `%3`, ATU `%4` "
                      "(enabled `%5`, memories `%6`, using memory `%7`), APD `%8` "
                      "(configurable `%9`, EQ active `%10`)")
                 .arg(yesNo(txPower["tuning"].toBool()))
                 .arg(yesNo(txPower["mox"].toBool()))
                 .arg(yesNo(txPower["transmitting"].toBool()))
                 .arg(orPlaceholder(atu["status"].toString()))
                 .arg(yesNo(atu["enabled"].toBool()))
                 .arg(yesNo(atu["memories_enabled"].toBool()))
                 .arg(yesNo(atu["using_memory"].toBool()))
                 .arg(yesNo(apd["enabled"].toBool()))
                 .arg(yesNo(apd["configurable"].toBool()))
                 .arg(yesNo(apd["equalizer_active"].toBool()));
    lines << QString("- Mic/phone: input `%1`, mic level `%2`, mic boost `%3`, mic bias `%4`, DAX `%5`, "
                      "monitor `%6` (`%7`), speech processor `%8` (`%9`), compander `%10` (`%11`), "
                      "dexp `%12` (`%13`), met-in-RX `%14`, sync CWX `%15`, AM carrier `%16`, "
                      "TX filter `%17..%18 Hz`")
                 .arg(orPlaceholder(mic["selection"].toString()))
                 .arg(mic["level"].toInt())
                 .arg(yesNo(mic["mic_boost"].toBool()))
                 .arg(yesNo(mic["mic_bias"].toBool()))
                 .arg(yesNo(mic["dax_on"].toBool()))
                 .arg(yesNo(mic["sb_monitor"].toBool()))
                 .arg(mic["mon_gain_sb"].toInt())
                 .arg(yesNo(mic["speech_processor_enable"].toBool()))
                 .arg(mic["speech_processor_level"].toInt())
                 .arg(yesNo(mic["compander_on"].toBool()))
                 .arg(mic["compander_level"].toInt())
                 .arg(yesNo(mic["dexp_on"].toBool()))
                 .arg(mic["dexp_level"].toInt())
                 .arg(yesNo(mic["met_in_rx"].toBool()))
                 .arg(yesNo(mic["sync_cwx"].toBool()))
                 .arg(mic["am_carrier_level"].toInt())
                 .arg(mic["tx_filter_low"].toInt())
                 .arg(mic["tx_filter_high"].toInt());
    lines << QString("- VOX: enabled `%1`, level `%2`, delay `%3`")
                 .arg(yesNo(vox["enabled"].toBool()))
                 .arg(vox["level"].toInt())
                 .arg(vox["delay"].toInt());
    lines << QString("- CW: speed `%1` WPM, pitch `%2 Hz`, break-in `%3`, delay `%4 ms`, sidetone `%5`, "
                      "iambic `%6`, mode `%7`, swap paddles `%8`, CWL `%9`, monitor `%10`")
                 .arg(cw["speed_wpm"].toInt())
                 .arg(cw["pitch_hz"].toInt())
                 .arg(yesNo(cw["break_in"].toBool()))
                 .arg(cw["delay_ms"].toInt())
                 .arg(yesNo(cw["sidetone"].toBool()))
                 .arg(yesNo(cw["iambic"].toBool()))
                 .arg(cw["iambic_mode"].toInt())
                 .arg(yesNo(cw["swap_paddles"].toBool()))
                 .arg(yesNo(cw["cwl_enabled"].toBool()))
                 .arg(cw["monitor_gain"].toInt());
    lines << QString("- Interlock: ACC TX delay `%1`, TX1 `%2`, TX2 `%3`, TX3 `%4`, TX delay `%5`, timeout `%6`, "
                      "ACC polarity `%7`, RCA polarity `%8`")
                 .arg(interlock["acc_tx_delay"].toInt())
                 .arg(interlock["tx1_delay"].toInt())
                 .arg(interlock["tx2_delay"].toInt())
                 .arg(interlock["tx3_delay"].toInt())
                 .arg(interlock["tx_delay"].toInt())
                 .arg(interlock["timeout"].toInt())
                 .arg(interlock["acc_tx_req_polarity"].toInt())
                 .arg(interlock["rca_tx_req_polarity"].toInt());
    lines << "";

    lines << "## TX Band Settings";
    if (snapshot["tx_band_settings"].toArray().isEmpty()) {
        lines << "- None";
    } else {
        for (const QJsonValue& value : snapshot["tx_band_settings"].toArray())
            lines << formatTxBandBullet(value.toObject());
    }
    lines << "";

    lines << "## Global Meter Cache";
    if (snapshot["global_meters"].toArray().isEmpty()) {
        lines << "- None";
    } else {
        for (const QJsonValue& value : snapshot["global_meters"].toArray())
            lines << formatMeterBullet(value.toObject());
    }
    lines << "";

    lines << "## Slices";
    const QJsonArray slices = snapshot["slices"].toArray();
    if (slices.isEmpty()) {
        lines << "- No app-owned slices are currently tracked.";
    } else {
        for (const QJsonValue& value : slices) {
            const QJsonObject slice = value.toObject();
            const QJsonObject filter = slice["filter"].toObject();
            const QJsonObject audio = slice["audio"].toObject();
            const QJsonObject antennas = slice["antennas"].toObject();
            const QJsonObject control = slice["control"].toObject();
            const QJsonObject dsp = slice["dsp"].toObject();
            const QJsonObject nb = dsp["nb"].toObject();
            const QJsonObject nr = dsp["nr"].toObject();
            const QJsonObject anf = dsp["anf"].toObject();
            const QJsonObject lmsNr = dsp["lms_nr"].toObject();
            const QJsonObject speexNr = dsp["speex_nr"].toObject();
            const QJsonObject nrf = dsp["nrf"].toObject();
            const QJsonObject lmsAnf = dsp["lms_anf"].toObject();
            const QJsonObject apfObj = dsp["apf"].toObject();
            const QJsonObject diversity = slice["diversity"].toObject();
            const QJsonObject tuning = slice["tuning"].toObject();
            const QJsonObject digital = slice["digital"].toObject();
            const QJsonObject fm = slice["fm"].toObject();
            const QJsonObject pan = slice["panadapter_state"].toObject();
            const QString sliceRxRoute = orPlaceholder(audio["rx_route"].toString(), "Unknown");
            const QString sliceRxTarget = sliceRxRoute == QStringLiteral("PC audio")
                ? orPlaceholder(audio["selected_output_device"].toString(), "Unavailable")
                : QStringLiteral("radio lineout/headphones");

            lines << QString("### Slice %1").arg(slice["slice_id"].toInt());
            lines << QString("- Pan `%1`, active `%2`, TX slice `%3`, frequency `%4 MHz`, mode `%5`")
                         .arg(orPlaceholder(slice["pan_id"].toString()))
                         .arg(yesNo(slice["active"].toBool()))
                         .arg(yesNo(slice["tx_slice"].toBool()))
                         .arg(formatNumber(slice["frequency_mhz"], 6))
                         .arg(orPlaceholder(slice["mode"].toString()));
            lines << QString("- Filter: `%1..%2 Hz`, step `%3 Hz`, modes `%4`")
                         .arg(filter["low_hz"].toInt())
                         .arg(filter["high_hz"].toInt())
                         .arg(tuning["step_hz"].toInt())
                         .arg(joinArray(slice["mode_list"].toArray()));
            lines << QString("- Audio / RF: audio gain `%1`, audio pan `%2`, audio mute `%3`, RF gain `%4`, route `%5` via `%6`")
                         .arg(formatNumber(audio["gain"]))
                         .arg(audio["pan"].toInt())
                         .arg(yesNo(audio["mute"].toBool()))
                         .arg(formatNumber(slice["rf_gain"], 1))
                         .arg(sliceRxRoute)
                         .arg(sliceRxTarget);
            lines << QString("- Antennas / control: RX `%1`, TX `%2`, locked `%3`, QSK `%4`, "
                              "record `%5`, play `%6` (enabled `%7`)")
                         .arg(orPlaceholder(antennas["rx"].toString()))
                         .arg(orPlaceholder(antennas["tx"].toString()))
                         .arg(yesNo(control["locked"].toBool()))
                         .arg(yesNo(control["qsk"].toBool()))
                         .arg(yesNo(control["record_on"].toBool()))
                         .arg(yesNo(control["play_on"].toBool()))
                         .arg(yesNo(control["play_enabled"].toBool()));
            lines << QString("- AGC / squelch: mode `%1`, threshold `%2`, squelch `%3` (`%4`)")
                         .arg(orPlaceholder(dsp["agc_mode"].toString()))
                         .arg(dsp["agc_threshold"].toInt())
                         .arg(yesNo(tuning["squelch_on"].toBool()))
                         .arg(tuning["squelch_level"].toInt());
            lines << QString("- RIT / XIT: RIT `%1` (`%2 Hz`), XIT `%3` (`%4 Hz`), DAX channel `%5`")
                         .arg(yesNo(tuning["rit_on"].toBool()))
                         .arg(tuning["rit_hz"].toInt())
                         .arg(yesNo(tuning["xit_on"].toBool()))
                         .arg(tuning["xit_hz"].toInt())
                         .arg(digital["dax_channel"].toInt());
            lines << QString("- Diversity / ESC: diversity `%1`, parent `%2`, child `%3`, index `%4`, "
                              "ESC `%5`, gain `%6`, phase `%7`")
                         .arg(yesNo(diversity["enabled"].toBool()))
                         .arg(yesNo(diversity["is_parent"].toBool()))
                         .arg(yesNo(diversity["is_child"].toBool()))
                         .arg(diversity["index"].toInt())
                         .arg(yesNo(diversity["esc_enabled"].toBool()))
                         .arg(formatNumber(diversity["esc_gain"], 2))
                         .arg(formatNumber(diversity["esc_phase_shift_deg"], 2));
            lines << QString("- DSP: NB `%1` (`%2`), NR `%3` (`%4`), ANF `%5` (`%6`), "
                              "LMS NR `%7` (`%8`), Speex NR `%9` (`%10`), RNNoise `%11`, "
                              "NRF `%12` (`%13`), LMS ANF `%14` (`%15`), ANFT `%16`, APF `%17` (`%18`)")
                         .arg(yesNo(nb["enabled"].toBool()))
                         .arg(nb["level"].toInt())
                         .arg(yesNo(nr["enabled"].toBool()))
                         .arg(nr["level"].toInt())
                         .arg(yesNo(anf["enabled"].toBool()))
                         .arg(anf["level"].toInt())
                         .arg(yesNo(lmsNr["enabled"].toBool()))
                         .arg(lmsNr["level"].toInt())
                         .arg(yesNo(speexNr["enabled"].toBool()))
                         .arg(speexNr["level"].toInt())
                         .arg(yesNo(dsp["rnnoise"].toBool()))
                         .arg(yesNo(nrf["enabled"].toBool()))
                         .arg(nrf["level"].toInt())
                         .arg(yesNo(lmsAnf["enabled"].toBool()))
                         .arg(lmsAnf["level"].toInt())
                         .arg(yesNo(dsp["anft"].toBool()))
                         .arg(yesNo(apfObj["enabled"].toBool()))
                         .arg(apfObj["level"].toInt());
            lines << QString("- Digital / RTTY: mark `%1`, shift `%2`, DIGL `%3`, DIGU `%4`, step list `%5`")
                         .arg(digital["rtty_mark_hz"].toInt())
                         .arg(digital["rtty_shift_hz"].toInt())
                         .arg(digital["digl_offset_hz"].toInt())
                         .arg(digital["digu_offset_hz"].toInt())
                         .arg(joinArray(tuning["step_list"].toArray()));
            lines << QString("- FM: tone mode `%1`, tone `%2`, repeater dir `%3`, repeater offset `%4 MHz`, "
                              "TX offset `%5 MHz`, deviation `%6 Hz`")
                         .arg(orPlaceholder(fm["tone_mode"].toString()))
                         .arg(orPlaceholder(fm["tone_value"].toString()))
                         .arg(orPlaceholder(fm["repeater_offset_dir"].toString()))
                         .arg(formatNumber(fm["repeater_offset_mhz"], 6))
                         .arg(formatNumber(fm["tx_offset_mhz"], 6))
                         .arg(fm["deviation_hz"].toInt());

            if (!pan.isEmpty()) {
                lines << QString("- Pan state: center `%1 MHz`, bandwidth `%2 MHz`, RF gain `%3`, preamp `%4`, "
                                  "WNB `%5` (`%6`), waterfall `%7`")
                             .arg(formatNumber(pan["center_mhz"], 6))
                             .arg(formatNumber(pan["bandwidth_mhz"], 6))
                             .arg(formatNumber(pan["rf_gain"], 0))
                             .arg(orPlaceholder(pan["preamp"].toString()))
                             .arg(yesNo(pan["wnb_active"].toBool()))
                             .arg(formatNumber(pan["wnb_level"], 0))
                             .arg(orPlaceholder(pan["waterfall_id"].toString()));
            }

            lines << "- Meters:";
            const QJsonArray meters = slice["meters"].toArray();
            if (meters.isEmpty()) {
                lines << "  - None";
            } else {
                for (const QJsonValue& meterValue : meters)
                    lines << formatMeterBullet(meterValue.toObject(), "  - ");
            }
            lines << "";
        }
    }

    return lines.join('\n').trimmed() + '\n';
}

} // namespace AetherSDR
