#include "KenwoodBackend.h"
#include "KenwoodFilter.h"
#include "KenwoodPowerLimits.h"

#include "core/Resampler.h"
#include "models/NetworkRadioLogin.h"

#include <QLoggingCategory>
#include <QHostAddress>
#include <QChar>
#include <QMetaType>
#include <QNetworkDatagram>
#include <QPointer>
#include <QScopeGuard>
#include <QStringList>
#include <QVersionNumber>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>
#include <QtEnvironmentVariables>

namespace AetherSDR::kenwood {
namespace {
Q_LOGGING_CATEGORY(lcKenwood, "aether.kenwood")
constexpr int kMemoryVfo = -2;
MeterDef receiveLevelDefinition()
{
    MeterDef level;
    level.index = 103;
    level.source = QStringLiteral("SLC");
    level.sourceIndex = 0;
    level.name = QStringLiteral("LEVEL");
    level.unit = QStringLiteral("dot");
    level.low = 0;
    level.high = 70;
    level.description = QStringLiteral("TS-890S receive S-meter display deflection; not calibrated dBm.");
    return level;
}

// Instruction Manual EN-7-3 lists these eight BS4 spans, corroborated in
// firmware 1.01 USB: docs/architecture/kenwood-ts890-rx-parity/hardware-span-2026-11-03.json.
// The January 2019 PC reference's seven-entry BS4 table differs.
constexpr std::array<int, 8> kSpans{5000, 10000, 20000, 30000, 50000, 100000, 200000, 500000};
const QStringList kModes{"LSB", "USB", "CW", "FM", "AM", "RTTY", "CWR", "RTTYR",
                          "PSK", "PSKR", "DIGL", "DIGU", "DFM", "DAM"};
const QStringList kNonFmModes{"LSB", "USB", "CW", "AM", "RTTY", "CWR", "RTTYR",
    "PSK", "PSKR", "DIGL", "DIGU", "DAM"};
const QStringList kVoiceModes{"LSB", "USB", "FM", "AM", "DIGL", "DIGU", "DFM", "DAM"};
constexpr auto kPan = "0";
std::optional<int> boundedInteger(const QVariant& value, int minimum, int maximum)
{
    const int type = value.typeId();
    const bool numeric = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
        || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
    bool converted = false;
    const double number = value.toDouble(&converted);
    if (!numeric || !converted || !std::isfinite(number) || std::trunc(number) != number
        || number < minimum || number > maximum) { return std::nullopt; }
    return static_cast<int>(number);
}
std::optional<int> decimal(const QByteArray& bytes, int minimum, int maximum)
{
    if (bytes.isEmpty() || bytes.size() > 9) { return std::nullopt; }
    int result = 0;
    for (char value : bytes) {
        if (value < '0' || value > '9') { return std::nullopt; }
        result = result * 10 + value - '0';
    }
    if (result < minimum || result > maximum) { return std::nullopt; }
    return result;
}
}

KenwoodBackend::KenwoodBackend(QObject* parent) : KenwoodBackend(Transport{}, parent) {}

KenwoodBackend::KenwoodBackend(Transport transport, QObject* parent)
    : IRadioBackend(parent), m_transport(std::move(transport))
{
    m_timer.setParent(this);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        consume(m_protocol.tick(m_clock.elapsed()));
        if (!guard || generation != m_generation) { return; }
        if (isConnected() && (!m_identityConfirmed || !m_transverterKnown)
            && m_clock.elapsed() >= m_identityDeadlineMs) {
            fail(QStringLiteral("TS-890S identification or transverter-Off qualification timed out."));
            return;
        }
        if (isConnected() && m_identityConfirmed && m_clock.elapsed() >= m_nextMeterPollMs) {
            m_nextMeterPollMs = m_clock.elapsed() + 200;
            send("SM");
        }
        if (!guard || generation != m_generation) { return; }
        pollRelativeTuning(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollScopeFrameFreshness(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollFmWidth(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollPskReceive(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollReceiverOptions(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollScanPreferences(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollVfoScan(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollTransverter(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollCwReceive(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollOperatingDisplay(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollAutoMode(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollMemoryInventory(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollRadioMessageWrite(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollNetworkAudioLevels(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollPowerLimits(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollVfos(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollCwPreferences(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollTransmitInputs(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        pollTunePreflight(m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
        if (m_keyIntent && !m_txOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
            setKeying(false, m_txOperation);
        }
        if (!guard || generation != m_generation) { return; }
        if (m_atuIntent && !m_atuOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
            setAtu(false, m_atuOperation);
        }
        if (!guard || generation != m_generation) { return; }
        if (m_cwIntent && !m_cwOperation.permitsDispatch(TxCoordinator::monotonicMs())) {
            abortCwText(m_cwOperation);
        }
    });
    m_txTimer.setParent(this);
    m_txTimer.setTimerType(Qt::PreciseTimer);
    m_txTimer.setInterval(20);
    connect(&m_txTimer, &QTimer::timeout, this, &KenwoodBackend::pumpTxAudio);
}

KenwoodBackend::~KenwoodBackend() { m_destructing = true; stop(); }

RadioCapabilities KenwoodBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("kenwood");
    caps.manufacturer = QStringLiteral("Kenwood");
    caps.model = QStringLiteral("TS-890S");
    using Interaction = ReceiveInteractionControl::Feature;
    caps.receiveInteractionControl = ReceiveInteractionControl{{Interaction::PendingTune,
        Interaction::AntennaReadback, Interaction::ContextRetirement}};
    using Client = ReceiveClientControl::Feature;
    caps.receiveClientControl = ReceiveClientControl{{Client::NativeModes, Client::ConfirmedNotifications,
        Client::FrequencyLimits, Client::QualifiedMeters, Client::QueuedIntentFencing}};
    caps.scopeModePresentation = ScopeModePresentation{QStringLiteral(
        "Available modes, dragging behavior and stored-range updates vary by radio.\n"
        "See the radio-specific user guide for details.")};
    caps.maxSlices = 1;
    caps.maxPanadapters = 1;
    caps.panDisplayControl = PanDisplayControl{{PanDisplayField::Geometry, PanDisplayField::RfGain},
        {PanGeometryComponent::Center, PanGeometryComponent::Bandwidth}};
    caps.canTransmit = isConnected() && m_identityConfirmed && m_txPermitted && m_loginEnabled
        && (m_txUdp || m_transport.audio);
    const std::optional<int> carrierBand = tunePowerBand();
    const bool carrierAvailable = caps.canTransmit && carrierBand && m_powerLimiterEnabled
        && (m_tune || (!m_cwIntent && !m_atuIntent && m_powerLimits.contains(60 + *carrierBand)
            && m_txStateKnown && !m_txObserved));
    caps.hasCwTune = carrierAvailable;
    caps.tuneCarrierControl = RadioCapabilities::TuneCarrierControl{carrierAvailable ? kModes : QStringList{},
        QStringLiteral("TS-890S TUNE requires confirmed RX, transmit permission, a physical TX VFO, normal output, transverter Off, known XIT, native tuning-power/limiter readback and no active CW or ATU input."),
        RadioCapabilities::TuneCarrierControl::IntentLifetime::Backend};
    caps.transmitIntentControl = RadioCapabilities::TransmitIntentControl{
        RadioCapabilities::TransmitIntentControl::Lifetime::Backend,
        RadioCapabilities::TransmitIntentControl::Lifetime::Backend};
    caps.hasFmRepeaterOffset = false;
    caps.hasTuner = caps.canTransmit;
    caps.txPowerMaxWatts = 100;
    if (maximumNormalPower()) {
        caps.transmitDriveControl = RadioCapabilities::TransmitDriveControl{SliceFrequencyControl::Authority::Radio, false};
    }
    caps.takesTxAudioOverSeam = true;
    caps.radioPttObservation = RadioCapabilities::RadioPttObservation{};
    caps.hasRadioPttReadback = true;
    caps.pcAudioRoutingControl = RadioCapabilities::PcAudioRoutingControl{};
    caps.hasRadioSideCwKeyer = caps.canTransmit;
    caps.cwSpeedMinWpm = caps.cwTextMinWpm = 5;
    caps.cwSpeedMaxWpm = caps.cwTextMaxWpm = 60;
    caps.cwPitchMinHz = 300;
    caps.cwPitchMaxHz = 1100;
    caps.cwPitchStepHz = 5;
    caps.cwTextMaxMessageChars = 24;
    caps.cwTextAllowedCharacters = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 '\"()*+,-./:=?@[]_<>\\#%");
    caps.cwTextHasProgress = false;
    caps.cwTextHasStoredMacros = false;
    caps.cwTextSupportsLive = false;
    caps.cwTextSupportsSpeedModifiers = false;
    caps.radioOwnsDbmScale = false;
    caps.sampleRatesHz = {24000};
    caps.tuningMinHz = kMinimumFrequencyHz;
    caps.tuningMaxHz = m_maximumFrequencyHz;
    caps.sliceFrequencyControl = {ordinaryReceiveCoordinatesAvailable()
        ? SliceFrequencyControl::Authority::Radio : SliceFrequencyControl::Authority::Unknown,
        kMinimumFrequencyHz, m_maximumFrequencyHz};
    caps.receiveModeControl = ReceiveModeControl{SliceFrequencyControl::Authority::Radio,
        receiveModeWritable() ? kModes : QStringList{}};
    ReceiveFilterControl filters{SliceFrequencyControl::Authority::Radio,
        {{QStringLiteral("CW"), -2050, 775, -775, 2050, 50, 2500},
         {QStringLiteral("CWR"), -2050, 775, -775, 2050, 50, 2500}},
        ReceiveFilterControl::ModePolicy::ListedOnly};
    const auto appendSsbFilter = [&filters](const QString& mode, int style, bool lower) {
        if (style != 0 && style != 1) { return; }
        ReceiveFilterMode range = style == 0
            ? ReceiveFilterMode{mode, 0, 2000, 600, 5000, 100, 5000}
            : ReceiveFilterMode{mode, -1450, 2475, 75, 4000, 50, 3000};
        if (lower) {
            const int lowMinimum = range.minimumLowHz;
            const int lowMaximum = range.maximumLowHz;
            range.minimumLowHz = -range.maximumHighHz;
            range.maximumLowHz = -range.minimumHighHz;
            range.minimumHighHz = -lowMaximum;
            range.maximumHighHz = -lowMinimum;
        }
        filters.modes.append(range);
    };
    appendSsbFilter(QStringLiteral("USB"), m_ssbFilterStyle, false);
    appendSsbFilter(QStringLiteral("LSB"), m_ssbFilterStyle, true);
    appendSsbFilter(QStringLiteral("DIGU"), m_dataFilterStyle, false);
    appendSsbFilter(QStringLiteral("DIGL"), m_dataFilterStyle, true);
    if (m_filterBankAxesPending || m_receiveDspMonitor != std::optional<bool>(false)) { filters.modes.clear(); }
    caps.receiveFilterControl = filters;
    caps.receiveAudioExport = ReceiveAudioExport{{24000}, 1};
    caps.hasRadioSideDsp = true;
    using DspFeature = ReceiveDspControl::Feature;
    ReceiveDspControl dsp{kModes,
        {DspFeature::Agc, DspFeature::Nb, DspFeature::Nr, DspFeature::Anf, DspFeature::Rit},
        {{DspFeature::Agc, kNonFmModes}, {DspFeature::Nb, kNonFmModes}, {DspFeature::Anf, kVoiceModes}}};
    if (!isConnected() || !m_identityConfirmed || m_receiveVfo < 0 || !kModes.contains(m_mode)) { dsp.modes.clear(); }
    if (m_noiseReduction != 0 && m_noiseReduction != 1) { dsp.unavailableLevels.append(DspFeature::Nr); }
    const bool apf = receiveControlsWritable() && m_receiveDspMonitor == std::optional<bool>(false)
        && !m_receiveControlsPending.contains(QStringLiteral("monitor.dsp"))
        && m_receiveControlValues.contains(QStringLiteral("apf.enabled"))
        && !m_receiveControlsPending.contains(QStringLiteral("apf.enabled"))
        && (m_mode == QLatin1String("CW") || m_mode == QLatin1String("CWR")
            || ((m_mode == QLatin1String("RTTY") || m_mode == QLatin1String("RTTYR"))
                && m_receiveFskSpacing == std::optional<int>(0)));
    caps.hasAudioPeakingFilter = apf;
    dsp.unavailableLevels.append(DspFeature::Apf);
    if (apf) {
        dsp.features.append(DspFeature::Apf);
        dsp.featureModes.insert(DspFeature::Apf, {m_mode});
    }
    caps.receiveDspControl = dsp;
    caps.hasModeIndependentSquelch = true;
    caps.speechProcessorControl = RadioCapabilities::SpeechProcessorControl{100, QStringLiteral("PROC IN")};
    caps.txMonitorControl = RadioCapabilities::TxMonitorControl{};
    caps.cwDelayControl = RadioCapabilities::CwDelayControl{1000, 50};
    using TxField = TransmitControlRequest::Field;
    RadioTransmitSettings settings{{TxField::MicGain, TxField::ProcessorEnabled,
        TxField::MonitorEnabled, TxField::MonitorLevel, TxField::CwSpeed,
        TxField::CwPitch, TxField::CwBreakIn, TxField::CwDelay,
        TxField::CwIambicMode, TxField::CwSwap}};
    if (caps.transmitDriveControl) { settings.fields.insert(TxField::RfPower); }
    if (m_processorOutput >= 0) { settings.fields.insert(TxField::ProcessorLevel); }
    if (m_identityConfirmed && m_radioSidetone >= 0) { settings.fields.insert(TxField::CwSidetone); }
    if (m_identityConfirmed && m_cwLowerSideband.has_value()) { settings.fields.insert(TxField::CwlEnabled); }
    const std::optional<int> tuneBand = tunePowerBand();
    if (tuneBand && m_powerLimits.contains(60 + *tuneBand) && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_atuIntent && !m_cwIntent) { settings.fields.insert(TxField::TunePower); }
    caps.hasTxFilterControls = txFilterKnown();
    if (caps.hasTxFilterControls) {
        settings.fields.insert(TxField::Filter);
        caps.txFilterLowEdgesHz = txFilterLowEdges();
        caps.txFilterHighEdgesHz = txFilterHighEdges();
    }
    caps.radioTransmitSettings = settings;
    caps.extensionNamespaces = {QStringLiteral("kenwood")};
    caps.extensions.insert(QStringLiteral("kenwood"),
        QVariantMap{{QStringLiteral("vfoControlVersion"), 1}, {QStringLiteral("nativeFilterControlVersion"), 1},
            {QStringLiteral("networkAudioLevelsVersion"), 1}});
    QVariantMap extension = caps.extensions.value(QStringLiteral("kenwood")).toMap();
    extension.insert(QStringLiteral("nativeSettingsPages"), QStringList{
        "vfo", "filters", "scope", "messages", "memories", "autoMode", "operatingDisplay",
        "cwReceive", "transverter", "vfoScan", "scanPreferences", "pskReceive", "receiverOptions",
        "receiveDsp", "receiveRouting", "receiveAgc", "receiveEq", "networkAudio", "transmitInputs", "powerLimits"});
    extension.insert(QStringLiteral("nativeReceiveControlsVersion"), 1);
    extension.insert(QStringLiteral("nativeRelativeTuningVersion"), 1);
    extension.insert(QStringLiteral("nativeRelativeTuningVerbs"), QStringList{"tuning.relative.get", "tuning.relative.read", "tuning.relative.fc", "tuning.relative.ud", "tuning.relative.cancel"});
    extension.insert(QStringLiteral("nativeTuningControlVersion"), 1);
    extension.insert(QStringLiteral("nativeFilterBankControlVersion"), 1);
    extension.insert(QStringLiteral("nativeFmWidthControlVersion"), 1);
    extension.insert(QStringLiteral("nativeScopeControlsVersion"), 1);
    extension.insert(QStringLiteral("nativePskReceiveVersion"), 1);
    extension.insert(QStringLiteral("nativePskReceiveVerbs"), QStringList{"psk.receive.get", "psk.receive.read", "psk.receive.set", "psk.receive.reset", "psk.receive.cancel"});
    extension.insert(QStringLiteral("nativeReceiverOptionsVersion"), 1);
    extension.insert(QStringLiteral("nativeReceiverOptionsVerbs"), QStringList{"receiver.options.get", "receiver.options.read", "receiver.options.set", "receiver.options.cancel"});
    extension.insert(QStringLiteral("nativeScanPreferencesVersion"), 1);
    extension.insert(QStringLiteral("nativeScanPreferencesVerbs"), QStringList{QStringLiteral("scan.preferences.get"), QStringLiteral("scan.preferences.read"), QStringLiteral("scan.preferences.set"), QStringLiteral("scan.preferences.reset"), QStringLiteral("scan.preferences.cancel")});
    extension.insert(QStringLiteral("nativeVfoScanVersion"), 1);
    extension.insert(QStringLiteral("nativeVfoScanVerbs"), QStringList{QStringLiteral("scan.vfo.get"), QStringLiteral("scan.vfo.read"), QStringLiteral("scan.vfo.set"), QStringLiteral("scan.vfo.cancel")});
    extension.insert(QStringLiteral("nativeTransverterVersion"), 1);
    extension.insert(QStringLiteral("nativeTransverterVerbs"), QStringList{QStringLiteral("transverter.get"), QStringLiteral("transverter.read"), QStringLiteral("transverter.cancel"), QStringLiteral("transverter.enabled.set"), QStringLiteral("transverter.offset.preview"), QStringLiteral("transverter.offset.set")});
    extension.insert(QStringLiteral("nativeCwReceiveVersion"), 1);
    extension.insert(QStringLiteral("nativeCwReceiveVerbs"), QStringList{QStringLiteral("cw.receive.get"), QStringLiteral("cw.receive.read"), QStringLiteral("cw.receive.set"), QStringLiteral("cw.receive.cancel"), QStringLiteral("cw.receive.clear")});
    extension.insert(QStringLiteral("nativeOperatingDisplayVersion"), 1);
    extension.insert(QStringLiteral("nativeOperatingDisplayVerbs"), QStringList{QStringLiteral("operating.display.get"), QStringLiteral("operating.display.read"), QStringLiteral("operating.display.cancel"), QStringLiteral("operating.display.set"), QStringLiteral("operating.display.escape")});
    extension.insert(QStringLiteral("nativeAutoModeVersion"), 1);
    extension.insert(QStringLiteral("nativeAutoModeVerbs"), QStringList{QStringLiteral("auto.mode.get"), QStringLiteral("auto.mode.read"), QStringLiteral("auto.mode.cancel"), QStringLiteral("auto.mode.enabled.set"), QStringLiteral("auto.mode.division.register"), QStringLiteral("auto.mode.division.delete")});
    extension.insert(QStringLiteral("nativeMemoryReadUnavailableReason"), QStringLiteral("Native memory support is disabled in this TS-890S build."));
    extension.insert(QStringLiteral("nativeRadioMessagesVersion"), 1);
    extension.insert(QStringLiteral("nativeRadioMessagesVerbs"), QStringList{QStringLiteral("radio.messages.get"), QStringLiteral("radio.beep.set"), QStringLiteral("radio.beep.reset")});
    extension.insert(QStringLiteral("nativeScopeControlsVerbs"), QStringList{QStringLiteral("scope.controls.get"),
        QStringLiteral("scope.controls.set"), QStringLiteral("scope.controls.reset"), QStringLiteral("scope.controls.action"),
        QStringLiteral("scope.ranges.set"), QStringLiteral("scope.ranges.reset")});
    extension.insert(QStringLiteral("nativeReceiveRoutingVersion"), 1);
    extension.insert(QStringLiteral("nativeReceiveRoutingVerbs"), QStringList{QStringLiteral("receive.routing.get"),
        QStringLiteral("receive.routing.rxAntenna.set"), QStringLiteral("receive.routing.externalOutput.set")});
    extension.insert(QStringLiteral("nativeReceiveAgcVersion"), 1);
    extension.insert(QStringLiteral("nativeReceiveAgcVerbs"), QStringList{QStringLiteral("receive.agc.get"),
        QStringLiteral("receive.agc.preset.set"), QStringLiteral("receive.agc.preset.reset"),
        QStringLiteral("receive.agc.quick.enabled.set"), QStringLiteral("receive.agc.quick.threshold.set"),
        QStringLiteral("receive.agc.quick.threshold.reset"), QStringLiteral("receive.agc.restore")});
    extension.insert(QStringLiteral("nativeReceiveEqControlVersion"), 1);
    extension.insert(QStringLiteral("nativeReceiveEqVerbs"), QStringList{QStringLiteral("receive.eq.get"),
        QStringLiteral("receive.eq.enabled.set"), QStringLiteral("receive.eq.effect.set"), QStringLiteral("receive.eq.curve.set"),
        QStringLiteral("receive.eq.curve.reset"), QStringLiteral("receive.eq.copy"), QStringLiteral("receive.eq.inspection.get")});
    extension.insert(QStringLiteral("powerLimitControlVersion"), 1);
    extension.insert(QStringLiteral("powerLimitTypes"), powerLimitTypes());
    extension.insert(QStringLiteral("powerLimitBands"), powerLimitBands());
    extension.insert(QStringLiteral("transmitInputControlVersion"), 1);
    extension.insert(QStringLiteral("transmitInputChoices"), transmitAudioInputChoices());
    caps.extensions.insert(QStringLiteral("kenwood"), extension);
    caps.panSpanModel = PanSpanModel{false, true};
    caps.panAmplitude = PanAmplitudeModel{false, false, false, PanAmplitudeModel::AxisUnit::RelativeDb};
    caps.clientSettingsDomains = {};
    return caps;
}

QString KenwoodBackend::validateConnectRequest(const RadioConnectRequest& request)
{
    const QString host = request.host;
    if (host.isEmpty() || host.size() > 253 || std::any_of(host.cbegin(), host.cend(), [](QChar character) {
            return character.isSpace() || character.unicode() < 32 || character.unicode() == 127;
        })) {
        return QStringLiteral("Enter a valid TS-890S host name or IPv4 address.");
    }
    QHostAddress numeric;
    if (numeric.setAddress(host) && (numeric.protocol() != QAbstractSocket::IPv4Protocol
        || numeric == QHostAddress(QHostAddress::AnyIPv4) || numeric.isMulticast()
        || numeric == QHostAddress(QHostAddress::Broadcast))) {
        return QStringLiteral("The TS-890S requires a unicast IPv4 destination.");
    }
    if (!request.localBindAddress.isNull() && (request.localBindAddress.protocol() != QAbstractSocket::IPv4Protocol
        || request.localBindAddress.isMulticast() || request.localBindAddress == QHostAddress(QHostAddress::Broadcast))) {
        return QStringLiteral("The TS-890S requires a unicast IPv4 source path.");
    }
    const QVariant variant = request.params.value(QStringLiteral("kenwood.variant"));
    if ((variant.isValid() && variant.typeId() != QMetaType::QString) || !maximumFrequencyForVariant(variant.toString())) {
        return QStringLiteral("Select a supported TS-890S regional variant.");
    }
    const QVariant administrator = request.params.value(QStringLiteral("kenwood.administrator"));
    if (administrator.isValid() && administrator.typeId() != QMetaType::Bool) {
        return QStringLiteral("The KNS administrator account selector must be a boolean.");
    }
    const auto fixedPort = [&request](const QString& key, int required) {
        const QVariant value = request.params.value(key);
        if (!value.isValid()) { return true; }
        const int type = value.typeId();
        const bool numeric = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
            || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float
            || type == QMetaType::Short || type == QMetaType::UShort;
        bool converted = false;
        const double port = value.toDouble(&converted);
        return numeric && converted && std::isfinite(port) && port == required;
    };
    if (!fixedPort(QStringLiteral("kenwood.audioPort"), 60001)) {
        return QStringLiteral("The TS-890S requires UDP audio port 60001.");
    }
    if (!fixedPort(QStringLiteral("kenwood.txSourcePort"), 60002)) {
        return QStringLiteral("The TS-890S requires transmit audio source port 60002.");
    }
    const QVariant user = request.params.value(QStringLiteral("kenwood.username"));
    const QVariant password = request.params.value(QStringLiteral("kenwood.password"));
    if (user.typeId() != QMetaType::QString || password.typeId() != QMetaType::QString
        || !KnsProtocol::credentialsValid(user.toString(), password.toString())) {
        return QStringLiteral("KNS credentials must contain 1–32 ASCII characters without semicolons.");
    }
    return {};
}

void KenwoodBackend::connectRadio(const RadioConnectRequest& request)
{
    const QPointer<KenwoodBackend> guard(this);
    if (m_destructing) { return; }
    const quint64 entryGeneration = m_generation;
    stop();
    if (!guard || m_generation != entryGeneration + 1) { return; }
    const QString error = validateConnectRequest(request);
    if (!error.isEmpty()) {
        emit connectionError(error);
        return;
    }
    const std::optional<qint64> maximum = maximumFrequencyForVariant(
        request.params.value(QStringLiteral("kenwood.variant")).toString());
    m_maximumFrequencyHz = *maximum;
    m_clock.start();
    m_loginRevision = request.params.value(QStringLiteral("connection.loginRevision")).toULongLong();
    m_loginHost = request.host;
    const quint64 generation = m_generation;
    m_tcp = std::make_unique<QTcpSocket>(this);
    m_udp = std::make_unique<QUdpSocket>(this);
    m_txUdp = std::make_unique<QUdpSocket>(this);
    m_tcp->setReadBufferSize(65536);
    // Bind exclusively: another session must not consume this radio's audio.
    const QHostAddress local = request.localBindAddress.isNull()
        ? QHostAddress(QHostAddress::AnyIPv4) : request.localBindAddress;
    if (!request.localBindAddress.isNull()
        && (request.localBindAddress.protocol() != QAbstractSocket::IPv4Protocol || !m_tcp->bind(local, 0))) {
        fail(QStringLiteral("Cannot bind the TS-890S control connection to the selected IPv4 source path."));
        return;
    }
    constexpr quint16 audioPort = 60001;
    if (!m_udp->bind(local, static_cast<quint16>(audioPort), QAbstractSocket::DontShareAddress)) {
        fail(QStringLiteral("Cannot bind the TS-890S network audio port."));
        return;
    }
    m_audioPort = static_cast<quint16>(audioPort);
    constexpr quint16 sourcePort = 60002;
    if (!m_txUdp->bind(local, sourcePort, QAbstractSocket::DontShareAddress)) {
        fail(QStringLiteral("Cannot bind the TS-890S transmit audio source port."));
        return;
    }
    connect(m_udp.get(), &QUdpSocket::readyRead, this, [this, guard, generation] {
        if (guard && generation == m_generation) { readAudio(); }
    });
    connect(m_tcp.get(), &QTcpSocket::readyRead, this, [this, guard, generation] {
        if (guard && generation == m_generation) { readControl(); }
    });
    connect(m_tcp.get(), &QTcpSocket::errorOccurred, this, [this, guard, generation](QAbstractSocket::SocketError) {
        if (guard && generation == m_generation && !m_stopping) {
            fail(QStringLiteral("TS-890S control connection failed."));
        }
    });
    connect(m_tcp.get(), &QTcpSocket::disconnected, this, [this, guard, generation] {
        if (guard && generation == m_generation && !m_stopping) {
            fail(QStringLiteral("TS-890S control connection closed."));
        }
    });
    const QString user = request.params.value(QStringLiteral("kenwood.username")).toString();
    const QString password = request.params.value(QStringLiteral("kenwood.password")).toString();
    const bool admin = request.params.value(QStringLiteral("kenwood.administrator"), false).toBool();
    connect(m_tcp.get(), &QTcpSocket::connected, this, [this, guard, generation, user, password, admin] {
        if (!guard || generation != m_generation) { return; }
        m_peer = m_tcp->peerAddress();
        m_loginAdministrator = admin;
        consume(m_protocol.start(user, password, admin, m_clock.elapsed()));
    }, Qt::SingleShotConnection);
    m_timer.start();
    m_tcp->connectToHost(request.host, request.port ? request.port : 60000,
                         QIODevice::ReadWrite, QAbstractSocket::IPv4Protocol);
    if (!guard || generation != m_generation) { return; }
    // DNS and TCP establishment are bounded independently of the KNS login.
    QTimer::singleShot(5000, this, [this, guard, generation] {
        if (guard && generation == m_generation && m_tcp
            && m_tcp->state() != QAbstractSocket::ConnectedState) {
            fail(QStringLiteral("TS-890S TCP connection timed out."));
        }
    });
}

void KenwoodBackend::stop()
{
    if (m_stopping) { return; }
    m_stopping = true;
    ++m_generation;
    resetRelativeTuning();
    resetTuningControls();
    resetFilterBanks();
    resetFmWidth();
    resetReceiveEq();
    resetReceiveAgc();
    resetReceiveRouting();
    resetScopeControls();
    resetPskReceive();
    resetReceiverOptions();
    resetScanPreferences();
    resetVfoScan();
    resetTransverter();
    resetCwReceive();
    resetOperatingDisplay();
    resetAutoMode();
    resetMemoryInventory();
    resetRadioMessages();
    std::optional<TuneRequest> retiredTune = std::move(m_tune);
    const TxCoordinator::Operation retiredCw = m_cwOperation;
    const TxCoordinator::Operation retiredAtu = m_atuOperation;
    ++m_cwSerial;
    ++m_atuSerial;
    m_atuProgressObserved = false;
    m_atuMinimumReplyBatch = 0;
    m_tune.reset();
    ++m_tuneSerial;
    m_tuneObservationDepth = 0;
    m_timer.stop();
    m_txTimer.stop();
    clearTxAudio();
    m_keyIntent = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_atuIntent = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_cwIntent = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_cwOperation = {};
    m_atuOperation = {};
    m_txObserved = false;
    m_txStateKnown = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_receiveControlValues.clear();
    m_receiveControlsPending.clear();
    m_receiveControlMinimumBatches.clear();
    m_receiveControlsInterested = false;
    m_receiveFskSpacing.reset();
    m_receiveControlsContext.clear();
    ++m_receiveControlsRevision;
    m_powerLimits.clear();
    m_powerLimiterEnabled.reset();
    m_powerLimitSelection.reset();
    m_nextPowerPollMs = 0;
    m_txPermitted = m_loginEnabled = false;
    m_loginAdministrator = false;
    m_voipInputLevel = m_voipOutputLevel = -1;
    m_nextVoipPollMs = 0;
    m_identityConfirmed = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_initialStatusPending = false;
    m_pcAudioWanted = m_dataLan = false;
    m_dataSourceKnown = false;
    m_audioInputSelection[0] = m_audioInputSelection[1] = -1;
    m_audioInputPending[0] = m_audioInputPending[1] = false;
    m_nextInputPollMs = 0;
    m_txOperation = {};
    m_txSequence = 0;
    retirePcmStreams();
    m_networkSpeaker.invalidate();
    m_networkSlice.invalidate();
    m_rxDiscontinuity = true;
    m_protocol.reset();
    if (m_tcp) {
        m_tcp->disconnect(this);
        m_tcp->abort();
        // Qt may still be delivering a socket signal into this method.
        m_tcp.release()->deleteLater();
    }
    if (m_udp) {
        m_udp->disconnect(this);
        m_udp->close();
        m_udp.release()->deleteLater();
    }
    if (m_txUdp) {
        m_txUdp->close();
        m_txUdp.release()->deleteLater();
    }
    m_audioSequence.reset();
    m_rxResampler.reset();
    m_peer.clear();
    m_receiveVfo = m_transmitVfo = m_scopeMode = m_scopeExpand = m_scopeShift = m_fixedRange = -1;
    m_maximumFrequencyHz = kDefaultMaximumFrequencyHz;
    m_scopeLowHz = m_scopeHighHz = 0;
    m_spanHz = 0;
    m_frequency[0] = m_frequency[1] = 0;
    m_vfoModes[0].clear();
    m_vfoModes[1].clear();
    m_displayModes[0].clear();
    m_displayModes[1].clear();
    m_nextVfoPollMs = 0;
    m_tfSet.reset();
    m_mode.clear();
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    m_driveOutput.reset();
    m_transverterEnabled.reset();
    m_xitEnabled.reset();
    m_incrementalOffsetHz.reset();
    m_lastTunePowerBand.reset();
    m_noiseReduction = m_processorOutput = m_radioSidetone = -1;
    m_nextCwPreferencesPollMs = 0;
    m_cwLowerSideband.reset();
    m_meterReadings.clear();
    m_nextMeterPollMs = 0;
    m_nextRefusalWarningMs = 0;
    for (int group = 0; group < 2; ++group) { m_txFilterLow[group].reset(); m_txFilterHigh[group].reset(); }
    m_ssbFilterStyle = m_dataFilterStyle = m_filterLowIndex = m_filterHighIndex = -1;
    m_receiveDspMonitor.reset();
    m_receiveDspMonitorBatch = m_controlIngressSequence;
    ++m_receiveDspMonitorSerial;
    m_filterBankAxesPending = false;
    m_filterBankAxesAwaitingBank = false;
    m_filterBankAxesSetterDispatched = true;
    m_filterBankAxesPendingId.clear();
    m_filterBankAxesPendingTicket = 0;
    m_filterBankAxesMinimumBatch = m_controlIngressSequence;
    m_loginHost.clear();
    m_loginRevision = 0;
    const bool announced = m_announced;
    m_announced = false;
    m_stopping = false;
    if (announced) { emit disconnected(); }
    if (retiredTune) {
        const bool handed = retiredTune->phase == TuneRequest::Phase::Handoff || retiredTune->phase == TuneRequest::Phase::Active;
        retiredTune->completion.finish();
        retiredTune->operation.retireLocalIntent(handed ? TxCoordinator::LocalIntentEnd::MayHaveKeyed : TxCoordinator::LocalIntentEnd::BeforeKeying);
        retiredTune->retirementReceipt.retireLocalIntent(handed ? TxCoordinator::LocalIntentEnd::MayHaveKeyed : TxCoordinator::LocalIntentEnd::BeforeKeying);
    }
    retiredCw.retireLocalIntent(TxCoordinator::LocalIntentEnd::MayHaveKeyed);
    retiredAtu.retireLocalIntent(TxCoordinator::LocalIntentEnd::MayHaveKeyed);
}

void KenwoodBackend::disconnectRadio()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (m_atuIntent) { setAtu(false, m_atuOperation); }
    if (!guard || generation != m_generation) { return; }
    if (m_cwIntent) { abortCwText(m_cwOperation); }
    if (!guard || generation != m_generation) { return; }
    if (m_keyIntent) { setKeying(false, m_txOperation); }
    if (!guard || generation != m_generation) { return; }
    if (isConnected()) { send("##VP0"); }
    if (guard && generation == m_generation) { stop(); }
}

void KenwoodBackend::fail(const QString& reason)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    // Error observers may reenter controls before teardown completes.
    // Retire dispatch and PCM authority before publishing the failure.
    m_protocol.reset();
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    clearTxAudio();
    m_keyIntent = false;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    retirePcmStreams();
    m_networkSpeaker.invalidate();
    m_networkSlice.invalidate();
    emit connectionError(reason);
    if (guard && generation == m_generation) { stop(); }
}

bool KenwoodBackend::write(const QByteArray& bytes)
{
    if (m_transport.control) {
        const std::function<bool(const QByteArray&)> sink = m_transport.control;
        return sink(bytes);
    }
    if (!m_tcp || m_tcp->state() != QAbstractSocket::ConnectedState) { return false; }
    if (m_tcp->bytesToWrite() + bytes.size() > 16384) {
        fail(QStringLiteral("TS-890S command queue exceeded its limit."));
        return false;
    }
    if (m_tcp->write(bytes) != bytes.size()) {
        fail(QStringLiteral("TS-890S control write failed."));
        return false;
    }
    return true;
}

bool KenwoodBackend::send(const QByteArray& body)
{
    const std::optional<QByteArray> encoded = KnsProtocol::command(body);
    if (!isConnected() || (!m_identityConfirmed && body != "ID" && body != "PS") || !encoded) { return false; }
    if (qEnvironmentVariableIsSet("AETHER_KENWOOD_APF_TRACE")
        && (body.startsWith("AP0") || body == "AP1" || body == "AP2" || body == "AP3" || body == "MO2")) {
        qCInfo(lcKenwood) << "APF diagnostic send" << body.left(8).toHex();
    }
    if (tuneMutation(body)) {
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        cancelTune(QStringLiteral("TUNE cancelled because its transmit target was edited."));
        if (!guard || generation != m_generation || !isConnected() || m_tune) { return false; }
    }
    if (body == "RX" || body == "TX0" || body == "TX1" || body == "TX2"
        || body == "AC111" || body == "AC110" || body == "KY0" || body.startsWith("KY2")) {
        // A command handoff is not a current radio-state observation.
        m_txStateKnown = false;
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        m_initialStatusPending = false;
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        emit meterRemoved(103);
        if (!guard || generation != m_generation) { return false; }
        publishNativeFilter();
        if (!guard || generation != m_generation) { return false; }
    }
    return write(*encoded);
}

bool KenwoodBackend::sendObserved(const QByteArray& body, const QByteArray& query)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (!send(body) || !guard || generation != m_generation) { return false; }
    return send(query) && guard && generation == m_generation && isConnected();
}

void KenwoodBackend::consume(KnsProtocol::Events events)
{
    const QPointer<KenwoodBackend> guard(this);
    if (!events.error.isEmpty()) { fail(events.error); return; }
    const quint64 generation = m_generation;
    const quint64 ingressBatch = ++m_controlIngressSequence;
    for (const QByteArray& bytes : events.outgoing) {
        if (!write(bytes)) { return; }
        if (!guard || generation != m_generation) { return; }
    }
    if (events.authenticated) { ready(); }
    for (const QByteArray& message : events.messages) {
        if (!guard || generation != m_generation) { return; }
        adopt(message, ingressBatch);
    }
}

void KenwoodBackend::ready()
{
    const QPointer<KenwoodBackend> guard(this);
    m_rxResampler = std::make_unique<Resampler>(16000, 24000, 640);
    m_announced = true;
    m_identityDeadlineMs = m_clock.elapsed() + 5000;
    const quint64 generation = m_generation;
    emit connected();
    if (!guard || generation != m_generation) { return; }
    if (!m_networkSpeaker.start(PcmPurpose::Speaker, -1, {}, pcmSession())
        || !m_networkSlice.start(PcmPurpose::Slice, 0, {}, pcmSession())) {
        fail(QStringLiteral("Cannot start the TS-890S audio stream."));
        return;
    }
    emit capabilitiesChanged();
    if (!guard || generation != m_generation) { return; }
    emit panCenterBandwidthChanged(QLatin1String(kPan), 0, 0);
    if (!guard || generation != m_generation) { return; }
    emit panRangeChanged(QLatin1String(kPan), -100, 0);
    if (!guard || generation != m_generation) { return; }
    SliceDelta slice;
    slice.panId = QLatin1String(kPan);
    slice.inUse = true;
    slice.active = true;
    slice.modeList = kModes;
    slice.rxAntennaList = QStringList{"ANT1", "ANT2", "RX-ANT"};
    slice.txAntennaList = QStringList{"ANT1", "ANT2"};
    emit sliceChanged(0, slice);
    if (!guard || generation != m_generation) { return; }
    emit panBandwidthLimitsChanged(QLatin1String(kPan), 0.005, 0.5);
    if (!guard || generation != m_generation) { return; }
    emit panRfGainInfoChanged(QLatin1String(kPan), 0, 100, 1, QStringLiteral("%"));
    if (!guard || generation != m_generation) { return; }
    emit panPreampInfoChanged(QLatin1String(kPan), {"OFF", "PRE1", "PRE2"});
    if (!guard || generation != m_generation) { return; }
    emit panAttenuatorInfoChanged(QLatin1String(kPan), {"OFF", "6 dB", "12 dB", "18 dB"});
    if (!guard || generation != m_generation) { return; }
    emit panScopeModesChanged(QLatin1String(kPan), {"Center", "Fixed", "Auto-scroll"});
    // Read power before identity/bootstrap: KNS can authenticate in standby.
    // A power transition retires this session rather than replaying its intents.
    if (!guard || generation != m_generation || !send("PS")) { return; }
    if (guard && generation == m_generation) { send("ID"); }
}

void KenwoodBackend::synchronizeRadio()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    m_nextVoipPollMs = m_clock.elapsed() + 5000;
    m_nextPowerPollMs = m_clock.elapsed() + 5000;
    m_nextVfoPollMs = m_clock.elapsed() + 5000;
    m_nextCwPreferencesPollMs = m_clock.elapsed() + 5000;
    m_nextInputPollMs = m_clock.elapsed() + 5000;
    const QList<QByteArray> queries{"XV", "AI2", "FV", "IF", "TS", "FR", "FT", "FA", "FB", "OM0", "OM1", "SF0", "SF1", "RG", "AG", "MU", "SQ", "RT", "XT", "RF",
        "PA", "RA", "NR", "RL1", "NB1", "NL1", "NB2", "BC", "MO2", "AN", "PC", "MG", "PR0", "PL", "MO0", "ML", "MS0", "MS1", "AC", "KS", "PT", "BI", "SD",
        "EX00606", "EX00607", "EX00608", "EX00609", "EX00611", "EX00612", "EX00502", "EX00503", "EX00505", "EX00102", "##KN30", "##KN31", "LP2", "RM11", "RM21", "BS3", "BS4", "BSK", "BSO", "DD01", "##VP1"};
    for (const QByteArray& query : queries) {
        if (!guard || generation != m_generation) { return; }
        if (query == "IF") { m_initialStatusPending = !m_txStateKnown; }
        if (!send(query)) { return; }
    }
    if (!guard || generation != m_generation) { return; }
    if (m_powerLimitSelection) {
        const std::optional<QByteArray> query = queryPowerLimit(m_powerLimitSelection->first, m_powerLimitSelection->second);
        if (query && !send(*query)) { return; }
        if (!guard || generation != m_generation) { return; }
    }
    for (const QString& name : {QStringLiteral("SM"), QStringLiteral("ALC"), QStringLiteral("SWR")}) {
        MeterDef meter;
        meter.index = name == QLatin1String("SM") ? 100 : name == QLatin1String("ALC") ? 101 : 102;
        meter.source = QStringLiteral("RAD");
        meter.name = QStringLiteral("KENWOOD_%1_RAW").arg(name);
        meter.unit = QStringLiteral("None");
        meter.low = 0;
        meter.high = 70;
        meter.description = QStringLiteral("Kenwood %1 display deflection in dots; not a calibrated physical unit.").arg(name);
        emit meterDefined(meter);
        if (!guard || generation != m_generation) { return; }
    }
    emit meterDefined(receiveLevelDefinition());
}

void KenwoodBackend::publishTransmitObservation(bool transmitting, std::optional<bool> tune)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    m_txStateKnown = true;
    m_txObserved = transmitting;
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    if (transmitting) {
        emit meterRemoved(103);
        if (!guard || generation != m_generation) { return; }
    }
    TransmitDelta delta;
    delta.mox = transmitting;
    delta.tune = tune;
    emit transmitChanged(delta);
    if (!guard || generation != m_generation) { return; }
    emit keyingStateConfirmed(transmitting);
    if (!guard || generation != m_generation) { return; }
    publishVfos();
    if (!guard || generation != m_generation) { return; }
    publishNativeFilter();
    if (guard && generation == m_generation) { publishPowerLimits(); }
}

void KenwoodBackend::readControl()
{
    const QPointer<KenwoodBackend> guard(this);
    if (!m_tcp) { return; }
    const quint64 entryGeneration = m_generation;
    consume(m_protocol.receive(m_tcp->read(16384), m_clock.elapsed()));
    if (!guard || entryGeneration != m_generation) { return; }
    if (m_tcp && m_tcp->bytesAvailable() > 0) {
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this, [this, guard, generation] {
            if (guard && generation == m_generation) { readControl(); }
        });
    }
}

void KenwoodBackend::readAudio()
{
    const QPointer<KenwoodBackend> guard(this);
    if (!m_udp) { return; }
    int budget = 32;
    while (m_udp && m_udp->hasPendingDatagrams() && budget-- > 0) {
        const QNetworkDatagram datagram = m_udp->receiveDatagram(2049);
        if (!isConnected() || datagram.senderAddress() != m_peer || !m_rxResampler) { continue; }
        const quint64 generation = m_generation;
        adoptAudio(datagram.data(), m_clock.elapsed());
        if (!guard || generation != m_generation) { return; }
    }
    if (m_udp && m_udp->hasPendingDatagrams()) {
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this, [this, guard, generation] {
            if (guard && generation == m_generation) { readAudio(); }
        });
    }
}

void KenwoodBackend::adoptAudio(const QByteArray& packet, qint64 nowMs)
{
    const QPointer<KenwoodBackend> guard(this);
    if (!isConnected() || !m_identityConfirmed || !m_rxResampler) { return; }
    const std::optional<AudioPacket> audio = decodeAudio(packet);
    if (!audio) { return; }
    const AudioSequence::Result disposition = m_audioSequence.accept(audio->sequence, audio->source, nowMs);
    if (disposition == AudioSequence::Result::Drop) { return; }
    if (disposition != AudioSequence::Result::Continuous) {
        m_rxResampler->reset();
        m_rxDiscontinuity = true;
    }
    const QByteArray stereo = m_rxResampler->processMonoToStereo(audio->samples.constData(), audio->samples.size());
    if (stereo.isEmpty()) { return; }
    QVector<float> samples(stereo.size() / sizeof(float));
    std::memcpy(samples.data(), stereo.constData(), stereo.size());
    const std::optional<PcmFrame> slice = m_networkSlice.produce(samples, std::nullopt, m_rxDiscontinuity);
    const std::optional<PcmFrame> speaker = m_networkSpeaker.produce(std::move(samples), std::nullopt, m_rxDiscontinuity);
    m_rxDiscontinuity = false;
    const quint64 generation = m_generation;
    if (slice) { emit sliceAudioFrameReady(0, *slice); }
    if (!guard || generation != m_generation) { return; }
    if (speaker) { emit audioFrameReady(*speaker); }
}

int KenwoodBackend::scopeSpanHzForIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(kSpans.size()) ? kSpans[index] : 0;
}

void KenwoodBackend::publishGeometry()
{
    if (!ordinaryReceiveCoordinatesAvailable() || m_nativeScopeProjectionPending) { return; }
    if (m_scopeMode == 0 && m_scopeShift == 4 && m_receiveVfo >= 0
        && m_frequency[m_receiveVfo] > 0 && m_spanHz > 0) {
        emit panCenterBandwidthChanged(QLatin1String(kPan), m_frequency[m_receiveVfo] / 1e6, m_spanHz / 1e6);
    } else if ((m_scopeMode == 1 || m_scopeMode == 2) && m_scopeHighHz > m_scopeLowHz) {
        emit panCenterBandwidthChanged(QLatin1String(kPan), (m_scopeLowHz + m_scopeHighHz) / 2e6,
                                        (m_scopeHighHz - m_scopeLowHz) / 1e6);
    }
}

QVariantMap KenwoodBackend::vfoStatus() const
{
    QVariantMap status{{QStringLiteral("statusRevision"), QString::number(m_vfoStatusRevision)},
        {QStringLiteral("txActive"), m_txObserved},
        {QStringLiteral("transmitSelectable"), m_receiveVfo != kMemoryVfo && m_transmitVfo != kMemoryVfo}};
    if (m_receiveVfo == kMemoryVfo) { status.insert(QStringLiteral("receiveSource"), QStringLiteral("memory")); }
    if (m_transmitVfo == kMemoryVfo) { status.insert(QStringLiteral("transmitSource"), QStringLiteral("memory")); }
    if (!m_vfoModes[0].isEmpty()) { status.insert(QStringLiteral("modeA"), m_vfoModes[0]); }
    if (!m_vfoModes[1].isEmpty()) { status.insert(QStringLiteral("modeB"), m_vfoModes[1]); }
    if (m_receiveVfo >= 0) {
        status.insert(QStringLiteral("receive"), m_receiveVfo == 0 ? "A" : "B");
    }
    if (m_transmitVfo >= 0) {
        status.insert(QStringLiteral("transmit"), m_transmitVfo == 0 ? "A" : "B");
    }
    if (m_receiveVfo >= 0 && m_transmitVfo >= 0) {
        status.insert(QStringLiteral("split"), m_receiveVfo != m_transmitVfo);
    }
    if (ordinaryReceiveCoordinatesAvailable() && m_frequency[0] > 0) { status.insert(QStringLiteral("frequencyAHz"), m_frequency[0]); }
    if (ordinaryReceiveCoordinatesAvailable() && m_frequency[1] > 0) { status.insert(QStringLiteral("frequencyBHz"), m_frequency[1]); }
    return status;
}

void KenwoodBackend::publishVfos()
{
    ++m_vfoStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("vfo"), vfoStatus());
}

void KenwoodBackend::publishTransmitVfo()
{
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    TransmitDelta delta;
    delta.transmitFreq = m_transmitVfo >= 0 && m_frequency[m_transmitVfo] > 0 ? m_frequency[m_transmitVfo] / 1e6 : 0.0;
    delta.txSliceMode = m_transmitVfo >= 0 ? m_vfoModes[m_transmitVfo] : QString{};
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    emit transmitChanged(delta);
    if (!guard || generation != m_generation) { return; }
    publishTxFilter();
    if (guard && generation == m_generation) { refreshTunePower(); }
}

bool KenwoodBackend::txFilterKnown() const
{
    if (!isConnected() || !m_identityConfirmed || m_transmitVfo < 0) { return false; }
    const std::optional<int> group = txFilterGroup(m_vfoModes[m_transmitVfo]);
    return group && m_txFilterLow[*group] && m_txFilterHigh[*group];
}

void KenwoodBackend::publishTxFilter()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    emit capabilitiesChanged();
    if (!guard || generation != m_generation || !txFilterKnown()) { return; }
    const int group = *txFilterGroup(m_vfoModes[m_transmitVfo]);
    TransmitDelta delta;
    delta.txFilterLow = *m_txFilterLow[group];
    delta.txFilterHigh = *m_txFilterHigh[group];
    emit transmitChanged(delta);
}

void KenwoodBackend::queryTransmitFilter()
{
    if (m_transmitVfo < 0) { return; }
    const std::optional<int> group = txFilterGroup(m_vfoModes[m_transmitVfo]);
    if (!group) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    for (const QByteArray& query : queryTxFilter(*group)) {
        if (!send(query) || !guard || generation != m_generation) { return; }
    }
}

void KenwoodBackend::queryVfos()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (!send("TS") || !guard || generation != m_generation) { return; }
    sendObserved("SF0", "SF1");
}

void KenwoodBackend::pollVfos(qint64 nowMs)
{
    if (!isConnected() || !m_identityConfirmed || nowMs < m_nextVfoPollMs) { return; }
    m_nextVfoPollMs = nowMs + 5000;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    queryVfos();
    if (guard && generation == m_generation) { queryTransmitFilter(); }
}

void KenwoodBackend::pollCwPreferences(qint64 nowMs)
{
    if (!isConnected() || !m_identityConfirmed || nowMs < m_nextCwPreferencesPollMs) { return; }
    m_nextCwPreferencesPollMs = nowMs + 5000;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    for (const QByteArray& query : {QByteArray("EX00102"), QByteArray("EX00502"), QByteArray("EX00503"), QByteArray("EX00505")}) {
        if (!send(query) || !guard || generation != m_generation) { return; }
    }
}

void KenwoodBackend::invalidateVfoModes()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    m_vfoModes[0].clear();
    m_vfoModes[1].clear();
    m_mode.clear();
    refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    ++m_filterRevision;
    m_filterLowIndex = m_filterHighIndex = -1;
    publishTransmitVfo();
    if (!guard || generation != m_generation) { return; }
    publishNativeFilter();
    if (guard && generation == m_generation) { publishVfos(); }
}

void KenwoodBackend::adopt(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    if (!isConnected()) { return; }
    const quint64 publicationGeneration = m_generation;
    if (!ingressBatch) { ingressBatch = ++m_controlIngressSequence; }
    // Only a native-relative handoff activates this per-target chronology fence.
    // An older coalesced outer packet cannot replace its newer nested readback.
    int relativeFrequencyTarget = -1;
    if (decodeFrequency(message)) { relativeFrequencyTarget = message[1] == 'A' ? 0 : 1; }
    else if (const std::optional<VfoState> state = decodeVfoState(message)) { relativeFrequencyTarget = state->vfo; }
    if (relativeFrequencyTarget >= 0 && m_relativeFrequencyOwned[relativeFrequencyTarget]) {
        if (ingressBatch <= m_relativeFrequencyFloor[relativeFrequencyTarget]
            || ingressBatch < m_relativeFrequencyLatest[relativeFrequencyTarget]) { return; }
        m_relativeFrequencyLatest[relativeFrequencyTarget] = ingressBatch;
    }
    if (qEnvironmentVariableIsSet("AETHER_KENWOOD_APF_TRACE")
        && (message.startsWith("AP0") || message.startsWith("AP1") || message.startsWith("AP2")
            || message.startsWith("AP3") || message.startsWith("MO2") || message == "?")) {
        qCInfo(lcKenwood) << "APF diagnostic receive" << message.left(16).toHex()
            << "batch" << ingressBatch << "context floor" << m_receiveControlsMinimumBatch
            << "APF floor" << m_receiveControlMinimumBatches.value(QStringLiteral("apf.enabled"), 0)
            << "receive editable" << receiveControlsWritable();
    }
    adoptFmWidth(message, ingressBatch);
    adoptRelativeTuning(message, ingressBatch);
    adoptPskReceive(message, ingressBatch);
    adoptReceiverOptions(message, ingressBatch);
    adoptScanPreferences(message, ingressBatch);
    adoptVfoScan(message, ingressBatch);
    const std::optional<bool> previousTransverter = m_transverterEnabled;
    const bool transverterFrame = message == "XV0" || message == "XV1";
    const quint64 transverterObservation = transverterFrame ? preflightTransverterObservation(message, ingressBatch) : 0;
    if (transverterFrame && !transverterObservation) { return; }
    if (transverterFrame) {
        qualifyTransverterEnabled(previousTransverter, ingressBatch);
        if (message == "XV1") {
            fail(QStringLiteral("TS-890S transverter On is unsupported in this experimental RX build. Turn it off on the radio before reconnecting. No setting was changed."));
            return;
        }
    }
    preflightTransverterBank(message, ingressBatch);
    adoptTransverter(message, ingressBatch);
    preflightOperatingDisplayBank(message, ingressBatch);
    adoptOperatingDisplay(message, ingressBatch);
    refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
    preflightCwReceiveBank(message, ingressBatch);
    adoptCwReceive(message, ingressBatch);

    ++m_tuneObservationDepth;
    const auto tuneObservation = qScopeGuard([guard, publicationGeneration] {
        if (guard && publicationGeneration == guard->m_generation) {
            --guard->m_tuneObservationDepth;
            if (guard->m_tuneObservationDepth == 0) { guard->advanceTunePreflight(); }
        }
    });
    const auto receiveObservation = qScopeGuard([guard, publicationGeneration, message, ingressBatch] {
        if (!guard || publicationGeneration != guard->m_generation) { return; }
        guard->refreshReceiveControls(ingressBatch);
        if (!guard || publicationGeneration != guard->m_generation) { return; }
        guard->adoptReceiveControl(message, ingressBatch);
        if (guard && publicationGeneration == guard->m_generation && message.startsWith("AP0")
            && qEnvironmentVariableIsSet("AETHER_KENWOOD_APF_TRACE")) {
            qCInfo(lcKenwood) << "APF diagnostic adopted" << guard->m_receiveControlValues.value(QStringLiteral("apf.enabled"), -1);
        }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptTuningControl(message, ingressBatch); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptFilterBank(message, ingressBatch); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptReceiveEq(message, ingressBatch); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptReceiveAgc(message, ingressBatch); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptReceiveRoutingContext(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptScopeControlsContext(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->refreshRadioMessagesAvailability(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->adoptAutoMode(message, ingressBatch); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_operatingDisplayDirty) { guard->publishOperatingDisplay(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->refreshCwReceiveContext(); guard->refreshTransverterContext(); guard->refreshVfoScanContext(); guard->refreshScanPreferencesContext(); guard->refreshReceiverOptionsContext(); guard->refreshPskReceiveContext(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_cwReceiveDirty) { guard->publishCwReceive(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_cwReceiveTextDirty) { guard->publishCwReceiveText(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_transverterDirty) { guard->publishTransverter(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_vfoScanDirty) { guard->publishVfoScan(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_pskReceiveDirty) { guard->publishPskReceive(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->refreshRelativeTuningContext(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_relativeDirty) { guard->pollRelativeTuning(guard->m_clock.elapsed()); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_receiverOptionsDirty) { guard->publishReceiverOptions(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_scanPreferencesDirty) { guard->publishScanPreferences(); }
        if (guard && publicationGeneration == guard->m_generation) { guard->refreshFmWidthSafety(); guard->advanceFmWidthRead(); }
        if (guard && publicationGeneration == guard->m_generation && guard->m_fmWidthDirty) { guard->publishFmWidth(); }
    });
    preflightAutoModeBank(message, ingressBatch);
    adoptMemoryInventory(message, ingressBatch);
    if (!guard || publicationGeneration != m_generation) { return; }
    if (message.startsWith("ME0") || message.startsWith("ME1") || message.startsWith("EX00100")) {
        adoptRadioMessage(message, ingressBatch);
        return;
    }
    if ((message.startsWith("BS") || message.startsWith("DD0") || message.startsWith("DD1") || message.startsWith("AI"))
        && !adoptScopeControl(message, ingressBatch)) { return; }
    if (!guard || publicationGeneration != m_generation) { return; }
    if (message.startsWith("##DD3")) {
        adoptScopeFilterFrame(message, ingressBatch);
        return;
    }
    observeTuneMessage(message, ingressBatch);
    if (!guard || publicationGeneration != m_generation) { return; }
    if (message == "PS0" || message == "PS2" || message == "PS3") {
        // Losing main power does not establish RF idle. The ordinary session
        // teardown revokes producers; reconnect reads a fresh powered state.
        fail(QStringLiteral("TS-890S main power is off or changing. The session was retired; reconnect after the radio has powered on. Transmitter state is unknown."));
    } else if (message == "PS4" || message == "PS5" || message == "PS6") {
        // PC Command Reference p.54 restricts commands during timer recording.
        // Closing this session avoids ordinary polling or editing in that state.
        fail(QStringLiteral("TS-890S timer operation restricts remote control. The session was retired; reconnect after the timer operation has ended. Transmitter state is unknown."));
    } else if (message.startsWith("FV")) {
        // PC Command Reference p.34: FV is a read-only firmware version string.
        if (!m_identityConfirmed) { return; }
        const QByteArray bytes = message.sliced(2);
        if (bytes.size() < 3 || bytes.size() > 24 || bytes.count('.') < 1 || bytes.count('.') > 3) { return; }
        for (char value : bytes) {
            if (value != '.' && (value < '0' || value > '9')) { return; }
        }
        const QString version = QString::fromLatin1(bytes);
        qsizetype suffix = 0;
        const QVersionNumber parsed = QVersionNumber::fromString(version, &suffix);
        if (parsed.isNull() || suffix != version.size()) { return; }
        RadioDelta delta;
        delta.softwareVersion = version;
        emit radioChanged(delta);
    } else if (message.startsWith("ID") && message.size() == 5) {
        if (message != "ID024") {
            fail(QStringLiteral("The connected KNS radio did not identify as a TS-890S."));
            return;
        }
        const bool first = !m_identityConfirmed;
        m_identityConfirmed = true;
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        commitNetworkRadioLogin(QStringLiteral("kenwood"), m_loginHost, m_loginRevision, this);
        if (!guard || publicationGeneration != m_generation) { return; }
        emit capabilitiesChanged();
        if (!guard || publicationGeneration != m_generation) { return; }
        publishNetworkAudioLevels();
        if (!guard || publicationGeneration != m_generation) { return; }
        if (first) { synchronizeRadio(); }
    } else if (message == "##TI0" || message == "##TI1" || message == "##UE0" || message == "##UE1") {
        if (message.startsWith("##TI")) { m_txPermitted = message.endsWith('1'); }
        else { m_loginEnabled = message.endsWith('1'); }
        if ((!m_txPermitted || !m_loginEnabled) && m_keyIntent) { setKeying(false, m_txOperation); }
        if (!guard || publicationGeneration != m_generation) { return; }
        if ((!m_txPermitted || !m_loginEnabled) && m_atuIntent) { setAtu(false, m_atuOperation); }
        if (!guard || publicationGeneration != m_generation) { return; }
        if ((!m_txPermitted || !m_loginEnabled) && m_cwIntent) { abortCwText(m_cwOperation); }
        if (!guard || publicationGeneration != m_generation) { return; }
        emit capabilitiesChanged();
    } else if (message == "TS0" || message == "TS1") {
        m_tfSet = message == "TS1";
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        emit capabilitiesChanged();
        if (guard && publicationGeneration == m_generation) { publishNativeFilter(); }
    } else if (message == "RX" || message == "TX0" || message == "TX1" || message == "TX2") {
        m_initialStatusPending = false;
        publishTransmitObservation(message != "RX", message == "TX2");
    } else if (m_initialStatusPending && message.startsWith("IF")) {
        if (const std::optional<bool> transmitting = decodeStatusTransmit(message)) {
            m_initialStatusPending = false;
            publishTransmitObservation(*transmitting, *transmitting ? std::nullopt : std::optional<bool>(false));
        }
    } else if (const std::optional<PowerLimit> limit = decodePowerLimit(message)) {
        m_powerLimits.insert(limit->type * 12 + limit->band, limit->watts);
        publishPowerLimits();
    } else if (message == "LP20" || message == "LP21") {
        m_powerLimiterEnabled = message == "LP21";
        publishPowerLimits();
    } else if (message.startsWith("##KN3") && message.size() == 9 && (message[5] == '0' || message[5] == '1')) {
        // PC Command Reference (2019-01-30), p.72. These levels are not MG.
        if (const std::optional<int> level = decimal(message.sliced(6), 0, 100)) {
            if (message[5] == '0') { m_voipInputLevel = *level; }
            else { m_voipOutputLevel = *level; }
            publishNetworkAudioLevels();
        }
    } else if (const std::optional<TransmitAudioInput> input = decodeTransmitAudioInput(message)) {
        const bool changed = input->selection != m_audioInputSelection[input->path];
        m_audioInputSelection[input->path] = input->selection;
        m_audioInputPending[input->path] = false;
        if (input->path == 1) {
            if (m_dataLan != (input->selection == 3)) { clearTxAudio(); }
            m_dataLan = input->selection == 3;
            m_dataSourceKnown = true;
        }
        publishTransmitInputs();
        if (!guard || publicationGeneration != m_generation) { return; }
        if (input->path == 1 && changed && m_pcAudioWanted && !m_dataLan) {
            emit configurationWarning(QStringLiteral("PC Audio requires DATA SEND audio input LAN. Toggle PC Audio off and on to select it explicitly."));
        }
    } else if (message.startsWith("SD") && message.size() == 6) {
        if (const std::optional<int> delay = decimal(message.sliced(2), 0, 1000); delay && *delay % 50 == 0) {
            TransmitDelta delta;
            delta.cwDelay = *delay;
            emit transmitChanged(delta);
        }
    } else if (message.startsWith("KS") && message.size() == 5) {
        if (const std::optional<int> speed = decimal(message.sliced(2), 4, 60)) {
            TransmitDelta delta;
            delta.cwSpeed = *speed;
            emit transmitChanged(delta);
        }
    } else if (message.startsWith("PT") && message.size() == 5) {
        if (const std::optional<int> pitch = decimal(message.sliced(2), 0, 160)) {
            TransmitDelta delta;
            delta.cwPitch = 300 + *pitch * 5;
            emit transmitChanged(delta);
        }
    } else if (message == "BI0" || message == "BI1") {
        TransmitDelta delta;
        delta.cwBreakIn = message == "BI1";
        emit transmitChanged(delta);
    } else if ((message.startsWith("SM") && message.size() == 6)
               || ((message.startsWith("RM1") || message.startsWith("RM2")) && message.size() == 7)) {
        const bool signalPower = message.startsWith("SM");
        const std::optional<int> value = decimal(message.sliced(signalPower ? 2 : 3), 0, 70);
        if (value) {
            const QString name = signalPower ? QStringLiteral("SM") : message[2] == '1' ? QStringLiteral("ALC") : QStringLiteral("SWR");
            m_meterReadings.insert(name, *value);
            emit meterUpdate(QStringLiteral("RAD:KENWOOD_%1_RAW").arg(name), *value);
            if (signalPower && guard && publicationGeneration == m_generation && m_txStateKnown && !m_txObserved) {
                emit meterDefined(receiveLevelDefinition());
                if (!guard || publicationGeneration != m_generation) { return; }
                emit meterUpdate(QStringLiteral("SLC0:LEVEL"), *value);
            }
        }
    } else if (message.startsWith("AN") && message.size() == 6 && (message[2] == '1' || message[2] == '2')
               && decimal(message.sliced(3, 1), 0, 1) && decimal(message.sliced(4, 1), 0, 1)
               && decimal(message.sliced(5, 1), 0, 1)) {
        if (!adoptReceiveRouting(message, ingressBatch) || !guard || publicationGeneration != m_generation) { return; }
        const quint64 routingObservation = m_receiveRoutingObservationSerial;
        const bool drive = message[4] == '1';
        const bool changed = !m_driveOutput || *m_driveOutput != drive;
        m_driveOutput = drive;
        const QString main = message[2] == '1' ? QStringLiteral("ANT1") : QStringLiteral("ANT2");
        const QString receive = message[3] == '1' ? QStringLiteral("RX-ANT") : main;
        SliceDelta delta;
        delta.txAntenna = main;
        delta.rxAntenna = receive;
        emit sliceChanged(0, delta);
        if (!guard || publicationGeneration != m_generation || routingObservation != m_receiveRoutingObservationSerial) { return; }
        emit panRxAntennaChanged(QLatin1String(kPan), receive);
        if (!guard || publicationGeneration != m_generation || routingObservation != m_receiveRoutingObservationSerial) { return; }
        if (changed) { emit capabilitiesChanged(); }
        if (guard && publicationGeneration == m_generation && routingObservation == m_receiveRoutingObservationSerial) { refreshTunePower(); }
    } else if (message == "XV0" || message == "XV1") {
        if (transverterObservation != m_transverterObservationSerial || ingressBatch != m_transverterObservationBatch) { return; }
        emit capabilitiesChanged();
        if (!guard || publicationGeneration != m_generation) { return; }
        if (!previousTransverter.has_value()) {
            for (const QByteArray& query : {QByteArray("FA"), QByteArray("FB"), QByteArray("SF0"), QByteArray("SF1")}) {
                if (!send(query) || !guard || publicationGeneration != m_generation) { return; }
            }
        }
        refreshTunePower();
    } else if (message.startsWith("PC") && message.size() == 5 && m_driveOutput && !*m_driveOutput) {
        if (const std::optional<int> value = decimal(message.sliced(2), 5, 100)) {
            TransmitDelta delta;
            delta.rfPower = *value;
            emit transmitChanged(delta);
        }
    } else if (message.startsWith("MG") && message.size() == 5) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 100)) {
            TransmitDelta delta;
            delta.micLevel = *value;
            emit transmitChanged(delta);
        }
    } else if (message.startsWith("PL") && message.size() == 8) {
        const std::optional<int> input = decimal(message.sliced(2, 3), 0, 100);
        const std::optional<int> output = decimal(message.sliced(5, 3), 0, 100);
        if (input && output) {
            const bool firstOutput = m_processorOutput < 0;
            m_processorOutput = *output;
            if (firstOutput) {
                emit capabilitiesChanged();
                if (!guard || publicationGeneration != m_generation) { return; }
            }
            TransmitDelta delta;
            delta.speechProcLevel = *input;
            emit transmitChanged(delta);
        }
    } else if (message == "MO00" || message == "MO01") {
        TransmitDelta delta;
        delta.sbMonitor = message == "MO01";
        emit transmitChanged(delta);
    } else if (message.startsWith("ML") && message.size() == 5) {
        if (const std::optional<int> level = decimal(message.sliced(2), 0, 20)) {
            TransmitDelta delta;
            delta.monGainSb = *level * 5;
            emit transmitChanged(delta);
        }
    } else if (message == "PR00" || message == "PR01") {
        TransmitDelta delta;
        delta.speechProcEnable = message == "PR01";
        emit transmitChanged(delta);
    } else if (message.startsWith("AC") && message.size() == 5
               && decimal(message.sliced(2, 1), 0, 1) && decimal(message.sliced(3, 1), 0, 1)
               && decimal(message.sliced(4, 1), 0, 1)) {
        if (m_atuIntent && ingressBatch > m_atuMinimumReplyBatch) {
            if (message[4] == '1') {
                m_atuProgressObserved = true;
            } else if (m_atuProgressObserved) {
                const quint64 retiredSerial = m_atuSerial + 1;
                retireAtuIntent();
                if (!guard || publicationGeneration != m_generation || retiredSerial != m_atuSerial) { return; }
            }
        }
        TransmitDelta delta;
        delta.atuEnabled = message[3] == '1';
        delta.atuStatusRaw = message[4] == '1' ? QStringLiteral("TUNE_IN_PROGRESS") : QStringLiteral("NONE");
        emit transmitChanged(delta);
        if (guard && publicationGeneration == m_generation) { publishPowerLimits(); }
    } else if (const std::optional<TxFilterReport> filter = decodeTxFilter(message)) {
        if (filter->high) { m_txFilterHigh[filter->group] = filter->hz; }
        else { m_txFilterLow[filter->group] = filter->hz; }
        publishTxFilter();
    } else if (const std::optional<VfoState> state = decodeVfoState(message)) {
        const quint64 routingObservation = m_receiveRoutingObservationSerial;
        const bool frequencyChanged = m_frequency[state->vfo] != state->frequencyHz;
        m_frequency[state->vfo] = state->frequencyHz;
        refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        m_vfoModes[state->vfo] = state->mode;
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        if (state->vfo == m_transmitVfo) { publishTransmitVfo(); }
        if (!guard || publicationGeneration != m_generation) { return; }
        if (state->vfo == m_receiveVfo) {
            const bool modeChanged = m_mode != state->mode;
            if (modeChanged) {
                ++m_filterRevision;
                m_filterLowIndex = m_filterHighIndex = -1;
            }
            m_mode = state->mode;
            refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
            emit capabilitiesChanged();
            if (!guard || publicationGeneration != m_generation) { return; }
            SliceDelta delta;
            delta.mode = state->mode;
            if (ordinaryReceiveCoordinatesAvailable()) { delta.frequency = state->frequencyHz / 1e6; }
            if (frequencyChanged && !(routingObservation != m_receiveRoutingObservationSerial
                && m_receiveRoutingAntenna.has_value() && m_receiveRoutingContext == receiveRoutingContextKey())) {
                delta.rxAntenna = QString{};
            }
            emit sliceChanged(0, delta);
            if (!guard || publicationGeneration != m_generation) { return; }
            publishGeometry();
            if (!guard || publicationGeneration != m_generation) { return; }
            publishNativeFilter();
            if (!guard || publicationGeneration != m_generation) { return; }
            if (modeChanged) {
                queryFilter();
                if (!guard || publicationGeneration != m_generation) { return; }
                if (kNonFmModes.contains(m_mode)) { send("GC"); }
            }
        }
        if (guard && publicationGeneration == m_generation) { publishVfos(); }
    } else if (const std::optional<qint64> hz = decodeFrequency(message)) {
        const int vfo = message[1] == 'A' ? 0 : 1;
        const quint64 routingObservation = m_receiveRoutingObservationSerial;
        const bool frequencyChanged = m_frequency[vfo] != *hz;
        m_frequency[vfo] = *hz;
        refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        if (vfo == m_transmitVfo) {
            TransmitDelta delta;
            delta.transmitFreq = *hz / 1e6;
            emit transmitChanged(delta);
            if (!guard || publicationGeneration != m_generation) { return; }
            refreshTunePower();
            if (!guard || publicationGeneration != m_generation) { return; }
        }
        if (vfo == m_receiveVfo && ordinaryReceiveCoordinatesAvailable()) {
            SliceDelta delta;
            delta.frequency = *hz / 1e6;
            if (frequencyChanged && !(routingObservation != m_receiveRoutingObservationSerial
                && m_receiveRoutingAntenna.has_value() && m_receiveRoutingContext == receiveRoutingContextKey())) {
                delta.rxAntenna = QString{};
            }
            emit sliceChanged(0, delta);
            if (!guard || publicationGeneration != m_generation) { return; }
            publishGeometry();
            if (!guard || publicationGeneration != m_generation) { return; }
            if (m_scopeMode == 2) { send("BSM0"); }
        }
        if (!guard || publicationGeneration != m_generation) { return; }
        publishVfos();
    } else if (message == "FT3") {
        m_transmitVfo = kMemoryVfo;
        publishTransmitVfo();
        if (guard && publicationGeneration == m_generation) { publishVfos(); }
    } else if (message == "FR3") {
        const bool roleChanged = m_receiveVfo != kMemoryVfo;
        m_receiveVfo = kMemoryVfo;
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        if (roleChanged) {
            SliceDelta delta;
            delta.receiveTuneContextChanged = true;
            emit sliceChanged(0, delta);
            if (!guard || publicationGeneration != m_generation || m_receiveVfo != kMemoryVfo) { return; }
        }
        m_mode.clear();
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        ++m_filterRevision;
        m_filterLowIndex = m_filterHighIndex = -1;
        if (m_transmitVfo != kMemoryVfo) { m_transmitVfo = -1; }
        publishTransmitVfo();
        if (!guard || publicationGeneration != m_generation) { return; }
        publishNativeFilter();
        if (!guard || publicationGeneration != m_generation) { return; }
        publishVfos();
        if (guard && publicationGeneration == m_generation) { send("FT"); }
    } else if (message == "FT0" || message == "FT1") {
        const int transmitVfo = message[2] - '0';
        if (m_transmitVfo != transmitVfo) { m_vfoModes[transmitVfo].clear(); }
        m_transmitVfo = transmitVfo;
        publishTransmitVfo();
        if (!guard || publicationGeneration != m_generation || m_transmitVfo != transmitVfo) { return; }
        publishVfos();
        if (!guard || publicationGeneration != m_generation || m_transmitVfo != transmitVfo) { return; }
        send("SF" + QByteArray::number(transmitVfo));
    } else if (message == "FR0" || message == "FR1") {
        const int receiveVfo = message[2] - '0';
        const bool leavingMemory = m_receiveVfo == kMemoryVfo;
        const bool roleChanged = m_receiveVfo != receiveVfo;
        if (roleChanged) {
            ++m_filterRevision;
            m_mode.clear();
            refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
            m_filterLowIndex = m_filterHighIndex = -1;
        }
        m_receiveVfo = receiveVfo;
        refreshAutoModeSafety(); refreshOperatingDisplaySafety(); refreshCwReceiveContext(); refreshTransverterContext(); refreshVfoScanContext(); refreshScanPreferencesContext(); refreshReceiverOptionsContext(); refreshRelativeTuningContext(); refreshPskReceiveContext();
        if (roleChanged || m_frequency[receiveVfo] > 0) {
            SliceDelta delta;
            delta.receiveTuneContextChanged = roleChanged;
            if (ordinaryReceiveCoordinatesAvailable() && m_frequency[receiveVfo] > 0) { delta.frequency = m_frequency[receiveVfo] / 1e6; }
            emit sliceChanged(0, delta);
            if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        }
        emit capabilitiesChanged();
        if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        if (leavingMemory) {
            send("FT");
            if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        }
        send("SF" + QByteArray::number(receiveVfo));
        if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        publishGeometry();
        if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        publishVfos();
        if (!guard || publicationGeneration != m_generation || m_receiveVfo != receiveVfo) { return; }
        publishNativeFilter();
    } else if ((message.startsWith("OM0") || message.startsWith("OM1")) && message.size() == 4) {
        if (const std::optional<QString> mode = decodeMode(message[3])) {
            const int display = message[2] - '0';
            if (m_displayModes[display] != *mode) {
                m_displayModes[display] = *mode;
                invalidateVfoModes();
                if (!guard || publicationGeneration != m_generation) { return; }
            }
            queryVfos();
        }
    } else if (message.startsWith("EX00102 ") && message.size() == 11) {
        if (const std::optional<int> value = decimal(message.sliced(8), 0, 21)) {
            const bool first = m_radioSidetone < 0;
            m_radioSidetone = *value;
            TransmitDelta delta;
            delta.cwSidetone = *value != 1;
            emit transmitChanged(delta);
            if (guard && publicationGeneration == m_generation && first) { emit capabilitiesChanged(); }
        }
    } else if (message.startsWith("EX00505 ") && message.size() == 11) {
        if (const std::optional<int> value = decimal(message.sliced(8), 0, 1)) {
            const bool first = !m_cwLowerSideband.has_value();
            m_cwLowerSideband = *value != 0;
            TransmitDelta delta;
            delta.cwlEnabled = *m_cwLowerSideband;
            emit transmitChanged(delta);
            if (guard && publicationGeneration == m_generation && first) { emit capabilitiesChanged(); }
        }
    } else if ((message.startsWith("EX00502 ") || message.startsWith("EX00503 ")) && message.size() == 11) {
        if (const std::optional<int> value = decimal(message.sliced(8), 0, 1)) {
            TransmitDelta delta;
            if (message[6] == '2') { delta.cwIambicMode = *value; }
            else { delta.cwSwapPaddles = *value != 0; }
            emit transmitChanged(delta);
        }
    } else if ((message.startsWith("EX00611 ") || message.startsWith("EX00612 ")) && message.size() == 11) {
        if (const std::optional<int> style = decimal(message.sliced(8), 0, 1)) {
            int& current = message[6] == '1' ? m_ssbFilterStyle : m_dataFilterStyle;
            if (current != *style) {
                current = *style;
                const bool dataStyle = message[6] == '2';
                const bool activeStyle = dataStyle
                    ? m_mode == QLatin1String("DIGL") || m_mode == QLatin1String("DIGU")
                    : m_mode == QLatin1String("LSB") || m_mode == QLatin1String("USB");
                if (activeStyle) {
                    invalidateFilterBankAxes();
                } else {
                    emit capabilitiesChanged();
                    if (!guard || publicationGeneration != m_generation) { return; }
                    publishNativeFilter();
                }
            }
        }
    } else if (message.startsWith("SL0") && message.size() == 5) {
        if (const std::optional<int> index = decimal(message.sliced(3), 0, 98); index && !m_filterBankAxesAwaitingBank && ingressBatch > m_filterBankAxesMinimumBatch) {
            m_filterLowIndex = *index;
            publishFilter();
        }
    } else if (message.startsWith("SH0") && message.size() == 6) {
        if (const std::optional<int> index = decimal(message.sliced(3), 0, 998); index && !m_filterBankAxesAwaitingBank && ingressBatch > m_filterBankAxesMinimumBatch) {
            m_filterHighIndex = *index;
            publishFilter();
        }
    } else if (message.startsWith("GC") && message.size() == 3) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 3)) {
            if (!adoptReceiveAgcSelection(*value, ingressBatch) || !guard || publicationGeneration != m_generation) { return; }
            SliceDelta delta;
            delta.agcMode = QStringList{"off", "slow", "med", "fast"}[*value];
            emit sliceChanged(0, delta);
        }
    } else if (message == "RT0" || message == "RT1" || message == "XT0" || message == "XT1") {
        SliceDelta delta;
        if (message.startsWith("RT")) { delta.ritOn = message.endsWith('1'); }
        else { delta.xitOn = message.endsWith('1'); }
        if (message.startsWith("XT")) { m_xitEnabled = message == "XT1"; }
        emit sliceChanged(0, delta);
        if (guard && publicationGeneration == m_generation && message.startsWith("XT")) { refreshTunePower(); }
    } else if (message.startsWith("RF") && message.size() == 7 && (message[2] == '0' || message[2] == '1')) {
        if (const std::optional<int> offset = decimal(message.sliced(3), 0, 9999)) {
            SliceDelta delta;
            delta.ritFreq = delta.xitFreq = message[2] == '1' ? -*offset : *offset;
            m_incrementalOffsetHz = *delta.xitFreq;
            emit sliceChanged(0, delta);
            if (guard && publicationGeneration == m_generation) { refreshTunePower(); }
        }
    } else if (message.startsWith("AG") && message.size() == 5) {
        if (const std::optional<int> gain = decimal(message.sliced(2), 0, 255)) {
            SliceDelta delta;
            delta.audioGain = qRound(*gain * 100.0 / 255);
            emit sliceChanged(0, delta);
        }
    } else if (message == "MU0" || message == "MU1") {
        SliceDelta delta;
        delta.audioMute = message == "MU1";
        emit sliceChanged(0, delta);
    } else if (message.startsWith("SQ") && message.size() == 5) {
        if (const std::optional<int> level = decimal(message.sliced(2), 0, 255)) {
            SliceDelta delta;
            delta.squelchOn = *level != 0;
            delta.squelchLevel = qRound(*level * 100.0 / 255);
            emit sliceChanged(0, delta);
        }
    } else if (message.startsWith("RG") && message.size() == 5) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 255)) {
            emit panRfGainChanged(QLatin1String(kPan), qRound(*value * 100.0 / 255));
        }
    } else if (message.startsWith("PA") && message.size() == 3) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 2)) {
            emit panPreampChanged(QLatin1String(kPan), *value);
        }
    } else if (message.startsWith("RA") && message.size() == 3) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 3)) {
            emit panAttenuatorChanged(QLatin1String(kPan), *value);
        }
    } else if (message.startsWith("NR") && message.size() == 3) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 2)) {
            m_noiseReduction = *value;
            SliceDelta delta;
            delta.nr = *value != 0;
            emit sliceChanged(0, delta);
            if (!guard || publicationGeneration != m_generation) { return; }
            emit capabilitiesChanged();
        }
    } else if (message.startsWith("RL1") && message.size() == 5) {
        if (const std::optional<int> level = decimal(message.sliced(3), 1, 10)) {
            SliceDelta delta;
            delta.nrLevel = qRound((*level - 1) * 100.0 / 9);
            emit sliceChanged(0, delta);
        }
    } else if (message.startsWith("NL1") && message.size() == 6) {
        if (const std::optional<int> level = decimal(message.sliced(3), 1, 20)) {
            SliceDelta delta;
            delta.nbLevel = qRound((*level - 1) * 100.0 / 19);
            emit sliceChanged(0, delta);
        }
    } else if (message.startsWith("NB1") && message.size() == 4) {
        if (const std::optional<int> value = decimal(message.sliced(3), 0, 1)) {
            SliceDelta delta;
            delta.nb = *value == 1;
            emit sliceChanged(0, delta);
        }
    } else if (message.startsWith("BC") && message.size() == 3) {
        if (const std::optional<int> value = decimal(message.sliced(2), 0, 2)) {
            SliceDelta delta;
            delta.anf = *value != 0;
            emit sliceChanged(0, delta);
        }
    } else if (message.startsWith("BS3") && message.size() == 4 && message[3] >= '0' && message[3] <= '2') {
        if (m_scopeMode != message[3] - '0') { m_scopeSpanIntent.reset(); }
        m_scopeMode = message[3] - '0';
        m_fixedRange = -1;
        m_scopeLowHz = m_scopeHighHz = 0;
        emit panScopeModeChanged(QLatin1String(kPan), m_scopeMode);
        if (!guard || publicationGeneration != m_generation) { return; }
        publishGeometry();
        if (!guard || publicationGeneration != m_generation) { return; }
        if (m_scopeMode != 0 && m_nativeScopePredispatch.isEmpty()) { send("BS5"); }
    } else if (message.startsWith("BS5") && message.size() == 4 && message[3] >= '0' && message[3] <= '3') {
        const int range = message[3] - '0';
        if (range != m_fixedRange) {
            m_fixedRange = range;
            m_scopeLowHz = m_scopeHighHz = 0;
            if ((m_scopeMode == 1 || m_scopeMode == 2) && m_nativeScopePredispatch.isEmpty()
                && !m_nativeScopeBoundsRefreshPending) { send("BSM0"); }
        }
    } else if (message.startsWith("BSK") && message.size() == 4 && message[3] >= '0' && message[3] <= '8') {
        if (m_scopeShift != message[3] - '0') { m_scopeSpanIntent.reset(); }
        m_scopeShift = message[3] - '0';
        publishGeometry();
        if (!guard || publicationGeneration != m_generation) { return; }
        if (m_scopeShift != 4 && m_scopeMode == 0) {
            emit configurationWarning(QStringLiteral("TS-890S shifted Center scope coverage is not verified; select the centered shift position on the radio."));
        }
    } else if (message.startsWith("BSM") && message.size() == 20 && message[3] >= '0' && message[3] <= '3') {
        const std::optional<int> low = decimal(message.sliced(4, 8), 0, 99999999);
        const std::optional<int> high = decimal(message.sliced(12, 8), 0, 99999999);
        if (low && high && *low < *high && (m_scopeMode == 1 || m_scopeMode == 2)
            && (message[3] == '0' || message[3] - '0' == m_fixedRange)) {
            m_scopeLowHz = *low;
            m_scopeHighHz = *high;
            publishGeometry();
        }
    } else if (message.startsWith("BSO") && message.size() == 4 && (message[3] == '0' || message[3] == '1')) {
        if (m_scopeExpand != message[3] - '0') { m_scopeSpanIntent.reset(); }
        m_scopeExpand = message[3] - '0';
        if (m_scopeExpand != 0) {
            emit configurationWarning(QStringLiteral("TS-890S expanded scope coverage is not verified; turn EXPAND off on the radio to display the scope."));
        }
    } else if (message.startsWith("BS4") && message.size() == 4 && message[3] >= '0'
               && message[3] - '0' < static_cast<int>(kSpans.size())) {
        m_spanHz = kSpans[message[3] - '0'];
        if (m_scopeSpanIntent) {
            if (m_spanHz == m_scopeSpanIntent->targetHz) {
                m_scopeSpanIntent->targetObserved = true;
            } else if (m_scopeSpanIntent->targetObserved || m_spanHz != m_scopeSpanIntent->sourceHz) {
                m_scopeSpanIntent.reset();
            }
        }
        publishGeometry();
    } else if (message.startsWith("##DD2")) {
        const std::optional<ScopeLine> scope = decodeScope(message);
        const bool center = m_scopeMode == 0 && m_scopeShift == 4 && m_spanHz > 0
            && m_receiveVfo >= 0 && m_frequency[m_receiveVfo] > 0;
        const bool fixed = (m_scopeMode == 1 || m_scopeMode == 2) && m_scopeHighHz > m_scopeLowHz;
        if (!ordinaryReceiveCoordinatesAvailable() || !scope || (!center && !fixed) || m_scopeExpand != 0 || m_nativeScopeProjectionPending
            || ingressBatch <= m_nativeScopeGeometryFloor
            || (m_nativeScopeAi && *m_nativeScopeAi != 2 && *m_nativeScopeAi != 4)
            || (m_nativeScopeValues.contains("bandscope_output")
                && (m_nativeScopeValues.value("bandscope_output") == 0 || m_nativeScopeValues.value("bandscope_output") >= 4))) { return; }
        // Relative display units only. Capability explicitly withdraws dBm calibration.
        const QVector<float>& relative = scope->levels;
        const QByteArray frame(reinterpret_cast<const char*>(relative.constData()), relative.size() * sizeof(float));
        emit spectrumFrameReady(0, frame);
    } else if (message == "?") {
        // The PC Command Reference (2019-01-30), p.2, does not identify
        // which command failed. Preserve observed state and do not retry it.
        const qint64 nowMs = m_clock.elapsed();
        if (nowMs >= m_nextRefusalWarningMs) {
            m_nextRefusalWarningMs = nowMs + 5000;
            qCWarning(lcKenwood) << "TS-890S rejected a command; live state awaits radio readback";
            emit configurationWarning(QStringLiteral("TS-890S rejected a command because of its syntax or current radio state. Live controls retain the last confirmed radio state."));
        }
    } else if (message == "E" || message == "O") {
        // Framing/overrun errors leave command delivery uncertain. Retire
        // local producers; disconnect is not an observation of radio idle.
        fail(message == "E"
            ? QStringLiteral("TS-890S reported a communication error. The connection was closed; transmitter state is unknown. Check the radio before reconnecting.")
            : QStringLiteral("TS-890S reported a receive buffer overrun. The connection was closed; transmitter state is unknown. Check the radio before reconnecting."));
    }
}

