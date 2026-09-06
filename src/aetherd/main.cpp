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
    parser.process(app);

    std::unique_ptr<AetherSDR::RadioDiscoverySource> discoverySource =
        AetherSDR::aetherd::makeDiscoverySource(
            {parser.isSet(localDiscoveryOption), parser.isSet(simDiscoveryOption)});
    AetherSDR::RadioSession radioSession;
    radioSession.setSessionId(1);
    AetherSDR::control::LocalControlServer server;
    AetherSDR::control::RadioCatalogue catalogue(
        std::move(discoverySource), &server.resourceStore());
    [[maybe_unused]] AetherSDR::control::RadioResourceAdapter resources(
        &radioSession.radioModel(), &server.resourceStore(),
        QStringLiteral("radio-1"));
    if (!server.listen(parser.value(socketOption))) {
        QTextStream(stderr) << "aetherd: cannot listen on local socket '"
                            << parser.value(socketOption) << "'\n";
        return 1;
    }
    catalogue.start();
    return app.exec();
}
