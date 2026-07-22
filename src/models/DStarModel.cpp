#include "DStarModel.h"

#include "core/DigitalVoiceWaveformProcess.h"
#include "core/DigitalVoiceWaveformSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif

#include <algorithm>
#include <utility>

namespace AetherSDR {

namespace {

QString normalizedCallsign(const QString& value, int maximumLength)
{
    return DigitalVoiceWaveformSettings::normalizeCallsign(value, maximumLength);
}

int serialPathScore(const QString& path)
{
    const QString lower = path.toLower();
    int score = 0;
    if (lower.contains(QStringLiteral("thumbdv"))
        || lower.contains(QStringLiteral("dv3000"))) {
        score += 100;
    }
    if (lower.contains(QStringLiteral("usbserial"))
        || lower.contains(QStringLiteral("usb-serial"))
        || lower.contains(QStringLiteral("ftdi"))) {
        score += 40;
    }
    if (lower.contains(QStringLiteral("usbmodem"))
        || lower.contains(QStringLiteral("ttyusb"))
        || lower.contains(QStringLiteral("ttyacm"))
        || lower.contains(QStringLiteral("wchusbserial"))
        || lower.contains(QStringLiteral("slab_usbtouart"))) {
        score += 20;
    }
    return score;
}

QString serialIdentity(const QString& path)
{
    const QString trimmedPath = path.trimmed();
    QString comPath = trimmedPath;
    if (comPath.startsWith(QStringLiteral("\\\\.\\"))) {
        comPath = comPath.mid(4);
    }
    if (comPath.size() > 3
        && comPath.left(3).compare(QStringLiteral("COM"), Qt::CaseInsensitive) == 0) {
        bool numericSuffix = true;
        for (const QChar ch : comPath.mid(3)) {
            if (!ch.isDigit()) {
                numericSuffix = false;
                break;
            }
        }
        if (numericSuffix) {
            return comPath.toUpper();
        }
    }
    const QString canonical = QFileInfo(trimmedPath).canonicalFilePath();
    return canonical.isEmpty() ? trimmedPath : canonical;
}

bool sameSerialDevice(const QString& lhs, const QString& rhs)
{
    if (lhs.trimmed().isEmpty() || rhs.trimmed().isEmpty()) {
        return false;
    }
    if (lhs == rhs) {
        return true;
    }
    const QString lhsIdentity = serialIdentity(lhs);
    const QString rhsIdentity = serialIdentity(rhs);
    return !lhsIdentity.isEmpty() && lhsIdentity == rhsIdentity;
}

void addSerialDevice(QList<DStarSerialDevice>& devices,
                     QSet<QString>& seen,
                     DStarSerialDevice device)
{
    device.path = device.path.trimmed();
    const QString identity = serialIdentity(device.path);
    if (device.path.isEmpty() || seen.contains(identity)) {
        return;
    }
    seen.insert(identity);
    if (device.label.trimmed().isEmpty()) {
        device.label = device.path;
    }
    devices.append(std::move(device));
}

QString serialProbeError(const QByteArray& output)
{
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.startsWith("AETHER_DV_ERROR ")) {
            return QString::fromLocal8Bit(
                line.mid(QByteArrayLiteral("AETHER_DV_ERROR ").size()));
        }
    }
    return {};
}

QString directionName(DStarTrafficDirection direction)
{
    switch (direction) {
    case DStarTrafficDirection::Receive:  return QStringLiteral("rx");
    case DStarTrafficDirection::Transmit: return QStringLiteral("tx");
    case DStarTrafficDirection::System:   return QStringLiteral("system");
    }
    return QStringLiteral("rx");
}

DStarTrafficDirection directionFromName(const QString& value)
{
    if (value == QLatin1String("tx")) {
        return DStarTrafficDirection::Transmit;
    }
    if (value == QLatin1String("system")) {
        return DStarTrafficDirection::System;
    }
    return DStarTrafficDirection::Receive;
}

QString normalizedSimpleCallsign(const QString& value)
{
    return value.trimmed().toUpper();
}

bool isSimpleCallsign(const QString& value, int maximumLength)
{
    const QString normalized = normalizedSimpleCallsign(value);
    if (normalized.size() < 3 || normalized.size() > maximumLength) {
        return false;
    }
    return std::all_of(normalized.cbegin(), normalized.cend(), [](QChar ch) {
        return (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'));
    });
}

bool isRepeaterModule(QChar module)
{
    const QChar upper = module.toUpper();
    return upper >= QLatin1Char('A') && upper <= QLatin1Char('D');
}

QString repeaterField(const QString& callsign, QChar module)
{
    return normalizedSimpleCallsign(callsign).leftJustified(
        7, QLatin1Char(' '), true) + module.toUpper();
}

QString repeaterAreaUrCall(const QString& callsign, QChar module)
{
    return QLatin1Char('/')
        + normalizedSimpleCallsign(callsign).leftJustified(
            6, QLatin1Char(' '), true)
        + module.toUpper();
}

bool splitRepeaterField(const QString& value, QString* callsign, QChar* module)
{
    if (value.size() != 8 || !isRepeaterModule(value.back())) {
        return false;
    }
    const QString base = value.left(7).trimmed();
    if (!isSimpleCallsign(base, 7)) {
        return false;
    }
    if (callsign) {
        *callsign = base;
    }
    if (module) {
        *module = value.back();
    }
    return true;
}

bool splitRepeaterAreaUrCall(const QString& value,
                             QString* callsign,
                             QChar* module)
{
    if (value.size() != 8 || !value.startsWith(QLatin1Char('/'))
        || !isRepeaterModule(value.back())) {
        return false;
    }
    const QString base = value.mid(1, 6).trimmed();
    if (!isSimpleCallsign(base, 6)) {
        return false;
    }
    if (callsign) {
        *callsign = base;
    }
    if (module) {
        *module = value.back();
    }
    return true;
}

} // namespace