bool KenwoodBackend::ordinaryReceiveCoordinatesAvailable() const
{
    return isConnected() && m_identityConfirmed && m_transverterKnown
        && m_transverterEnabled == std::optional<bool>(false);
}

void KenwoodBackend::setSliceFrequency(int sliceId, double hz)
{
    if (!ordinaryReceiveCoordinatesAvailable() || sliceId != 0 || m_receiveVfo < 0 || !std::isfinite(hz)
        || hz < kMinimumFrequencyHz || hz > m_maximumFrequencyHz) { return; }
    if (const std::optional<QByteArray> command = encodeFrequency(m_receiveVfo == 0 ? 'A' : 'B', std::llround(hz))) {
        sendObserved(*command, m_receiveVfo == 0 ? "FA" : "FB");
    }
}

bool KenwoodBackend::requestReceiveTune(int sliceId, double hz, ReceiveTuneView view)
{
    if (!ordinaryReceiveCoordinatesAvailable() || sliceId != 0 || m_receiveVfo < 0 || m_receiveVfo > 1
        || !std::isfinite(hz) || hz < kMinimumFrequencyHz || hz > m_maximumFrequencyHz
        || (view != ReceiveTuneView::Preserve && view != ReceiveTuneView::Reveal
            && view != ReceiveTuneView::Center)) { return false; }
    // The Center scope follows its VFO. Fixed scope bounds remain a separate
    // operator intent; a receive tune never changes the stored scope range.
    const std::optional<QByteArray> command = encodeFrequency(m_receiveVfo == 0 ? 'A' : 'B', std::llround(hz));
    return command && sendObserved(*command, m_receiveVfo == 0 ? "FA" : "FB");
}

