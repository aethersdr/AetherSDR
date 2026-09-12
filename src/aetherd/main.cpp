#include "DiscoveryStartup.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RadioResourceAdapter.h"
#include "core/control/RadioCatalogue.h"
#include "models/RadioSession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <utility>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aetherd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHERSDR_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AetherSDR headless engine control service"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(
        QStringList{QStringLiteral("s"), QStringLiteral("socket")},
        QStringLiteral("Current-user local socket name."),
        QStringLiteral("name"), QStringLiteral("aetherd-v1"));
    parser.addOption(socketOption);
    const QCommandLineOption localDiscoveryOption(
        QStringLiteral("discover-local"),
        QStringLiteral("Enable LAN discovery and available RTL-SDR USB enumeration; never connect."));
    const QCommandLineOption simDiscoveryOption(
        QStringLiteral("discover-sim"),
        QStringLiteral("Publish the simulator discovery identity without accessing radio hardware."));
    parser.addOption(localDiscoveryOption);
    parser.addOption(simDiscoveryOption);
    const QCommandLineOption controlOption(
        QStringLiteral("allow-local-control"),
        QStringLiteral("Grant current-user local clients non-TX connection and receive control."));
    parser.addOption(controlOption);
    parser.process(app);

    AetherSDR::control::LocalControlServer server(
        nullptr, {}, nullptr, parser.isSet(controlOption));
    if (!server.listen(parser.value(socketOption),
                       AetherSDR::control::LocalControlServer::ListenMode::ReserveEndpoint)) {
        QTextStream(stderr) << "aetherd: cannot listen on local socket '"
                            << parser.value(socketOption) << "'\n";
        return 1;
    }
    // Claim the endpoint before settings or model construction: even the
    // AppSettings singleton constructor can create directories/migrate paths.
    // Native settings must then load before RadioModel snapshots its settings.
    // Constructors/settings imports can pump nested event loops. Reserving
    // the endpoint closes early arrivals without admitting sessions, so no
    // client can race the one-shot target binding or see partial resources.
    std::unique_ptr<AetherSDR::RadioDiscoverySource> discoverySource =
        AetherSDR::aetherd::makeDiscoverySource(
            {parser.isSet(localDiscoveryOption), parser.isSet(simDiscoveryOption)});
    AetherSDR::RadioSession radioSession;
    radioSession.setSessionId(1);
    std::unique_ptr<AetherSDR::control::RadioConnectionTarget> connectionTarget;
    std::unique_ptr<AetherSDR::control::SliceFrequencyTarget> frequencyTarget;
    std::unique_ptr<AetherSDR::control::ReceiveControlTarget> receiveTarget;
    if (parser.isSet(controlOption)) {
        connectionTarget = AetherSDR::control::makeModelRadioConnectionTarget(&radioSession.radioModel());
        if (!connectionTarget || !server.bindConnectionTarget(connectionTarget.get())) {
            QTextStream(stderr) << "aetherd: cannot initialize connection control\n";
            return 1;
        }
        frequencyTarget = AetherSDR::control::makeModelSliceFrequencyTarget(
            &radioSession.radioModel(), connectionTarget.get());
        if (!frequencyTarget || !server.bindFrequencyTarget(frequencyTarget.get())) {
            QTextStream(stderr) << "aetherd: cannot initialize frequency control\n";
            return 1;
        }
        receiveTarget = AetherSDR::control::makeModelReceiveControlTarget(
            &radioSession.radioModel(), connectionTarget.get());
        if (!receiveTarget || !server.bindReceiveTarget(receiveTarget.get())) {
            QTextStream(stderr) << "aetherd: cannot initialize receive control\n";
            return 1;
        }
    }
    AetherSDR::control::RadioCatalogue catalogue(
        std::move(discoverySource), &server.resourceStore());
    [[maybe_unused]] AetherSDR::control::RadioResourceAdapter resources(
        &radioSession.radioModel(), &server.resourceStore(),
        QStringLiteral("radio-1"), nullptr, connectionTarget.get());
    catalogue.start();
    if (!server.startServing()) {
        QTextStream(stderr) << "aetherd: cannot start local client service\n";
        return 1;
    }
    const int result = app.exec();
    // The server was constructed first; stop delivery before target/model
    // teardown rather than relying on reverse local-variable destruction.
    server.close();
    return result;
}