DStarModel::DStarModel(QObject* parent, bool autoSelectSerial)
    : QObject(parent)
    , m_autoSelectSerial(autoSelectSerial)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1500);
    connect(&m_saveTimer, &QTimer::timeout, this, &DStarModel::saveTraffic);

    DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    connect(&process, &DigitalVoiceWaveformProcess::stateChanged,
            this, &DStarModel::serviceChanged);
    connect(&process, &DigitalVoiceWaveformProcess::statusTextChanged,
            this, &DStarModel::serviceChanged);
    connect(&process, &DigitalVoiceWaveformProcess::healthChanged,
            this, &DStarModel::serviceChanged);
    connect(&process, &DigitalVoiceWaveformProcess::metricsChanged,
            this, &DStarModel::serviceChanged);
    connect(&DigitalVoiceModeRegistry::instance(),
            &DigitalVoiceModeRegistry::activeSliceChanged,
            this,
            [this](int) { emit serviceChanged(); },
            Qt::QueuedConnection);

    m_serialProbe.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_serialProbe,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_activeSerialProbePath.isEmpty()) {
            return;
        }
        const QByteArray output = m_serialProbe.readAll();
        const bool verified = exitStatus == QProcess::NormalExit
            && exitCode == 0
            && output.contains("AETHER_DV_PROBE verified");
        QString detail = m_serialProbeForcedError;
        if (detail.isEmpty()) {
            detail = serialProbeError(output);
        }
        if (detail.isEmpty() && !verified) {
            detail = tr("The device did not return a valid DV3000 response.");
        }
        finishSerialProbe(verified, detail);
    });
    connect(&m_serialProbe, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_activeSerialProbePath.isEmpty()) {
            finishSerialProbe(false,
                tr("Could not start the ThumbDV verification helper: %1")
                    .arg(m_serialProbe.errorString()));
        }
    });
    connect(&process, &DigitalVoiceWaveformProcess::stateChanged, this,
            [this](DigitalVoiceWaveformProcess::State) {
        if (!DigitalVoiceWaveformProcess::instance().registrationVerified()) {
            return;
        }
        const QString configured = DigitalVoiceWaveformSettings::serialPort();
        const int index = serialDeviceIndex(configured);
        if (index >= 0
            && m_serialDevices[index].verification
                != DStarSerialDevice::Verification::Verified) {
            m_serialDevices[index].verification = DStarSerialDevice::Verification::Verified;
            m_serialDevices[index].detail = tr("Verified by the running D-STAR service.");
            emit serialDevicesChanged();
        }
    });

    refreshSerialDevices();
}

DStarModel::~DStarModel()
{
    cancelSerialProbe();
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveTraffic();
    }
}

DStarConfiguration DStarModel::configuration(const QString& fallbackMyCall) const
{
    DStarConfiguration config;
    config.autoStart = DigitalVoiceWaveformSettings::autoStart();
    config.executablePath = DigitalVoiceWaveformSettings::executablePath();
    config.serialPort = DigitalVoiceWaveformSettings::serialPort();
    config.myCall = DigitalVoiceWaveformSettings::effectiveMyCall(fallbackMyCall);
    config.myCallSuffix = DigitalVoiceWaveformSettings::myCallSuffix();
    config.urCall = DigitalVoiceWaveformSettings::urCall();
    config.rpt1 = DigitalVoiceWaveformSettings::rpt1();
    config.rpt2 = DigitalVoiceWaveformSettings::rpt2();
    config.message = DigitalVoiceWaveformSettings::message();
    return config;
}

QString DStarModel::runtimeSetCommand(const DStarConfiguration& config)
{
    auto encode = [](QString value) {
        value.replace(QLatin1Char(' '), QChar(0x7f));
        return value;
    };
    return QStringLiteral(
        "set destination_rptr=%1 departure_rptr=%2 companion_call=%3 "
        "own_call1=%4 own_call2=%5 message=%6")
        .arg(encode(config.rpt2),
             encode(config.rpt1),
             encode(config.urCall),
             encode(config.myCall),
             encode(config.myCallSuffix),
             encode(config.message));
}