bool KenwoodBackend::receiveModeWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_transmitVfo != kMemoryVfo && !m_mode.isEmpty()
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent;
}

void KenwoodBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (sliceId != 0) { return; }
    if (!receiveModeWritable()) {
        emit configurationWarning(QStringLiteral("Receive mode changes require confirmed receive state with TF-SET off."));
        return;
    }
    if (const std::optional<char> code = encodeMode(mode)) { sendObserved(QByteArray("OM0") + *code, "OM0"); }
}

void KenwoodBackend::publishFilter()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (!isConnected()) { return; }
    if (m_receiveDspMonitor != std::optional<bool>(false)) { publishNativeFilter(); return; }
    const int style = m_mode == QLatin1String("DIGL") || m_mode == QLatin1String("DIGU")
        ? m_dataFilterStyle : m_ssbFilterStyle;
    const quint64 filterRevision = m_filterRevision;
    const std::optional<NativeFilterLayout> layout = nativeFilterLayout(m_mode, style);
    const bool nativeAxesKnown = layout && m_filterLowIndex >= 0 && m_filterLowIndex < layout->low.valuesHz.size()
        && (layout->high.valuesHz.isEmpty() || (m_filterHighIndex >= 0 && m_filterHighIndex < layout->high.valuesHz.size()));
    if (m_filterBankAxesPending && !m_filterBankAxesAwaitingBank && nativeAxesKnown) {
        m_filterBankAxesPending = false;
        emit capabilitiesChanged();
        if (!guard || generation != m_generation || filterRevision != m_filterRevision) { return; }
    }
    if (const std::optional<FilterEdges> edges = decodeFilter(m_mode, style, m_filterLowIndex, m_filterHighIndex)) {
        SliceDelta delta;
        delta.filterLow = edges->lowHz;
        delta.filterHigh = edges->highHz;
        emit sliceChanged(0, delta);
    }
    if (guard && generation == m_generation) { publishNativeFilter(); }
}

