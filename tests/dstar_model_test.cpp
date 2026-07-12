#include "models/DStarModel.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>

#include <cstdio>

using AetherSDR::DStarConfiguration;
using AetherSDR::DStarModel;
using AetherSDR::DStarResolvedRoute;
using AetherSDR::DStarRouteDestination;
using AetherSDR::DStarRouteOrigin;
using AetherSDR::DStarRouteRequest;
using AetherSDR::DStarSerialDevice;
using AetherSDR::DStarSerialPortMetadata;
using AetherSDR::DStarTrafficDirection;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

QString encoded(const QString& text)
{
    QString value = text;
    value.replace(QLatin1Char(' '), QChar(0x7f));
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporaryDir;
    report("temporary persistence directory is available", temporaryDir.isValid());
    const QString historyPath = temporaryDir.filePath(QStringLiteral("dstar-traffic.json"));

    DStarSerialPortMetadata windowsThumbDv;
    windowsThumbDv.path = QStringLiteral("COM14");
    windowsThumbDv.description = QStringLiteral("FT230X Basic UART");
    windowsThumbDv.manufacturer = QStringLiteral("FTDI");
    windowsThumbDv.serialNumber = QStringLiteral("DA016GZ6");
    windowsThumbDv.vendorIdentifier = 0x0403;
    windowsThumbDv.productIdentifier = 0x6015;
    const DStarSerialDevice windowsCandidate =
        DStarModel::classifySerialDevice(windowsThumbDv);
    report("Windows FT230X identity is a high-confidence probe candidate",
           windowsCandidate.highConfidence && windowsCandidate.score >= 120);

    DStarSerialPortMetadata unrelatedWindowsPort;
    unrelatedWindowsPort.path = QStringLiteral("COM9");
    unrelatedWindowsPort.description = QStringLiteral("Bluetooth serial port");
    const DStarSerialDevice unrelatedCandidate =
        DStarModel::classifySerialDevice(unrelatedWindowsPort);
    report("unrelated Windows serial port is not auto-detection material",
           !unrelatedCandidate.highConfidence && unrelatedCandidate.score == 0);

    const DStarSerialDevice configuredComPort = DStarModel::classifySerialDevice(
        unrelatedWindowsPort, QStringLiteral("\\\\.\\COM9"));
    report("Windows COM and extended COM paths identify the same configured device",
           configuredComPort.detail.startsWith(QStringLiteral("Configured device")));

    DStarSerialPortMetadata stableLinuxPort;
    stableLinuxPort.path = QStringLiteral(
        "/dev/serial/by-id/usb-FTDI_FT230X_Basic_UART_DA016GZ6-if00-port0");
    stableLinuxPort.stablePath = true;
    const DStarSerialDevice stableCandidate =
        DStarModel::classifySerialDevice(stableLinuxPort);
    report("stable Linux by-id paths outrank transient tty aliases",
           stableCandidate.score > 30);

    DStarSerialDevice verifiedWindows = windowsCandidate;
    verifiedWindows.verification = DStarSerialDevice::Verification::Verified;
    report("sole verified ThumbDV replaces a stale Windows COM path",
           DStarModel::autoSelectedSerialPath(
               {verifiedWindows}, QStringLiteral("COM9")) == QStringLiteral("COM14"));

    DStarSerialDevice verifiedMac = verifiedWindows;
    verifiedMac.path = QStringLiteral("/dev/cu.usbserial-NEWDEVICE");
    report("sole verified ThumbDV replaces a stale macOS callout path",
           DStarModel::autoSelectedSerialPath(
               {verifiedMac}, QStringLiteral("/dev/cu.usbserial-OLDDEVICE"))
               == verifiedMac.path);

    DStarSerialDevice verifiedLinux = stableCandidate;
    verifiedLinux.verification = DStarSerialDevice::Verification::Verified;
    report("sole verified ThumbDV replaces a stale Linux tty path",
           DStarModel::autoSelectedSerialPath(
               {verifiedLinux}, QStringLiteral("/dev/ttyUSB7"))
               == verifiedLinux.path);
    report("an already verified configured ThumbDV remains selected",
           DStarModel::autoSelectedSerialPath(
               {verifiedWindows}, QStringLiteral("COM14")) == QStringLiteral("COM14"));

    DStarSerialDevice secondVerified = verifiedWindows;
    secondVerified.path = QStringLiteral("COM15");
    report("multiple verified vocoders do not override the user's selection",
           DStarModel::autoSelectedSerialPath(
               {verifiedWindows, secondVerified}, QStringLiteral("COM9"))
               == QStringLiteral("COM9"));

    {
        DStarModel model;
        model.setTrafficPersistencePath(historyPath);

        DStarConfiguration valid;
        valid.myCall = QStringLiteral("KI4TTZ");
        valid.myCallSuffix = QStringLiteral("91AD");
        valid.urCall = QStringLiteral("CQCQCQ");
        valid.rpt1 = QStringLiteral("DIRECT");
        valid.rpt2 = QStringLiteral("DIRECT");
        valid.message = QStringLiteral("HELLO DSTAR");
        report("valid D-STAR configuration is accepted",
               model.configurationError(valid).isEmpty());
        const QString runtimeCommand = DStarModel::runtimeSetCommand(valid);
        report("runtime command encodes message spaces as SmartSDR DEL bytes",
               runtimeCommand.contains(
                   QStringLiteral("message=HELLO%1DSTAR").arg(QChar(0x7f)))
                   && !runtimeCommand.contains(QStringLiteral("HELLO DSTAR")));
        valid.message = QString(21, QLatin1Char('X'));
        report("overlength D-STAR message is rejected",
               !model.configurationError(valid).isEmpty());

        DStarRouteRequest routeRequest;
        DStarResolvedRoute route;
        QString routeError;
        report("direct local CQ resolves without repeater fields",
               DStarModel::resolveRoute(routeRequest, &route, &routeError)
                   && route.urCall == QStringLiteral("CQCQCQ")
                   && route.rpt1 == QStringLiteral("DIRECT")
                   && route.rpt2 == QStringLiteral("DIRECT"));

        routeRequest.origin = DStarRouteOrigin::Repeater;
        routeRequest.accessRepeaterCallsign = QStringLiteral("WX4GPB");
        routeRequest.accessRepeaterModule = QLatin1Char('C');
        report("local repeater CQ stays on the selected repeater",
               DStarModel::resolveRoute(routeRequest, &route, &routeError)
                   && route.urCall == QStringLiteral("CQCQCQ")
                   && route.rpt1 == QStringLiteral("WX4GPB C")
                   && route.rpt2 == QStringLiteral("WX4GPB C"));

        routeRequest.destination = DStarRouteDestination::Station;
        routeRequest.destinationCallsign = QStringLiteral("N9JA");
        report("station routing derives the access repeater gateway",
               DStarModel::resolveRoute(routeRequest, &route, &routeError)
                   && route.urCall == QStringLiteral("N9JA")
                   && route.rpt1 == QStringLiteral("WX4GPB C")
                   && route.rpt2 == QStringLiteral("WX4GPB G"));

        routeRequest.destination = DStarRouteDestination::RepeaterArea;
        routeRequest.destinationCallsign = QStringLiteral("WD4STR");
        routeRequest.destinationRepeaterModule = QLatin1Char('B');
        report("repeater-area routing encodes slash route and gateway",
               DStarModel::resolveRoute(routeRequest, &route, &routeError)
                   && route.urCall == QStringLiteral("/WD4STRB")
                   && route.rpt1 == QStringLiteral("WX4GPB C")
                   && route.rpt2 == QStringLiteral("WX4GPB G"));

        DStarConfiguration routed = valid;
        routed.message = QStringLiteral("HELLO DSTAR");
        routed.urCall = route.urCall;
        routed.rpt1 = route.rpt1;
        routed.rpt2 = route.rpt2;
        const DStarRouteRequest inferred =
            DStarModel::routeRequestForConfiguration(routed);
        report("resolved repeater route round-trips to friendly fields",
               inferred.origin == DStarRouteOrigin::Repeater
                   && inferred.destination == DStarRouteDestination::RepeaterArea
                   && inferred.accessRepeaterCallsign == QStringLiteral("WX4GPB")
                   && inferred.accessRepeaterModule == QLatin1Char('C')
                   && inferred.destinationCallsign == QStringLiteral("WD4STR")
                   && inferred.destinationRepeaterModule == QLatin1Char('B'));

        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("2")},
            {QStringLiteral("destination_rptr_rx"), encoded(QStringLiteral("DIRECT  "))},
            {QStringLiteral("departure_rptr_rx"), encoded(QStringLiteral("DIRECT  "))},
            {QStringLiteral("companion_call_rx"), encoded(QStringLiteral("CQCQCQ  "))},
            {QStringLiteral("own_call1_rx"), encoded(QStringLiteral("KI4TTZ  "))},
            {QStringLiteral("own_call2_rx"), encoded(QStringLiteral("91AD"))}
        });
        report("RX header starts one traffic session", model.traffic().size() == 1);
        report("RX header maps callsign and route fields",
               model.traffic().constFirst().direction == DStarTrafficDirection::Receive
                   && model.traffic().constFirst().myCall == QStringLiteral("KI4TTZ")
                   && model.traffic().constFirst().urCall == QStringLiteral("CQCQCQ")
                   && model.traffic().constFirst().rpt1 == QStringLiteral("DIRECT")
                   && model.traffic().constFirst().rpt2 == QStringLiteral("DIRECT"));

        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("2")},
            {QStringLiteral("message"), encoded(QStringLiteral("ON FREQUENCY NOW    "))}
        });
        report("RX slow-data message attaches to open session",
               model.traffic().size() == 1
                   && model.traffic().constFirst().message
                       == QStringLiteral("ON FREQUENCY NOW"));

        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("2")},
            {QStringLiteral("RX"), QStringLiteral("END")}
        });
        report("RX end closes the session", model.traffic().constFirst().complete);

        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("2")},
            {QStringLiteral("own_call1_tx"), encoded(QStringLiteral("KK7GWY  "))},
            {QStringLiteral("own_call2_tx"), encoded(QStringLiteral("AETH"))},
            {QStringLiteral("companion_call_tx"), encoded(QStringLiteral("KI4TTZ  "))},
            {QStringLiteral("departure_rptr_tx"), encoded(QStringLiteral("DIRECT  "))},
            {QStringLiteral("destination_rptr_tx"), encoded(QStringLiteral("DIRECT  "))},
            {QStringLiteral("message_tx"), encoded(QStringLiteral("COPY YOU CLEAR      "))}
        });
        report("TX status creates a completed outgoing entry",
               model.traffic().size() == 2
                   && model.traffic().constLast().direction
                       == DStarTrafficDirection::Transmit
                   && model.traffic().constLast().message
                       == QStringLiteral("COPY YOU CLEAR")
                   && model.traffic().constLast().complete);

        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("3")},
            {QStringLiteral("waveform_status"),
             QStringLiteral("own_call1_rx=N7TEST%1 companion_call_rx=CQCQCQ%1 ")
                 .arg(QChar(0x7f))}
        });
        report("nested waveform_status payload is normalized",
               model.traffic().size() == 3
                   && model.traffic().constLast().myCall == QStringLiteral("N7TEST"));

        const qsizetype beforeMalformed = model.traffic().size();
        model.handleWaveformStatus({
            {QStringLiteral("slice"), QStringLiteral("999")},
            {QStringLiteral("own_call1_rx"), QStringLiteral("BAD")}
        });
        report("out-of-range slice status is rejected",
               model.traffic().size() == beforeMalformed);
    }

    report("traffic history is written atomically", QFileInfo::exists(historyPath));
    {
        DStarModel reloaded;
        reloaded.setTrafficPersistencePath(historyPath);
        report("traffic history reloads across model instances",
               reloaded.traffic().size() == 3
                   && reloaded.traffic().constFirst().complete);
        reloaded.clearTraffic();
    }
    {
        DStarModel cleared;
        cleared.setTrafficPersistencePath(historyPath);
        report("cleared traffic history remains empty", cleared.traffic().isEmpty());
    }

    std::printf("\n%d D-STAR model test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