bool DStarModel::resolveRoute(const DStarRouteRequest& requested,
                              DStarResolvedRoute* route,
                              QString* error)
{
    if (!route) {
        return false;
    }
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    DStarRouteRequest request = requested;
    request.accessRepeaterCallsign =
        normalizedSimpleCallsign(request.accessRepeaterCallsign);
    request.destinationCallsign =
        normalizedSimpleCallsign(request.destinationCallsign);
    request.accessRepeaterModule = request.accessRepeaterModule.toUpper();
    request.destinationRepeaterModule =
        request.destinationRepeaterModule.toUpper();

    if (request.destination == DStarRouteDestination::Custom) {
        DStarResolvedRoute custom {
            request.customUrCall.trimmed().toUpper(),
            request.customRpt1.trimmed().toUpper(),
            request.customRpt2.trimmed().toUpper()
        };
        if (!DigitalVoiceWaveformSettings::isValidUrCall(custom.urCall)
            || !DigitalVoiceWaveformSettings::isValidRoutingField(custom.rpt1)
            || !DigitalVoiceWaveformSettings::isValidRoutingField(custom.rpt2)) {
            return fail(QObject::tr(
                "Enter valid URCALL, RPT1, and RPT2 values under Advanced"));
        }
        *route = custom;
        return true;
    }

    if (request.origin == DStarRouteOrigin::Direct) {
        if (request.destination == DStarRouteDestination::RepeaterArea) {
            return fail(QObject::tr(
                "Choose Repeater under From before selecting another repeater area"));
        }
        route->rpt1 = QStringLiteral("DIRECT");
        route->rpt2 = QStringLiteral("DIRECT");
    } else {
        if (!isSimpleCallsign(request.accessRepeaterCallsign, 7)) {
            return fail(QObject::tr(
                "Enter the access repeater callsign without its module letter"));
        }
        if (!isRepeaterModule(request.accessRepeaterModule)) {
            return fail(QObject::tr("Choose access repeater module A, B, C, or D"));
        }
        route->rpt1 = repeaterField(request.accessRepeaterCallsign,
                                    request.accessRepeaterModule);
        // JARL D-STAR STD6 sections 2.2/2.4: local repeater calls use the
        // access repeater for both relay fields; routed calls put that
        // repeater's G module in the destination relay field.
        route->rpt2 = request.destination == DStarRouteDestination::LocalCq
            ? route->rpt1
            : repeaterField(request.accessRepeaterCallsign, QLatin1Char('G'));
    }

    switch (request.destination) {
    case DStarRouteDestination::LocalCq:
        route->urCall = QStringLiteral("CQCQCQ");
        break;
    case DStarRouteDestination::Station:
        if (!isSimpleCallsign(request.destinationCallsign, 8)) {
            return fail(QObject::tr("Enter a valid destination station callsign"));
        }
        route->urCall = request.destinationCallsign;
        break;
    case DStarRouteDestination::RepeaterArea:
        if (!isSimpleCallsign(request.destinationCallsign, 6)) {
            return fail(QObject::tr(
                "Enter the destination repeater callsign (up to 6 characters) without its module"));
        }
        if (!isRepeaterModule(request.destinationRepeaterModule)) {
            return fail(QObject::tr(
                "Choose destination repeater module A, B, C, or D"));
        }
        route->urCall = repeaterAreaUrCall(
            request.destinationCallsign, request.destinationRepeaterModule);
        break;
    case DStarRouteDestination::Custom:
        break;
    }
    if (error) {
        error->clear();
    }
    return true;
}

DStarRouteRequest DStarModel::routeRequestForConfiguration(
    const DStarConfiguration& config)
{
    DStarRouteRequest request;
    request.customUrCall = config.urCall;
    request.customRpt1 = config.rpt1;
    request.customRpt2 = config.rpt2;

    const bool direct = config.rpt1.compare(QStringLiteral("DIRECT"),
                                            Qt::CaseInsensitive) == 0
        && config.rpt2.compare(QStringLiteral("DIRECT"),
                               Qt::CaseInsensitive) == 0;
    if (direct) {
        request.origin = DStarRouteOrigin::Direct;
        if (config.urCall.compare(QStringLiteral("CQCQCQ"),
                                  Qt::CaseInsensitive) == 0) {
            request.destination = DStarRouteDestination::LocalCq;
        } else if (!config.urCall.startsWith(QLatin1Char('/'))) {
            request.destination = DStarRouteDestination::Station;
            request.destinationCallsign = config.urCall;
        } else {
            request.destination = DStarRouteDestination::Custom;
        }
        return request;
    }

    QString accessCallsign;
    QChar accessModule;
    if (!splitRepeaterField(config.rpt1, &accessCallsign, &accessModule)) {
        request.destination = DStarRouteDestination::Custom;
        return request;
    }
    request.origin = DStarRouteOrigin::Repeater;
    request.accessRepeaterCallsign = accessCallsign;
    request.accessRepeaterModule = accessModule;

    if (config.urCall.compare(QStringLiteral("CQCQCQ"),
                              Qt::CaseInsensitive) == 0
        && config.rpt2 == config.rpt1) {
        request.destination = DStarRouteDestination::LocalCq;
        return request;
    }

    const QString gateway = repeaterField(accessCallsign, QLatin1Char('G'));
    if (config.rpt2 != gateway) {
        request.destination = DStarRouteDestination::Custom;
        return request;
    }
    if (config.urCall.startsWith(QLatin1Char('/'))) {
        QString destinationCallsign;
        QChar destinationModule;
        if (!splitRepeaterAreaUrCall(config.urCall,
                                     &destinationCallsign,
                                     &destinationModule)) {
            request.destination = DStarRouteDestination::Custom;
            return request;
        }
        request.destination = DStarRouteDestination::RepeaterArea;
        request.destinationCallsign = destinationCallsign;
        request.destinationRepeaterModule = destinationModule;
        return request;
    }

    request.destination = DStarRouteDestination::Station;
    request.destinationCallsign = config.urCall;
    return request;
}