QVariantMap KenwoodBackend::nativeFilterStatus() const
{
    const int style = m_mode == QLatin1String("DIGL") || m_mode == QLatin1String("DIGU")
        ? m_dataFilterStyle : m_ssbFilterStyle;
    const std::optional<NativeFilterLayout> layout = nativeFilterLayout(m_mode, style);
    QVariantList axes;
    if (layout) {
        const auto appendAxis = [&axes, this](const QString& id, const NativeFilterAxis& axis, int index) {
            if (axis.valuesHz.isEmpty()) { return; }
            QVariantList values;
            for (int hz : axis.valuesHz) { values.append(hz); }
            QVariantMap entry{{QStringLiteral("id"), id}, {QStringLiteral("label"), axis.label},
                {QStringLiteral("valuesHz"), values}};
            if (m_receiveDspMonitor == std::optional<bool>(false) && index >= 0 && index < axis.valuesHz.size()) { entry.insert(QStringLiteral("index"), index); }
            axes.append(entry);
        };
        appendAxis(QStringLiteral("low"), layout->low, m_filterLowIndex);
        appendAxis(QStringLiteral("high"), layout->high, m_filterHighIndex);
    }
    return {{QStringLiteral("statusRevision"), QString::number(m_nativeFilterStatusRevision)},
        {QStringLiteral("mode"), m_mode},
        {QStringLiteral("context"), QString::number(m_generation) + QLatin1Char(':') + QString::number(m_filterRevision)},
        {QStringLiteral("editable"), receiveModeWritable() && layout.has_value() && !m_filterBankAxesPending
            && m_receiveDspMonitor == std::optional<bool>(false)},
        {QStringLiteral("axes"), axes}};
}

