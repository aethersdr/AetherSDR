#include "CredentialStartup.h"

#include <QEventLoop>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <string_view>
#include <utility>

namespace AetherSDR::aetherd {

bool credentialDispatcherRequested(int argc, const char* const argv[])
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--") {
            break;
        }
        // --socket/-s (also --s) takes a value in ordinary runs.
        // Its value may itself look like a credential option. Equals-form
        // values cannot match the exact option names below.
        if (argument == "--socket" || argument == "-s" || argument == "--s") {
            ++i;
            continue;
        }
        if (argument == "--initialize-credentials"
            || argument == "--credential-authority"
            || argument.starts_with("--credential-authority=")) {
            return true;
        }
    }
    return false;
}

void addCredentialOptions(QCommandLineParser& parser)
{
    parser.addOption({QStringLiteral("credential-authority"),
        QStringLiteral("Enable verified local identities from this OS-vault authority (never grants TX)."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("initialize-credentials"),
        QStringLiteral("Explicit offline setup: create a new OS-vault authority and grant-admin identity, then exit.")});
    parser.addOption({QStringLiteral("add-credential"),
        QStringLiteral("Offline: add a client or grant-admin credential to the selected authority, then exit."), QStringLiteral("role")});
    parser.addOption({QStringLiteral("remove-credential"),
        QStringLiteral("Offline: remove the selected credential ID from the authority, then exit."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("list-credentials"),
        QStringLiteral("Offline: list credential IDs and roles only, never secrets, then exit.")});
}

CredentialOptions credentialOptions(const QCommandLineParser& parser)
{
    using Operation = control::ControlCredentialProvisioner::Operation;
    CredentialOptions options;
    options.authorityId = parser.value(QStringLiteral("credential-authority"));
    const bool initialize = parser.isSet(QStringLiteral("initialize-credentials"));
    const bool add = parser.isSet(QStringLiteral("add-credential"));
    const bool remove = parser.isSet(QStringLiteral("remove-credential"));
    const bool list = parser.isSet(QStringLiteral("list-credentials"));
    const int operations = int(initialize) + int(add) + int(remove) + int(list);
    for (const auto& name : {QStringLiteral("credential-authority"), QStringLiteral("initialize-credentials"),
                            QStringLiteral("add-credential"), QStringLiteral("remove-credential"), QStringLiteral("list-credentials")}) {
        if (parser.optionNames().count(name) > 1) { options.error = QStringLiteral("duplicate credential option"); return options; }
    }
    if (operations > 1 || (operations != 0 && (parser.isSet(QStringLiteral("discover-local"))
        || parser.isSet(QStringLiteral("discover-sim")) || parser.isSet(QStringLiteral("allow-local-control"))
        || parser.isSet(QStringLiteral("allow-local-tx"))))) {
        options.error = QStringLiteral("credential administration must be a single offline action without discovery/control/TX");
        return options;
    }
    if (initialize && !parser.isSet(QStringLiteral("credential-authority"))) {
        options.authorityId = QUuid::createUuid().toString(QUuid::Id128);
    }
    if ((operations != 0 || parser.isSet(QStringLiteral("credential-authority")))
        && !control::ControlCredentialVault::validAuthorityId(options.authorityId)) {
        options.error = QStringLiteral("credential authority must be a 32-character lowercase hexadecimal ID");
        return options;
    }
    if (initialize) { options.operation = Operation::Initialize; }
    if (list) { options.operation = Operation::List; }
    if (add) {
        const QString role = parser.value(QStringLiteral("add-credential"));
        if (role != QStringLiteral("client") && role != QStringLiteral("grant-admin")) {
            options.error = QStringLiteral("credential role must be client or grant-admin");
            return options;
        }
        options.operation = role == QStringLiteral("client") ? Operation::AddClient : Operation::AddAdmin;
    }
    if (remove) {
        options.operation = Operation::Remove;
        options.removeId = parser.value(QStringLiteral("remove-credential"));
        if (!control::ControlCredentialVault::validAuthorityId(options.removeId)) {
            options.error = QStringLiteral("credential ID must be 32 lowercase hexadecimal characters");
        }
    }
    return options;
}

int provisionCredentials(const CredentialOptions& options, control::ControlCredentialVault& vault)
{
    if (!options.error.isEmpty() || !options.operation) { return 1; }
    control::ControlCredentialProvisioner provisioner(vault);
    control::ControlCredentialProvisioner::Result result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool done = false;
    // Nonsecret namespace remains available even if an accepted write later
    // fails readback, so the operator can inspect/recover instead of losing it.
    QTextStream(stdout) << "authorityId=" << options.authorityId << Qt::endl;
    provisioner.run(*options.operation, options.removeId, [&](auto completed) {
        result = std::move(completed);
        done = true;
        loop.quit();
    });
    timeout.start(30000);
    if (!done) { loop.exec(); }
    if (!done || result.error != control::ControlCredentialProvisioner::Error::None) {
        QTextStream(stderr) << "aetherd: credential action failed (code=" << int(result.error)
                            << ", storage=" << int(result.storageError)
                            << "); no live grants created. A submitted write may require inspection.\n";
        return 1;
    }
    for (const auto& record : result.records) {
        QTextStream(stdout) << "credentialId=" << record.id << " role="
            << (record.role == control::ControlCredentials::Role::GrantAdmin ? "grant-admin" : "client") << '\n';
    }
    return 0;
}

bool loadCredentials(control::ControlCredentialVault& vault, control::ControlCredentials& credentials)
{
    QObject completionContext;
    const QPointer<QObject> context(&completionContext);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool done = false;
    bool loaded = false;
    vault.load([&, context](control::ControlCredentialVault::Result result) {
        if (!context) { return; }
        loaded = result.error == control::ControlCredentialVault::Error::None
            && credentials.replace(result.records);
        if (!loaded) { credentials.clear(); }
        done = true;
        loop.quit();
    });
    timeout.start(30000);
    if (!done) { loop.exec(); }
    return done && loaded;
}

} // namespace AetherSDR::aetherd
