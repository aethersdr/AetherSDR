#include "DiscoveryStartup.h"
#include "CredentialStartup.h"
#include "GrantAdminClient.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RadioResourceAdapter.h"
#include "core/control/RadioCatalogue.h"
#include "models/RadioSession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QLockFile>
#include <QTextStream>

#include <utility>

int main(int argc, char* argv[])
{
#ifdef Q_OS_MAC
    // QtKeychain's Apple backend delivers completions on the native main
    // dispatch queue. Qt's default QCoreApplication UNIX dispatcher does not
    // service that queue. Select CoreFoundation only for credential runs;
    // ordinary observe/control runs and worker threads keep their defaults.
    const bool credentialDispatcher = AetherSDR::aetherd::credentialDispatcherRequested(argc, argv);
    const bool hadDispatcherSetting = qEnvironmentVariableIsSet("QT_EVENT_DISPATCHER_CORE_FOUNDATION");
    const QByteArray dispatcherSetting = qgetenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION");
    if (credentialDispatcher) {
        qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", "1");
    }
#endif
    QCoreApplication app(argc, argv);
#ifdef Q_OS_MAC
    if (credentialDispatcher) {
        if (hadDispatcherSetting) {
            qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", dispatcherSetting);
        } else {
            qunsetenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION");
        }
    }
#endif
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
    const QCommandLineOption transmitOption(QStringLiteral("allow-local-tx"),
        QStringLiteral("Enable explicit credential-bound TX grants; never arms a client at startup."));
    parser.addOption(transmitOption);
    AetherSDR::aetherd::addCredentialOptions(parser);
    AetherSDR::aetherd::addGrantAdminOptions(parser);
    parser.process(app);
    if (parser.isSet(QStringLiteral("tx-admin"))) {
        return AetherSDR::aetherd::runGrantAdmin(parser);
    }
    for (const auto& option : {QStringLiteral("admin-credential"), QStringLiteral("tx-client"),
                              QStringLiteral("tx-grant"), QStringLiteral("radio-generation"),
                              QStringLiteral("tx-operation-ms"), QStringLiteral("tx-lifetime-ms"),
                              QStringLiteral("tx-keepalive-ms")}) {
        if (parser.isSet(option)) {
            QTextStream(stderr) << "aetherd: grant administration options require --tx-admin\n";
            return 1;
        }
    }

    const auto credentialOptions = AetherSDR::aetherd::credentialOptions(parser);
    if (!credentialOptions.error.isEmpty()) {
        QTextStream(stderr) << "aetherd: " << credentialOptions.error << '\n';
        return 1;
    }
    if (parser.isSet(transmitOption) && (credentialOptions.authorityId.isEmpty()
        || credentialOptions.operation || !parser.isSet(controlOption))) {
        QTextStream(stderr) << "aetherd: local TX requires a serving credential authority and local control\n";
        return 1;
    }

    AetherSDR::control::LocalControlServer server(
        nullptr, {}, nullptr, parser.isSet(controlOption));
    if (!server.listen(parser.value(socketOption),
                       AetherSDR::control::LocalControlServer::ListenMode::ReserveEndpoint)) {
        QTextStream(stderr) << "aetherd: cannot listen on local socket '"
                            << parser.value(socketOption) << "'\n";
        return 1;
    }
    std::unique_ptr<QLockFile> authorityReservation;
    AetherSDR::control::ControlCredentials credentials;
    if (!credentialOptions.authorityId.isEmpty()) {
        // The authority reservation is independent of --socket: using another
        // endpoint cannot race provisioning or keep an old verifier alive.
        authorityReservation = AetherSDR::control::LocalControlServer::reserveCredentialAuthority(
            credentialOptions.authorityId);
        if (!authorityReservation) {
            QTextStream(stderr) << "aetherd: credential authority is unavailable or already in use\n";
            return 1;
        }
        AetherSDR::control::ControlCredentialVault vault(credentialOptions.authorityId);
        if (credentialOptions.operation) {
            return AetherSDR::aetherd::provisionCredentials(credentialOptions, vault);
        }
        if (!AetherSDR::aetherd::loadCredentials(vault, credentials) || !server.bindCredentials(&credentials)) {
            QTextStream(stderr) << "aetherd: cannot load the requested OS-vault authority; service remains closed\n";
            return 1;
        }
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
    std::unique_ptr<AetherSDR::control::TransmitControlTarget> transmitTarget;
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
    if (parser.isSet(transmitOption)) {
        transmitTarget = AetherSDR::control::makeModelTransmitControlTarget(&radioSession.radioModel());
        if (!transmitTarget || !server.bindTransmitTarget(transmitTarget.get())) {
            QTextStream(stderr) << "aetherd: cannot initialize transmit control\n";
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