void KenwoodBackend::publishNativeFilter()
{
    ++m_nativeFilterStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("filter"), nativeFilterStatus());
}

QVariantMap KenwoodBackend::networkAudioLevelStatus() const
{
    QVariantMap state{{QStringLiteral("statusRevision"), QString::number(m_networkAudioStatusRevision)},
        {QStringLiteral("writable"), isConnected() && m_identityConfirmed && m_loginAdministrator},
        {QStringLiteral("context"), QString::number(m_generation)}};
    if (m_voipInputLevel >= 0) { state.insert(QStringLiteral("inputLevel"), m_voipInputLevel); }
    if (m_voipOutputLevel >= 0) { state.insert(QStringLiteral("outputLevel"), m_voipOutputLevel); }
    return state;
}

void KenwoodBackend::publishNetworkAudioLevels()
{
    ++m_networkAudioStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("audio.levels"), networkAudioLevelStatus());
}

void KenwoodBackend::pollNetworkAudioLevels(qint64 nowMs)
{
    if (!isConnected() || !m_identityConfirmed || nowMs < m_nextVoipPollMs) { return; }
    m_nextVoipPollMs = nowMs + 5000;
    sendObserved("##KN30", "##KN31");
}

QVariantMap KenwoodBackend::transmitInputStatus() const
{
    QVariantMap state{{QStringLiteral("context"), QString::number(m_generation)},
        {QStringLiteral("editable"), isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved
            && !m_keyIntent && !m_atuIntent && !m_cwIntent}};
    if (m_audioInputSelection[0] >= 0) { state.insert(QStringLiteral("send"), m_audioInputSelection[0]); }
    if (m_audioInputSelection[1] >= 0) { state.insert(QStringLiteral("data"), m_audioInputSelection[1]); }
    return state;
}