QString DStarModel::configurationError(const DStarConfiguration& config,
                                       const QString& fallbackMyCall) const
{
    const QString myCall = config.myCall.trimmed().isEmpty()
        ? normalizedCallsign(fallbackMyCall, 8)
        : normalizedCallsign(config.myCall, 8);
    if (!DigitalVoiceWaveformSettings::isValidMyCall(myCall)) {
        return tr("Configure a valid D-STAR MYCALL (3-8 letters and digits)");
    }
    if (!DigitalVoiceWaveformSettings::isValidSuffix(config.myCallSuffix)) {
        return tr("D-STAR MYCALL suffix must contain at most 4 letters or digits");
    }
    if (!DigitalVoiceWaveformSettings::isValidUrCall(config.urCall)) {
        return tr("D-STAR URCALL must contain 1-8 letters, digits, or spaces, with an optional leading slash");
    }
    if (!DigitalVoiceWaveformSettings::isValidRoutingField(config.rpt1)
        || !DigitalVoiceWaveformSettings::isValidRoutingField(config.rpt2)) {
        return tr("D-STAR repeater fields must contain 1-8 letters, digits, or spaces");
    }
    if (!DigitalVoiceWaveformSettings::isValidMessage(config.message)) {
        return tr("D-STAR message must contain at most 20 printable ASCII characters");
    }
    return {};
}

bool DStarModel::setConfiguration(const DStarConfiguration& requested,
                                  const QString& fallbackMyCall,
                                  QString* error)
{
    DStarConfiguration config = requested;
    config.executablePath = config.executablePath.trimmed();
    config.serialPort = config.serialPort.trimmed();
    config.myCall = normalizedCallsign(config.myCall, 8);
    if (config.myCall.isEmpty()) {
        config.myCall = normalizedCallsign(fallbackMyCall, 8);
    }
    config.myCallSuffix = normalizedCallsign(config.myCallSuffix, 4);
    config.urCall = config.urCall.trimmed().toUpper();
    config.rpt1 = config.rpt1.trimmed().toUpper();
    config.rpt2 = config.rpt2.trimmed().toUpper();

    const QString validation = configurationError(config, fallbackMyCall);
    if (!validation.isEmpty()) {
        if (error) {
            *error = validation;
        }
        return false;
    }

    const DStarConfiguration existing = configuration(fallbackMyCall);
    if (existing == config) {
        return true;
    }
    const bool serialChanged = existing.serialPort != config.serialPort;

    DigitalVoiceWaveformSettings::setConfiguration(
        config.autoStart,
        config.executablePath,
        config.serialPort,
        config.myCall,
        config.myCallSuffix,
        config.urCall,
        config.rpt1,
        config.rpt2,
        config.message);
    emit configurationChanged();
    if (serialChanged) {
        refreshSerialDevices();
    }
    return true;
}