void KenwoodBackend::publishTransmitInputs()
{
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("inputs"), transmitInputStatus());
}

void KenwoodBackend::pollTransmitInputs(qint64 nowMs)
{
    if (!isConnected() || !m_identityConfirmed || nowMs < m_nextInputPollMs) { return; }
    m_nextInputPollMs = nowMs + 5000;
    sendObserved("MS0", "MS1");
}

QVariantMap KenwoodBackend::powerLimitStatus() const
{
    QVariantList limits;
    for (auto entry = m_powerLimits.cbegin(); entry != m_powerLimits.cend(); ++entry) {
        limits.append(QVariantMap{{QStringLiteral("type"), entry.key() / 12},
            {QStringLiteral("band"), entry.key() % 12}, {QStringLiteral("watts"), entry.value()}});
    }
    QVariantMap state{{QStringLiteral("context"), QString::number(m_generation)},
        {QStringLiteral("editable"), isConnected() && m_identityConfirmed && m_txStateKnown && !m_txObserved
            && !m_keyIntent && !m_atuIntent && !m_cwIntent}, {QStringLiteral("limits"), limits}};
    if (m_powerLimiterEnabled) { state.insert(QStringLiteral("enabled"), *m_powerLimiterEnabled); }
    return state;
}

void KenwoodBackend::publishPowerLimits()
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const std::optional<int> band = tunePowerBand();
    if (band && m_powerLimits.contains(60 + *band)) {
        TransmitDelta delta;
        delta.tunePower = m_powerLimits.value(60 + *band);
        emit transmitChanged(delta);
        if (!guard || generation != m_generation) { return; }
    }
    emit capabilitiesChanged();
    if (!guard || generation != m_generation) { return; }
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("power.limits"), powerLimitStatus());
    if (guard && generation == m_generation) { publishTransmitInputs(); }
}