DStarSerialDevice DStarModel::classifySerialDevice(
    const DStarSerialPortMetadata& metadata,
    const QString& configuredPort)
{
    const QString path = metadata.path.trimmed();
    QStringList detailParts;
    if (!metadata.description.trimmed().isEmpty()) {
        detailParts.append(metadata.description.trimmed());
    }
    if (!metadata.manufacturer.trimmed().isEmpty()
        && !detailParts.contains(metadata.manufacturer.trimmed())) {
        detailParts.append(metadata.manufacturer.trimmed());
    }
    const QString details = detailParts.join(QStringLiteral(", "));
    const bool namedThumbDv = path.contains(QStringLiteral("thumbdv"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("dv3000"), Qt::CaseInsensitive)
        || details.contains(QStringLiteral("thumbdv"), Qt::CaseInsensitive)
        || details.contains(QStringLiteral("dv3000"), Qt::CaseInsensitive);
    const bool thumbDvUsbBridge = metadata.vendorIdentifier == 0x0403
        && metadata.productIdentifier == 0x6015;

    int score = serialPathScore(path);
    if (namedThumbDv) {
        score += 100;
    }
    if (details.contains(QStringLiteral("ftdi"), Qt::CaseInsensitive)) {
        score += 40;
    }
    if (metadata.vendorIdentifier == 0x0403) {
        score += 40;
    }
    if (thumbDvUsbBridge) {
        score += 80;
    }
    if (metadata.stablePath) {
        score += 30;
    }

    QString label;
    if (namedThumbDv) {
        label = metadata.serialNumber.trimmed().isEmpty()
            ? QStringLiteral("ThumbDV / DV3000")
            : QStringLiteral("ThumbDV (%1)").arg(metadata.serialNumber.trimmed());
    } else if (thumbDvUsbBridge) {
        label = metadata.serialNumber.trimmed().isEmpty()
            ? QStringLiteral("ThumbDV-compatible serial device")
            : QStringLiteral("ThumbDV-compatible device (%1)")
                  .arg(metadata.serialNumber.trimmed());
    } else if (!details.isEmpty()) {
        label = details;
    } else {
        label = path;
    }

    DStarSerialDevice device;
    device.path = path;
    device.label = label;
    device.score = score;
    device.highConfidence = namedThumbDv || thumbDvUsbBridge;
    device.present = metadata.present;
    if (!metadata.present) {
        device.verification = DStarSerialDevice::Verification::Unavailable;
        device.detail = QStringLiteral("The configured serial device is not connected.");
    } else if (sameSerialDevice(path, configuredPort)) {
        device.detail = QStringLiteral("Configured device; DV3000 verification pending.");
    } else if (device.highConfidence) {
        device.detail = QStringLiteral("USB identity matches a ThumbDV-compatible device; verification pending.");
    } else {
        device.detail = QStringLiteral("Possible serial device; select it to verify that it is a DV3000.");
    }
    return device;
}

bool DStarModel::serialPathsEquivalent(const QString& lhs, const QString& rhs)
{
    return sameSerialDevice(lhs, rhs);
}

QString DStarModel::autoSelectedSerialPath(
    const QList<DStarSerialDevice>& devices,
    const QString& configuredPort)
{
    QString verifiedPath;
    int verifiedCount = 0;
    for (const DStarSerialDevice& device : devices) {
        if (device.verification != DStarSerialDevice::Verification::Verified) {
            continue;
        }
        verifiedPath = device.path.trimmed();
        ++verifiedCount;
    }

    if (verifiedCount != 1 || verifiedPath.isEmpty()) {
        return configuredPort.trimmed();
    }
    return verifiedPath;
}

void DStarModel::refreshSerialDevices()
{
    cancelSerialProbe();

    QList<DStarSerialDevice> devices;
    QSet<QString> seen;
    const QString configuredPort = DigitalVoiceWaveformSettings::serialPort().trimmed();

    QHash<QString, QString> stablePathByIdentity;
#if defined(Q_OS_LINUX)
    QDir byIdDirectory(QStringLiteral("/dev/serial/by-id"));
    const QFileInfoList byIdEntries = byIdDirectory.entryInfoList(
        QDir::System | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : byIdEntries) {
        stablePathByIdentity.insert(serialIdentity(info.absoluteFilePath()),
                                    info.absoluteFilePath());
    }
#endif

    auto appendMetadata = [&](DStarSerialPortMetadata metadata) {
        const QString stablePath = stablePathByIdentity.value(
            serialIdentity(metadata.path));
        if (!stablePath.isEmpty()) {
            metadata.path = stablePath;
            metadata.stablePath = true;
        }
        DStarSerialDevice device = classifySerialDevice(metadata, configuredPort);
        if (device.score <= 0 && !sameSerialDevice(device.path, configuredPort)) {
            return;
        }
        addSerialDevice(devices, seen, std::move(device));
    };

#ifdef HAVE_SERIALPORT
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        QString path = info.systemLocation();
        if (path.isEmpty()) {
            path = info.portName();
        }
#if defined(Q_OS_MAC)
        if (path.startsWith(QStringLiteral("/dev/tty."))) {
            continue;
        }
#endif
        DStarSerialPortMetadata metadata;
        metadata.path = path;
        metadata.description = info.description();
        metadata.manufacturer = info.manufacturer();
        metadata.serialNumber = info.serialNumber();
        if (info.hasVendorIdentifier()) {
            metadata.vendorIdentifier = info.vendorIdentifier();
        }
        if (info.hasProductIdentifier()) {
            metadata.productIdentifier = info.productIdentifier();
        }
        appendMetadata(std::move(metadata));
    }
#endif

#if defined(Q_OS_MAC)
    QDir dev(QStringLiteral("/dev"));
    const QStringList patterns {
        QStringLiteral("cu.usbserial*"),
        QStringLiteral("cu.usbmodem*"),
        QStringLiteral("cu.SLAB_USBtoUART*"),
        QStringLiteral("cu.wchusbserial*")
    };
    for (const QString& pattern : patterns) {
        for (const QFileInfo& info : dev.entryInfoList(
                 {pattern}, QDir::System | QDir::Files)) {
            DStarSerialPortMetadata metadata;
            metadata.path = info.absoluteFilePath();
            appendMetadata(std::move(metadata));
        }
    }
#elif defined(Q_OS_LINUX)
    for (const QFileInfo& info : byIdEntries) {
        DStarSerialPortMetadata metadata;
        metadata.path = info.absoluteFilePath();
        metadata.stablePath = true;
        appendMetadata(std::move(metadata));
    }
    QDir dev(QStringLiteral("/dev"));
    for (const QString& pattern
         : {QStringLiteral("ttyUSB*"), QStringLiteral("ttyACM*")}) {
        for (const QFileInfo& info : dev.entryInfoList(
                 {pattern}, QDir::System | QDir::Files)) {
            DStarSerialPortMetadata metadata;
            metadata.path = info.absoluteFilePath();
            appendMetadata(std::move(metadata));
        }
    }
#endif

    if (!configuredPort.isEmpty() && !seen.contains(serialIdentity(configuredPort))) {
        DStarSerialPortMetadata metadata;
        metadata.path = configuredPort;
#if defined(Q_OS_WIN)
        // COM paths do not participate in QFileInfo. Let the helper probe be
        // the authority for a manually configured Windows port.
        metadata.present = true;
#else
        metadata.present = QFileInfo::exists(configuredPort);
#endif
        appendMetadata(std::move(metadata));
    }

    std::sort(devices.begin(), devices.end(), [](const DStarSerialDevice& lhs,
                                                 const DStarSerialDevice& rhs) {
        if (lhs.verification != rhs.verification) {
            if (lhs.verification == DStarSerialDevice::Verification::Verified) {
                return true;
            }
            if (rhs.verification == DStarSerialDevice::Verification::Verified) {
                return false;
            }
        }
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.path < rhs.path;
    });

    m_serialDevices = devices;
    emit serialDevicesChanged();

    const DigitalVoiceWaveformProcess& service =
        DigitalVoiceWaveformProcess::instance();
    if (service.isActive()) {
        if (service.registrationVerified()) {
            const int index = serialDeviceIndex(configuredPort);
            if (index >= 0) {
                m_serialDevices[index].verification =
                    DStarSerialDevice::Verification::Verified;
                m_serialDevices[index].detail =
                    tr("Verified by the running D-STAR service.");
                emit serialDevicesChanged();
            }
        }
        return;
    }

    for (const DStarSerialDevice& device : std::as_const(m_serialDevices)) {
        if (device.present
            && device.verification != DStarSerialDevice::Verification::Verified
            && ((m_autoSelectSerial && device.highConfidence)
                || sameSerialDevice(device.path, configuredPort))) {
            m_serialProbeQueue.append(device.path);
        }
    }
    startNextSerialProbe();
}

void DStarModel::cancelSerialProbe()
{
    ++m_serialProbeGeneration;
    m_serialProbeQueue.clear();
    m_activeSerialProbePath.clear();
    m_serialProbeForcedError.clear();
    if (m_serialProbe.state() != QProcess::NotRunning) {
        m_serialProbe.kill();
        m_serialProbe.waitForFinished(500);
    }
    if (m_serialProbe.isOpen()) {
        m_serialProbe.readAll();
    }
}

void DStarModel::startNextSerialProbe()
{
    if (!m_activeSerialProbePath.isEmpty()
        || m_serialProbe.state() != QProcess::NotRunning) {
        return;
    }
    if (m_serialProbeQueue.isEmpty()) {
        autoSelectVerifiedSerial();
        return;
    }

    const QString path = m_serialProbeQueue.takeFirst();
    const int index = serialDeviceIndex(path);
    if (index < 0) {
        startNextSerialProbe();
        return;
    }

    const QString executable = DigitalVoiceWaveformProcess::resolveExecutablePath(
        DigitalVoiceWaveformSettings::executablePath());
    const QFileInfo executableInfo(executable);
    if (!executableInfo.exists() || !executableInfo.isFile()
        || !executableInfo.isExecutable()) {
        m_activeSerialProbePath = path;
        finishSerialProbe(false,
            tr("The ThumbDV verification helper is not available: %1")
                .arg(executable));
        return;
    }

    m_activeSerialProbePath = path;
    m_serialProbeForcedError.clear();
    m_activeSerialProbeGeneration = m_serialProbeGeneration;
    m_serialDevices[index].verification = DStarSerialDevice::Verification::Probing;
    m_serialDevices[index].detail = tr("Checking for a DV3000 response...");
    emit serialDevicesChanged();

    m_serialProbe.setProgram(executable);
    m_serialProbe.setArguments({
        QStringLiteral("--probe-serial"),
        QStringLiteral("--vocoder"), QStringLiteral("thumbdv"),
        QStringLiteral("--serial"), path
    });
    m_serialProbe.setWorkingDirectory(executableInfo.absolutePath());
    m_serialProbe.start();

    const quint64 generation = m_activeSerialProbeGeneration;
    QTimer::singleShot(3000, this, [this, generation, path]() {
        if (generation != m_activeSerialProbeGeneration
            || path != m_activeSerialProbePath
            || m_serialProbe.state() == QProcess::NotRunning) {
            return;
        }
        m_serialProbeForcedError =
            tr("Timed out waiting for a DV3000 response.");
        m_serialProbe.kill();
    });
}

void DStarModel::finishSerialProbe(bool verified, const QString& detail)
{
    const QString path = m_activeSerialProbePath;
    if (path.isEmpty()) {
        return;
    }
    m_activeSerialProbePath.clear();
    m_serialProbeForcedError.clear();

    const int index = serialDeviceIndex(path);
    if (index >= 0) {
        m_serialDevices[index].verification = verified
            ? DStarSerialDevice::Verification::Verified
            : DStarSerialDevice::Verification::Unavailable;
        m_serialDevices[index].detail = verified
            ? tr("Verified DV3000 vocoder.")
            : detail;
        emit serialDevicesChanged();
    }
    QTimer::singleShot(0, this, &DStarModel::startNextSerialProbe);
}

void DStarModel::autoSelectVerifiedSerial()
{
    if (!m_autoSelectSerial) {
        return;
    }

    const QString configured = DigitalVoiceWaveformSettings::serialPort().trimmed();
    const QString selected = autoSelectedSerialPath(m_serialDevices, configured);
    if (!selected.isEmpty() && selected != configured) {
        DigitalVoiceWaveformSettings::setSerialPort(selected);
        emit configurationChanged();
    }
}

int DStarModel::serialDeviceIndex(const QString& path) const
{
    for (int index = 0; index < m_serialDevices.size(); ++index) {
        if (sameSerialDevice(m_serialDevices[index].path, path)) {
            return index;
        }
    }
    return -1;
}

QString DStarModel::decodeStatusValue(const QString& value, int maximumLength)
{
    if (value.size() > maximumLength) {
        return {};
    }
    QString decoded = value;
    decoded.replace(QChar(0x7f), QLatin1Char(' '));
    for (const QChar ch : decoded) {
        const ushort code = ch.unicode();
        if (code < 0x20 || code > 0x7e) {
            return {};
        }
    }
    return decoded.trimmed();
}

QMap<QString, QString> DStarModel::normalizedReport(
    const QMap<QString, QString>& report)
{
    QMap<QString, QString> normalized = report;
    const QString nested = report.value(QStringLiteral("waveform_status"));
    if (nested.isEmpty() || nested.size() > 1024) {
        return normalized;
    }
    const QStringList tokens = nested.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const qsizetype equals = token.indexOf(QLatin1Char('='));
        if (equals <= 0 || equals >= token.size() - 1) {
            continue;
        }
        const QString key = token.left(equals);
        if (key.size() > 48) {
            continue;
        }
        normalized.insert(key, token.mid(equals + 1));
    }
    return normalized;
}

bool DStarModel::reportHasAny(const QMap<QString, QString>& report,
                              const QStringList& keys)
{
    return std::any_of(keys.cbegin(), keys.cend(), [&report](const QString& key) {
        return report.contains(key);
    });
}