void KenwoodBackend::pollPowerLimits(qint64 nowMs)
{
    if (!isConnected() || !m_identityConfirmed || nowMs < m_nextPowerPollMs) { return; }
    m_nextPowerPollMs = nowMs + 5000;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const std::optional<int> band = tunePowerBand();
    if (band && (!m_powerLimitSelection || *m_powerLimitSelection != QPair<int, int>{5, *band})) {
        if (!send(*queryPowerLimit(5, *band)) || !guard || generation != m_generation) { return; }
    }
    if (m_powerLimitSelection && m_powerLimits.contains(m_powerLimitSelection->first * 12 + m_powerLimitSelection->second)) {
        const std::optional<QByteArray> query = queryPowerLimit(m_powerLimitSelection->first, m_powerLimitSelection->second);
        if (query) { sendObserved(*query, "LP2"); }
    } else { send("LP2"); }
}

std::optional<int> KenwoodBackend::tunePowerBand() const
{
    if (!isConnected() || !m_identityConfirmed || m_transmitVfo < 0 || m_vfoModes[m_transmitVfo].isEmpty()
        || !m_driveOutput || *m_driveOutput || !m_transverterEnabled || *m_transverterEnabled
        || !m_xitEnabled || (*m_xitEnabled && !m_incrementalOffsetHz)) { return std::nullopt; }
    const qint64 frequency = m_frequency[m_transmitVfo];
    const std::optional<int> band = powerLimitBandForFrequency(frequency);
    const std::optional<int> shifted = powerLimitBandForFrequency(frequency + (*m_xitEnabled ? *m_incrementalOffsetHz : 0));
    return band && shifted == band ? band : std::nullopt;
}

void KenwoodBackend::refreshTunePower()
{
    const std::optional<int> band = tunePowerBand();
    const bool changed = band != m_lastTunePowerBand;
    m_lastTunePowerBand = band;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (changed && band) {
        if (!send(*queryPowerLimit(5, *band)) || !guard || generation != m_generation) { return; }
    }
    publishPowerLimits();
}

void KenwoodBackend::queryFilter()
{
    if (m_mode.isEmpty() || m_receiveDspMonitor != std::optional<bool>(false)) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 filterRevision = m_filterRevision;
    const int style = m_mode == QLatin1String("DIGL") || m_mode == QLatin1String("DIGU")
        ? m_dataFilterStyle : m_ssbFilterStyle;
    const std::optional<NativeFilterLayout> layout = nativeFilterLayout(m_mode, style);
    if (!send("SL0") || !guard || generation != m_generation || filterRevision != m_filterRevision
        || m_receiveDspMonitor != std::optional<bool>(false)) { return; }
    if (layout && layout->high.valuesHz.isEmpty()) { return; }
    send("SH0");
}

void KenwoodBackend::requestSliceFilter(int sliceId, const SliceFilterRequest& request)
{
    if (request.origin == SliceFilterRequest::Origin::Operator) {
        setSliceFilter(sliceId, request.lowHz, request.highHz);
    }
}

void KenwoodBackend::setSliceFilter(int sliceId, int low, int high)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (sliceId != 0 || !receiveModeWritable() || m_filterBankAxesPending
        || m_receiveDspMonitor != std::optional<bool>(false)) { return; }
    const quint64 filterRevision = m_filterRevision;
    const int style = m_mode == QLatin1String("DIGL") || m_mode == QLatin1String("DIGU")
        ? m_dataFilterStyle : m_ssbFilterStyle;
    const std::optional<QList<QByteArray>> commands = encodeFilter(m_mode, style, {low, high});
    if (!commands) {
        emit configurationWarning(QStringLiteral("This TS-890S mode or filter setting is not mapped to the shared passband controls."));
        return;
    }
    for (const QByteArray& command : *commands) {
        if (!receiveModeWritable() || m_receiveDspMonitor != std::optional<bool>(false) || filterRevision != m_filterRevision
            || !send(command) || !guard || generation != m_generation) { return; }
    }
    if (receiveModeWritable() && m_receiveDspMonitor == std::optional<bool>(false)
        && filterRevision == m_filterRevision) { sendObserved("SL0", "SH0"); }
}

void KenwoodBackend::setSliceAgc(int sliceId, const QString& mode, int)
{
    const std::optional<ReceiveDspControl> controls = capabilities().receiveDspControl;
    if (sliceId != 0 || !controls || !controls->supports(ReceiveDspControl::Feature::Agc, m_mode)) { return; }
    const QStringList names{"off", "slow", "med", "fast"};
    const qsizetype index = names.indexOf(mode.toLower());
    if (index >= 0) { sendObserved("GC" + QByteArray::number(index), "GC"); }
}

ReceiveDispatch KenwoodBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    if (sliceId != 0 || !isConnected() || !request.valid()
        || (request.feature == SliceDspRequest::Feature::Nb
            && request.requestedBlanker() == NoiseBlankerKind::Advanced)) {
        return ReceiveDispatch::Unsupported;
    }
    using DspFeature = ReceiveDspControl::Feature;
    if (request.feature == SliceDspRequest::Feature::Apf) {
        if (request.field != SliceDspRequest::Field::Enabled || !capabilities().hasAudioPeakingFilter) {
            return ReceiveDispatch::Unsupported;
        }
        const QVariantMap fields{{QStringLiteral("context"), receiveControlsStatus().value(QStringLiteral("context"))},
            {QStringLiteral("id"), QStringLiteral("apf.enabled")}, {QStringLiteral("value"), request.enabled ? 2 : 1}};
        return setReceiveControl(fields) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    DspFeature feature;
    switch (request.feature) {
    case SliceDspRequest::Feature::Nr: feature = DspFeature::Nr; break;
    case SliceDspRequest::Feature::Nb: feature = DspFeature::Nb; break;
    case SliceDspRequest::Feature::Anf: feature = DspFeature::Anf; break;
    default: return ReceiveDispatch::Unsupported;
    }
    const std::optional<ReceiveDspControl> controls = capabilities().receiveDspControl;
    if (!controls || !controls->supports(feature, m_mode)) { return ReceiveDispatch::Unsupported; }
    if (request.field == SliceDspRequest::Field::Level) {
        if (!controls->supportsLevel(feature, m_mode)) { return ReceiveDispatch::Unsupported; }
        if (request.feature == SliceDspRequest::Feature::Nr && m_noiseReduction != 2) {
            return sendObserved("RL1" + QByteArray::number(1 + qRound(request.level * 9.0 / 100)).rightJustified(2, '0'), "RL1")
                ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
        }
        if (request.feature == SliceDspRequest::Feature::Nb) {
            return sendObserved("NL1" + QByteArray::number(1 + qRound(request.level * 19.0 / 100)).rightJustified(3, '0'), "NL1")
                ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
        }
        return ReceiveDispatch::Unsupported;
    }
    if (request.field != SliceDspRequest::Field::Enabled) { return ReceiveDispatch::Unsupported; }
    QByteArray prefix;
    switch (request.feature) {
    case SliceDspRequest::Feature::Nr: prefix = "NR"; break;
    case SliceDspRequest::Feature::Nb: prefix = "NB1"; break;
    case SliceDspRequest::Feature::Anf: prefix = "BC"; break;
    default: return ReceiveDispatch::Unsupported;
    }
    if (!sendObserved(prefix + (request.enabled ? '1' : '0'), prefix)) { return ReceiveDispatch::Unsupported; }
    return ReceiveDispatch::Dispatched;
}

ReceiveDispatch KenwoodBackend::requestSliceControl(int sliceId, const SliceControlRequest& request)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (sliceId != 0 || !isConnected() || !request.valid()) { return ReceiveDispatch::Unsupported; }
    if (request.field == SliceControlRequest::Field::ActiveSlice && std::get<bool>(request.value)) {
        return ReceiveDispatch::LocalOnly;
    }
    if (request.field == SliceControlRequest::Field::TxAntenna) {
        const QString antenna = std::get<QString>(request.value);
        if (antenna != QLatin1String("ANT1") && antenna != QLatin1String("ANT2")) { return ReceiveDispatch::Unsupported; }
        return sendObserved(antenna == QLatin1String("ANT1") ? "AN1999" : "AN2999", "AN")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == SliceControlRequest::Field::Rit || request.field == SliceControlRequest::Field::Xit) {
        const IncrementalTuning tuning = std::get<IncrementalTuning>(request.value);
        if (std::abs(tuning.hz) > 9999) { return ReceiveDispatch::Unsupported; }
        setSliceRitOffset(0, tuning.hz);
        if (!guard || generation != m_generation) { return ReceiveDispatch::Unsupported; }
        if (request.field == SliceControlRequest::Field::Rit) { setSliceRitEnabled(0, tuning.enabled); }
        else { setSliceXitEnabled(0, tuning.enabled); }
        return ReceiveDispatch::Dispatched;
    }
    return ReceiveDispatch::Unsupported;
}