void DStarModel::handleWaveformStatus(const QMap<QString, QString>& rawReport)
{
    const QMap<QString, QString> report = normalizedReport(rawReport);
    bool sliceOk = false;
    const int sliceId = report.value(QStringLiteral("slice")).toInt(&sliceOk);
    if (!sliceOk || sliceId < 0 || sliceId > 31) {
        return;
    }

    const QStringList rxHeaderKeys {
        QStringLiteral("destination_rptr_rx"),
        QStringLiteral("departure_rptr_rx"),
        QStringLiteral("companion_call_rx"),
        QStringLiteral("own_call1_rx"),
        QStringLiteral("own_call2_rx")
    };
    const QStringList txHeaderKeys {
        QStringLiteral("destination_rptr_tx"),
        QStringLiteral("departure_rptr_tx"),
        QStringLiteral("companion_call_tx"),
        QStringLiteral("own_call1_tx"),
        QStringLiteral("own_call2_tx"),
        QStringLiteral("message_tx")
    };

    if (reportHasAny(report, rxHeaderKeys)) {
        closeOpenReceive(sliceId);
        DStarTrafficEntry entry;
        entry.direction = DStarTrafficDirection::Receive;
        entry.timestampUtc = QDateTime::currentDateTimeUtc();
        entry.sliceId = sliceId;
        entry.myCall = decodeStatusValue(
            report.value(QStringLiteral("own_call1_rx")), 8);
        entry.myCallSuffix = decodeStatusValue(
            report.value(QStringLiteral("own_call2_rx")), 4);
        entry.urCall = decodeStatusValue(
            report.value(QStringLiteral("companion_call_rx")), 8);
        entry.rpt1 = decodeStatusValue(
            report.value(QStringLiteral("departure_rptr_rx")), 8);
        entry.rpt2 = decodeStatusValue(
            report.value(QStringLiteral("destination_rptr_rx")), 8);
        appendTraffic(entry);
        m_openReceiveBySlice.insert(sliceId, m_traffic.constLast().id);
    }

    if (report.contains(QStringLiteral("message"))) {
        const QString message = decodeStatusValue(
            report.value(QStringLiteral("message")), 20);
        const quint64 openId = m_openReceiveBySlice.value(sliceId, 0);
        const int index = trafficIndexForId(openId);
        if (index >= 0) {
            if (m_traffic[index].message != message) {
                m_traffic[index].message = message;
                scheduleTrafficSave();
                emit trafficChanged();
            }
        } else if (!message.isEmpty()) {
            DStarTrafficEntry entry;
            entry.direction = DStarTrafficDirection::Receive;
            entry.timestampUtc = QDateTime::currentDateTimeUtc();
            entry.sliceId = sliceId;
            entry.message = message;
            appendTraffic(entry);
            m_openReceiveBySlice.insert(sliceId, m_traffic.constLast().id);
        }
    }

    if (report.value(QStringLiteral("RX")).compare(
            QStringLiteral("END"), Qt::CaseInsensitive) == 0) {
        closeOpenReceive(sliceId);
    }

    if (reportHasAny(report, txHeaderKeys)) {
        DStarTrafficEntry entry;
        entry.direction = DStarTrafficDirection::Transmit;
        entry.timestampUtc = QDateTime::currentDateTimeUtc();
        entry.sliceId = sliceId;
        entry.myCall = decodeStatusValue(
            report.value(QStringLiteral("own_call1_tx")), 8);
        entry.myCallSuffix = decodeStatusValue(
            report.value(QStringLiteral("own_call2_tx")), 4);
        entry.urCall = decodeStatusValue(
            report.value(QStringLiteral("companion_call_tx")), 8);
        entry.rpt1 = decodeStatusValue(
            report.value(QStringLiteral("departure_rptr_tx")), 8);
        entry.rpt2 = decodeStatusValue(
            report.value(QStringLiteral("destination_rptr_tx")), 8);
        entry.message = decodeStatusValue(
            report.value(QStringLiteral("message_tx")), 20);
        entry.complete = true;
        appendTraffic(entry);
    }
}

int DStarModel::trafficIndexForId(quint64 id) const
{
    if (id == 0) {
        return -1;
    }
    for (int i = m_traffic.size() - 1; i >= 0; --i) {
        if (m_traffic.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

void DStarModel::appendTraffic(DStarTrafficEntry entry)
{
    entry.id = m_nextTrafficId++;
    if (!entry.timestampUtc.isValid()) {
        entry.timestampUtc = QDateTime::currentDateTimeUtc();
    }
    m_traffic.append(entry);
    while (m_traffic.size() > kMaximumTrafficEntries) {
        const quint64 removedId = m_traffic.constFirst().id;
        m_traffic.removeFirst();
        for (auto it = m_openReceiveBySlice.begin();
             it != m_openReceiveBySlice.end();) {
            if (it.value() == removedId) {
                it = m_openReceiveBySlice.erase(it);
            } else {
                ++it;
            }
        }
    }
    scheduleTrafficSave();
    emit trafficChanged();
}

void DStarModel::closeOpenReceive(int sliceId)
{
    const quint64 id = m_openReceiveBySlice.take(sliceId);
    const int index = trafficIndexForId(id);
    if (index < 0 || m_traffic[index].complete) {
        return;
    }
    m_traffic[index].complete = true;
    scheduleTrafficSave();
    emit trafficChanged();
}

void DStarModel::clearTraffic()
{
    if (m_traffic.isEmpty()) {
        return;
    }
    m_traffic.clear();
    m_openReceiveBySlice.clear();
    saveTraffic();
    emit trafficChanged();
}

void DStarModel::setTrafficPersistencePath(const QString& path)
{
    const QString cleaned = path.trimmed();
    if (m_trafficPersistencePath == cleaned) {
        return;
    }
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveTraffic();
    }
    m_trafficPersistencePath = cleaned;
    loadTraffic();
}

void DStarModel::scheduleTrafficSave()
{
    if (!m_trafficPersistencePath.isEmpty()) {
        m_saveTimer.start();
    }
}

void DStarModel::loadTraffic()
{
    m_traffic.clear();
    m_openReceiveBySlice.clear();
    m_nextTrafficId = 1;
    if (m_trafficPersistencePath.isEmpty()) {
        emit trafficChanged();
        return;
    }

    QFile file(m_trafficPersistencePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit trafficChanged();
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit trafficChanged();
        return;
    }

    const QJsonArray entries = document.object().value(QStringLiteral("entries")).toArray();
    const qsizetype first = std::max<qsizetype>(
        0, entries.size() - kMaximumTrafficEntries);
    for (qsizetype i = first; i < entries.size(); ++i) {
        const QJsonObject object = entries.at(i).toObject();
        DStarTrafficEntry entry;
        entry.id = static_cast<quint64>(object.value(QStringLiteral("id")).toDouble());
        entry.direction = directionFromName(
            object.value(QStringLiteral("direction")).toString());
        entry.timestampUtc = QDateTime::fromString(
            object.value(QStringLiteral("timestampUtc")).toString(), Qt::ISODateWithMs);
        entry.sliceId = object.value(QStringLiteral("sliceId")).toInt(-1);
        entry.myCall = decodeStatusValue(
            object.value(QStringLiteral("myCall")).toString(), 8);
        entry.myCallSuffix = decodeStatusValue(
            object.value(QStringLiteral("myCallSuffix")).toString(), 4);
        entry.urCall = decodeStatusValue(
            object.value(QStringLiteral("urCall")).toString(), 8);
        entry.rpt1 = decodeStatusValue(
            object.value(QStringLiteral("rpt1")).toString(), 8);
        entry.rpt2 = decodeStatusValue(
            object.value(QStringLiteral("rpt2")).toString(), 8);
        entry.message = decodeStatusValue(
            object.value(QStringLiteral("message")).toString(), 20);
        entry.complete = object.value(QStringLiteral("complete")).toBool(true);
        if (entry.id == 0 || !entry.timestampUtc.isValid()
            || entry.sliceId < -1 || entry.sliceId > 31) {
            continue;
        }
        entry.complete = true;
        m_nextTrafficId = std::max(m_nextTrafficId, entry.id + 1);
        m_traffic.append(entry);
    }
    emit trafficChanged();
}

void DStarModel::saveTraffic() const
{
    if (m_trafficPersistencePath.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(m_trafficPersistencePath).absolutePath());
    QJsonArray entries;
    for (const DStarTrafficEntry& entry : m_traffic) {
        entries.append(QJsonObject{
            {QStringLiteral("id"), static_cast<double>(entry.id)},
            {QStringLiteral("direction"), directionName(entry.direction)},
            {QStringLiteral("timestampUtc"),
             entry.timestampUtc.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("sliceId"), entry.sliceId},
            {QStringLiteral("myCall"), entry.myCall},
            {QStringLiteral("myCallSuffix"), entry.myCallSuffix},
            {QStringLiteral("urCall"), entry.urCall},
            {QStringLiteral("rpt1"), entry.rpt1},
            {QStringLiteral("rpt2"), entry.rpt2},
            {QStringLiteral("message"), entry.message},
            {QStringLiteral("complete"), entry.complete}
        });
    }

    QSaveFile file(m_trafficPersistencePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    const QJsonObject root {
        {QStringLiteral("version"), 1},
        {QStringLiteral("entries"), entries}
    };
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

bool DStarModel::start(const QHostAddress& radioAddress,
                       const QString& radioCallsign)
{
    cancelSerialProbe();
    return DigitalVoiceWaveformProcess::instance().startForRadio(
        radioAddress, radioCallsign, DigitalVoiceModeId::DStar);
}

void DStarModel::stop()
{
    DigitalVoiceWaveformProcess::instance().stop();
}

bool DStarModel::serviceActive() const
{
    return DigitalVoiceWaveformProcess::instance().isActive();
}

bool DStarModel::serviceStopping() const
{
    return DigitalVoiceWaveformProcess::instance().state()
        == DigitalVoiceWaveformProcess::State::Stopping;
}

QString DStarModel::serviceStateName() const
{
    const DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    return DigitalVoiceWaveformProcess::stateName(process.state());
}

QString DStarModel::serviceStatusText() const
{
    return DigitalVoiceWaveformProcess::instance().statusText();
}

QString DStarModel::serviceLastError() const
{
    return DigitalVoiceWaveformProcess::instance().lastError();
}

QString DStarModel::serviceHealthName() const
{
    const DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    return DigitalVoiceWaveformProcess::healthName(process.health());
}

QString DStarModel::serviceHealthDetail() const
{
    return DigitalVoiceWaveformProcess::instance().healthDetail();
}

bool DStarModel::registrationVerified() const
{
    return DigitalVoiceWaveformProcess::instance().registrationVerified();
}

int DStarModel::activeSliceId() const
{
    return DigitalVoiceModeRegistry::instance().activeSliceId();
}

bool DStarModel::helperAvailable() const
{
    const QString path = DigitalVoiceWaveformProcess::resolveExecutablePath(
        DigitalVoiceWaveformSettings::executablePath());
    return QFileInfo(path).isExecutable();
}

QString DStarModel::vocoderLabel() const
{
    return DigitalVoiceWaveformSettings::backendLabel(
        DigitalVoiceWaveformSettings::backend());
}

} // namespace AetherSDR