ReceiveDispatch KenwoodBackend::requestSliceRxAntenna(int sliceId, const QString& antenna)
{
    if (sliceId != 0 || !receiveControlsWritable() || m_tune
        || m_frequency[m_receiveVfo] <= 0 || m_vfoModes[m_receiveVfo] != m_mode
        || !encodeMode(m_mode).has_value()) { return ReceiveDispatch::Unsupported; }
    QByteArray command;
    if (antenna == QLatin1String("ANT1")) { command = "AN1099"; }
    else if (antenna == QLatin1String("ANT2")) { command = "AN2099"; }
    else if (antenna == QLatin1String("RX-ANT")) { command = "AN9199"; }
    else { return ReceiveDispatch::Unsupported; }
    return sendObserved(command, "AN") ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

void KenwoodBackend::setSliceRxAntenna(int sliceId, const QString& antenna)
{
    requestSliceRxAntenna(sliceId, antenna);
}

ReceiveDispatch KenwoodBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (sliceId != 0 || !isConnected() || !request.valid()) { return ReceiveDispatch::Unsupported; }
    if (request.field == SliceAudioRequest::Field::Gain) {
        return sendObserved("AG" + QByteArray::number(qRound(request.value * 255.0 / 100)).rightJustified(3, '0'), "AG")
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.field == SliceAudioRequest::Field::Mute) {
        return sendObserved(request.value ? "MU1" : "MU0", "MU") ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    return ReceiveDispatch::Unsupported;
}

ReceiveDispatch KenwoodBackend::requestSliceSquelch(int sliceId, const SliceSquelchRequest& request)
{
    if (sliceId != 0 || !isConnected() || !request.valid() || request.kind != SliceSquelchRequest::Kind::Manual) {
        return ReceiveDispatch::Unsupported;
    }
    const int raw = request.enabled ? qRound(request.level * 255.0 / 100) : 0;
    return sendObserved("SQ" + QByteArray::number(raw).rightJustified(3, '0'), "SQ")
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

void KenwoodBackend::setSliceRitEnabled(int sliceId, bool on)
{
    if (sliceId == 0) { sendObserved(on ? "RT1" : "RT0", "RT"); }
}
void KenwoodBackend::setSliceXitEnabled(int sliceId, bool on)
{
    if (sliceId == 0) { sendObserved(on ? "XT1" : "XT0", "XT"); }
}
void KenwoodBackend::setSliceRitOffset(int sliceId, int hz)
{
    if (sliceId != 0 || hz < -9999 || hz > 9999) { return; }
    sendObserved(QByteArray(hz < 0 ? "RD" : "RU") + QByteArray::number(std::abs(hz)).rightJustified(5, '0'), "RF");
}

bool KenwoodBackend::setFixedScope(double centerHz, double widthHz)
{
    if (!ordinaryReceiveCoordinatesAvailable() || m_scopeMode != 1 || m_fixedRange < 1 || m_fixedRange > 3
        || !std::isfinite(centerHz) || !std::isfinite(widthHz) || widthHz < 5000 || widthHz > 500000
        || centerHz - widthHz / 2 < 0 || centerHz + widthHz / 2 > 99999999) { return false; }
    const qint64 low = std::llround(centerHz - widthHz / 2);
    const qint64 high = std::llround(centerHz + widthHz / 2);
    m_scopeSpanIntent.reset();
    return sendObserved("BSM" + QByteArray::number(m_fixedRange)
        + QByteArray::number(low).rightJustified(8, '0')
        + QByteArray::number(high).rightJustified(8, '0'), "BSM0");
}

void KenwoodBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    const QPointer<KenwoodBackend> guard(this);
    if (!ordinaryReceiveCoordinatesAvailable() || panId != QLatin1String(kPan) || !std::isfinite(hz)) { return; }
    if (m_scopeMode == 1) {
        if (!setFixedScope(hz, m_scopeHighHz - m_scopeLowHz) && guard && isConnected()) {
            emit configurationWarning(QStringLiteral("Select a stored Fixed scope range before moving the TS-890S scope window."));
        }
    } else if ((m_scopeMode == 0 || m_scopeMode == 2) && intent == PanCenterIntent::Drag) {
        setSliceFrequency(0, hz);
    } else { publishGeometry(); }
}

ReceiveDispatch KenwoodBackend::requestPanDisplay(const QString& panId,
    const QString& waterfallId, const PanDisplayRequest& request,
    const RadioControlCompletion& completion, const RadioControlFence& current)
{
    if (!ordinaryReceiveCoordinatesAvailable() || panId != QLatin1String(kPan)
        || !request.valid() || (current && !current())) { return ReceiveDispatch::Unsupported; }
    if (request.field == PanDisplayField::Geometry && m_scopeMode == 1) {
        const PanGeometryRequest geometry = std::get<PanGeometryRequest>(request.value);
        const double center = geometry.centerMhz ? *geometry.centerMhz * 1.0e6
            : (m_scopeHighHz + m_scopeLowHz) / 2.0;
        const double width = geometry.bandwidthMhz ? *geometry.bandwidthMhz * 1.0e6
            : m_scopeHighHz - m_scopeLowHz;
        return setFixedScope(center, width) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    return IRadioBackend::requestPanDisplay(panId, waterfallId, request, completion, current);
}

void KenwoodBackend::setPanBandwidth(const QString& panId, double hz)
{
    const QPointer<KenwoodBackend> guard(this);
    if (!ordinaryReceiveCoordinatesAvailable() || panId != QLatin1String(kPan) || !std::isfinite(hz) || hz < 5000 || hz > 500000) { return; }
    if (m_scopeMode == 1) {
        if (!setFixedScope((m_scopeHighHz + m_scopeLowHz) / 2.0, hz) && guard && isConnected()) {
            emit configurationWarning(QStringLiteral("Select a stored Fixed scope range before zooming the TS-890S scope window."));
        }
        return;
    }
    auto nearest = std::min_element(kSpans.cbegin(), kSpans.cend(), [hz](int left, int right) {
        return std::abs(left - hz) < std::abs(right - hz);
    });
    if (m_spanHz > 0 && *nearest == m_spanHz && std::abs(hz - m_spanHz) > 0.5) {
        if (hz < m_spanHz && nearest != kSpans.cbegin()) { --nearest; }
        else if (hz > m_spanHz && std::next(nearest) != kSpans.cend()) { ++nearest; }
    }
    const QString context = QStringLiteral("%1:%2:%3:%4:%5")
        .arg(scopeControlsContextKey())
        .arg(m_receiveVfo >= 0 && m_receiveVfo <= 1 ? m_frequency[m_receiveVfo] : 0)
        .arg(m_scopeMode).arg(m_scopeShift).arg(m_scopeExpand);
    ScopeSpanIntent intent{hz, m_spanHz, *nearest, *nearest == m_spanHz, context, 0};
    if (m_scopeSpanIntent && m_scopeSpanIntent->context == context
        && std::abs(m_scopeSpanIntent->requestedHz - hz) <= 0.5) {
        intent = *m_scopeSpanIntent;
    }
    // A gesture's settle request repeats the same absolute width. Retain its
    // native target across readback instead of reversing its rounding direction.
    intent.serial = ++m_scopeSpanIntentSerial;
    m_scopeSpanIntent = intent;
    const auto target = std::find(kSpans.cbegin(), kSpans.cend(), intent.targetHz);
    if (!sendObserved("BS4" + QByteArray::number(std::distance(kSpans.cbegin(), target)), "BS4")
        && guard && m_scopeSpanIntent && m_scopeSpanIntent->serial == intent.serial) {
        m_scopeSpanIntent.reset();
    }
}

void KenwoodBackend::setPanRfGain(const QString& panId, int gain)
{
    if (panId == QLatin1String(kPan) && gain >= 0 && gain <= 100) {
        sendObserved("RG" + QByteArray::number(qRound(gain * 255.0 / 100)).rightJustified(3, '0'), "RG");
    }
}
void KenwoodBackend::setPanPreamp(const QString& panId, int step)
{
    if (panId == QLatin1String(kPan) && step >= 0 && step <= 2) { sendObserved("PA" + QByteArray::number(step), "PA"); }
}
void KenwoodBackend::setPanAttenuator(const QString& panId, int step)
{
    if (panId == QLatin1String(kPan) && step >= 0 && step <= 3) { sendObserved("RA" + QByteArray::number(step), "RA"); }
}
void KenwoodBackend::setPanScopeMode(const QString& panId, int index)
{
    if (panId == QLatin1String(kPan) && index >= 0 && index <= 2) {
        m_scopeSpanIntent.reset();
        sendObserved("BS3" + QByteArray::number(index), "BS3");
    }
}

void KenwoodBackend::invokeExtension(const QString& ns, const QString& verb, quint64 requestId, const QVariant& argument)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    if (ns == QLatin1String("kenwood") && handleRelativeTuningExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleTuningExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleFmWidthExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleFilterBankExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleReceiveEqExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleReceiveAgcExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleReceiveRoutingExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleScopeControlsExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handlePskReceiveExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleReceiverOptionsExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleScanPreferencesExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleVfoScanExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleTransverterExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleCwReceiveExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleOperatingDisplayExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && handleAutoModeExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && verb.startsWith(QLatin1String("memory.read."))) {
        emit extensionError(requestId, QStringLiteral("Native memory support is disabled in this TS-890S build."));
        return;
    }
    if (ns == QLatin1String("kenwood") && handleRadioMessagesExtension(verb, requestId, argument)) { return; }
    if (ns == QLatin1String("kenwood") && verb == QLatin1String("receive.controls.get")) {
        m_receiveControlsInterested = true;
        const bool changed = refreshReceiveControls(m_controlIngressSequence);
        if (!guard || generation != m_generation) { return; }
        if (!changed) { queryReceiveControls(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, receiveControlsStatus()); }
        return;
    }
    if (ns == QLatin1String("kenwood") && (verb == QLatin1String("receive.controls.set")
        || verb == QLatin1String("receive.controls.reset"))) {
        if (setReceiveControl(argument.toMap(), verb == QLatin1String("receive.controls.reset"))) {
            if (guard && generation == m_generation) { emit extensionResult(requestId, true); }
        } else if (guard && generation == m_generation) {
            emit extensionError(requestId, QStringLiteral("Receive control requires current radio readback and a legal receive context."));
        }
        return;
    }
    if (ns == QLatin1String("kenwood") && isConnected()) {
        if (verb == QLatin1String("inputs.get")) {
            emit extensionResult(requestId, transmitInputStatus());
            return;
        }
        if (verb == QLatin1String("inputs.set")) {
            const QVariantMap fields = argument.toMap();
            const QVariantMap state = transmitInputStatus();
            const QString path = fields.value(QStringLiteral("path")).toString();
            const std::optional<int> selection = boundedInteger(fields.value(QStringLiteral("selection")), 0, 6);
            const int index = path == QLatin1String("send") ? 0 : path == QLatin1String("data") ? 1 : -1;
            if (index >= 0 && selection && m_audioInputSelection[index] >= 0 && state.value(QStringLiteral("editable")).toBool()
                && fields.value(QStringLiteral("context")).toString() == state.value(QStringLiteral("context")).toString()) {
                const QByteArray command = *encodeTransmitAudioInput({index, *selection});
                const QByteArray query = "MS" + QByteArray::number(index);
                m_audioInputSelection[index] = -1;
                m_audioInputPending[index] = true;
                if (index == 1) {
                    clearTxAudio();
                    m_dataSourceKnown = m_dataLan = false;
                }
                const bool sent = sendObserved(command, query);
                if (!guard || generation != m_generation) { return; }
                publishTransmitInputs();
                if (!guard || generation != m_generation) { return; }
                if (sent) { emit extensionResult(requestId, true); }
                else { emit extensionError(requestId, QStringLiteral("TS-890S transmit input request could not be sent.")); }
                return;
            }
        }
        if (verb == QLatin1String("power.limits.get")) {
            emit extensionResult(requestId, powerLimitStatus());
            return;
        }
        if (verb == QLatin1String("power.limits.query") || verb == QLatin1String("power.limits.set")
            || verb == QLatin1String("power.limiter.set")) {
            const QVariantMap fields = argument.toMap();
            const QVariantMap state = powerLimitStatus();
            const bool current = fields.value(QStringLiteral("context")).toString() == state.value(QStringLiteral("context")).toString();
            const std::optional<int> type = boundedInteger(fields.value(QStringLiteral("type")), 0, 5);
            const std::optional<int> band = boundedInteger(fields.value(QStringLiteral("band")), 0, 11);
            if (verb == QLatin1String("power.limits.query") && current && type && band) {
                m_powerLimitSelection = QPair<int, int>{*type, *band};
                if (!m_identityConfirmed) { emit extensionResult(requestId, powerLimitStatus()); return; }
                const QByteArray query = *queryPowerLimit(*type, *band);
                if (sendObserved(query, "LP2") && guard && generation == m_generation) {
                    emit extensionResult(requestId, powerLimitStatus());
                } else if (guard && generation == m_generation) {
                    emit extensionError(requestId, QStringLiteral("TS-890S power limit query could not be sent."));
                }
                return;
            }
            const std::optional<int> watts = boundedInteger(fields.value(QStringLiteral("watts")), 5, 100);
            if (verb == QLatin1String("power.limits.set") && current && state.value(QStringLiteral("editable")).toBool()
                && type && band && watts && m_powerLimits.contains(*type * 12 + *band)) {
                const QByteArray query = *queryPowerLimit(*type, *band);
                const QByteArray command = *encodePowerLimit({*type, *band, *watts});
                if (sendObserved(command, query) && guard && generation == m_generation) { emit extensionResult(requestId, true); }
                else if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("TS-890S power limit request could not be sent.")); }
                return;
            }
            const QVariant enabled = fields.value(QStringLiteral("enabled"));
            if (verb == QLatin1String("power.limiter.set") && current && state.value(QStringLiteral("editable")).toBool()
                && m_powerLimiterEnabled && enabled.typeId() == QMetaType::Bool) {
                if (sendObserved(enabled.toBool() ? "LP21" : "LP20", "LP2") && guard && generation == m_generation) {
                    emit extensionResult(requestId, true);
                } else if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("TS-890S limiter request could not be sent.")); }
                return;
            }
        }
        if (verb == QLatin1String("audio.levels.get")) {
            emit extensionResult(requestId, networkAudioLevelStatus());
            return;
        }
        if (verb == QLatin1String("audio.levels.set")) {
            const QVariantMap fields = argument.toMap();
            const QVariantMap state = networkAudioLevelStatus();
            const QString direction = fields.value(QStringLiteral("direction")).toString();
            const QVariant value = fields.value(QStringLiteral("level"));
            const int type = value.typeId();
            const bool numeric = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
                || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
            bool converted = false;
            const double level = value.toDouble(&converted);
            const QString key = direction == QLatin1String("input") ? QStringLiteral("inputLevel") : QStringLiteral("outputLevel");
            if ((direction == QLatin1String("input") || direction == QLatin1String("output"))
                && state.value(QStringLiteral("writable")).toBool() && state.contains(key)
                && fields.value(QStringLiteral("context")).toString() == state.value(QStringLiteral("context")).toString()
                && numeric && converted && std::isfinite(level) && std::trunc(level) == level && level >= 0 && level <= 100) {
                const QByteArray query = direction == QLatin1String("input") ? "##KN30" : "##KN31";
                const QByteArray command = query + QByteArray::number(static_cast<int>(level)).rightJustified(3, '0');
                if (sendObserved(command, query) && guard && generation == m_generation) {
                    emit extensionResult(requestId, true);
                } else if (guard && generation == m_generation) {
                    emit extensionError(requestId, QStringLiteral("TS-890S network audio level request could not be sent."));
                }
                return;
            }
        }
        if (verb == QLatin1String("filter.get")) {
            emit extensionResult(requestId, nativeFilterStatus());
            return;
        }
        if (verb == QLatin1String("filter.set")) {
            const QVariantMap fields = argument.toMap();
            const QVariantMap status = nativeFilterStatus();
            bool valid = false;
            const double requested = fields.value(QStringLiteral("index")).toDouble(&valid);
            const QString axisId = fields.value(QStringLiteral("axis")).toString();
            if (status.value(QStringLiteral("editable")).toBool()
                && fields.value(QStringLiteral("context")).toString() == status.value(QStringLiteral("context")).toString()
                && valid && std::isfinite(requested) && requested >= 0 && requested <= 180
                && std::trunc(requested) == requested) {
                for (const QVariant& value : status.value(QStringLiteral("axes")).toList()) {
                    const QVariantMap axis = value.toMap();
                    if (axis.value(QStringLiteral("id")).toString() != axisId
                        || !axis.contains(QStringLiteral("index"))
                        || requested >= axis.value(QStringLiteral("valuesHz")).toList().size()) { continue; }
                    const QByteArray prefix = axisId == QLatin1String("low") ? "SL0" : "SH0";
                    const QByteArray command = prefix + QByteArray::number(static_cast<int>(requested))
                        .rightJustified(axisId == QLatin1String("low") ? 2 : 3, '0');
                    if (sendObserved(command, prefix) && guard && generation == m_generation) {
                        emit extensionResult(requestId, true);
                    } else if (guard && generation == m_generation) {
                        emit extensionError(requestId, QStringLiteral("TS-890S filter request could not be sent."));
                    }
                    return;
                }
            }
        }
        if (verb == QLatin1String("vfo.get")) {
            emit extensionResult(requestId, vfoStatus());
            return;
        }
        if (verb == QLatin1String("meters.raw")) {
            QVariantMap readings;
            for (auto entry = m_meterReadings.cbegin(); entry != m_meterReadings.cend(); ++entry) {
                readings.insert(entry.key(), entry.value());
            }
            emit extensionResult(requestId, QVariantMap{{QStringLiteral("unit"), QStringLiteral("dot")},
                                                       {QStringLiteral("readings"), readings}});
            return;
        }
        if (verb == QLatin1String("vfo.receive") || verb == QLatin1String("vfo.transmit")) {
            const QString vfo = argument.toString();
            const bool transmitAllowed = verb != QLatin1String("vfo.transmit")
                || (m_receiveVfo != kMemoryVfo && m_transmitVfo != kMemoryVfo);
            if ((vfo == QLatin1String("A") || vfo == QLatin1String("B")) && !m_txObserved && transmitAllowed) {
                const QByteArray prefix = verb == QLatin1String("vfo.receive") ? "FR" : "FT";
                if (sendObserved(prefix + (vfo == QLatin1String("A") ? '0' : '1'), prefix)) {
                    if (!guard || generation != m_generation) { return; }
                    emit extensionResult(requestId, true);
                    return;
                }
            }
        }
        if (verb == QLatin1String("vfo.frequency") && ordinaryReceiveCoordinatesAvailable() && !m_txObserved) {
            const QVariantMap fields = argument.toMap();
            const QString vfo = fields.value(QStringLiteral("vfo")).toString();
            bool valid = false;
            const double hz = fields.value(QStringLiteral("frequencyHz")).toDouble(&valid);
            if ((vfo == QLatin1String("A") || vfo == QLatin1String("B")) && valid && std::isfinite(hz)
                && hz >= kMinimumFrequencyHz && hz <= m_maximumFrequencyHz && std::trunc(hz) == hz) {
                const std::optional<QByteArray> command = encodeFrequency(vfo == QLatin1String("A") ? 'A' : 'B', static_cast<qint64>(hz));
                if (command && sendObserved(*command, vfo == QLatin1String("A") ? "FA" : "FB")) {
                    if (!guard || generation != m_generation) { return; }
                    emit extensionResult(requestId, true);
                    return;
                }
            }
        }
    }
    if (guard && generation == m_generation) {
        emit extensionError(requestId, QStringLiteral("Unsupported TS-890S extension."));
    }
}
} // namespace AetherSDR::kenwood
