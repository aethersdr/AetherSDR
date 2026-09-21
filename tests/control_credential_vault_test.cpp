#include "core/control/ControlCredentialVault.h"
#include "core/control/ControlCredentialProvisioner.h"
#include "core/control/LocalCredentialHandshake.h"
#include "aetherd/CredentialStartup.h"

#include <qt6keychain/keychain.h>

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QTimer>

#include <cstdio>
#include <initializer_list>
#include <memory>

using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); ++failures; }
}
void finishRead(const QByteArray& data)
{
    QKeychain::ReadPasswordJob* job = QKeychain::TestControl::pendingRead;
    QKeychain::TestControl::pendingRead = nullptr;
    check(job != nullptr, "missing injected read job");
    if (job) { job->completeBinary(data); }
}
void finishWrite(QKeychain::Error error = QKeychain::NoError)
{
    QKeychain::WritePasswordJob* job = QKeychain::TestControl::pendingWrite;
    QKeychain::TestControl::pendingWrite = nullptr;
    check(job != nullptr, "missing injected write job");
    if (job) { job->finish(error); }
}

void testDispatcherSelection()
{
    struct Case {
        std::initializer_list<const char*> arguments;
        bool expected;
    };
    const Case cases[] = {
        {{"aetherd"}, false},
        {{"aetherd", "--discover-sim", "--allow-local-control"}, false},
        {{"aetherd", "--allow-local-tx"}, false},
        {{"aetherd", "--initialize-credentials"}, true},
        {{"aetherd", "--credential-authority", "authority"}, true},
        {{"aetherd", "--credential-authority=authority", "--tx-admin", "list"}, true},
        {{"aetherd", "--credential-authority-extra=authority"}, false},
        {{"aetherd", "--", "--initialize-credentials"}, false},
        {{"aetherd", "--", "--credential-authority=authority"}, false},
        {{"aetherd", "--socket", "--initialize-credentials"}, false},
        {{"aetherd", "-s", "--credential-authority=authority"}, false},
        {{"aetherd", "--s", "--initialize-credentials"}, false},
        {{"aetherd", "--socket=--initialize-credentials"}, false},
        {{"aetherd", "--socket", "--", "--initialize-credentials"}, true},
        {{"aetherd", "-s", "socket", "--initialize-credentials"}, true},
    };
    for (const Case& test : cases) {
        check(AetherSDR::aetherd::credentialDispatcherRequested(
                  static_cast<int>(test.arguments.size()), test.arguments.begin()) == test.expected,
              "only credential options before -- and outside socket values select the dispatcher");
    }
}

void testProvisioning()
{
    using Provisioner = ControlCredentialProvisioner;
    using Operation = Provisioner::Operation;
    using Error = Provisioner::Error;
    QKeychain::TestControl::reset();
    auto vault = std::make_unique<ControlCredentialVault>(QString(32, u'b'));
    auto provisioner = std::make_unique<Provisioner>(*vault);
    Provisioner::Result last;
    int callbacks = 0;
    const auto receive = [&](Provisioner::Result result) { last = std::move(result); ++callbacks; };
    provisioner->run(Operation::Initialize, {}, receive);
    provisioner->run(Operation::List, {}, receive);
    check(last.error == Error::Busy && callbacks == 1, "provisioning must serialize complete transactions");
    QKeychain::TestControl::failRead(QKeychain::EntryNotFound, {});
    const QByteArray initial = QKeychain::TestControl::pendingWrite->binaryData();
    const auto initialRecords = ControlCredentialVault::decode(initial);
    check(initialRecords && initialRecords->size() == 1
              && initialRecords->first().role == ControlCredentials::Role::GrantAdmin,
          "explicit initialization alone creates an administrative credential");
    finishWrite();
    check(callbacks == 1, "native write success alone cannot complete provisioning");
    finishRead(initial);
    check(last.error == Error::None && last.records.size() == 1 && callbacks == 2,
          "verified readback must complete provisioning without exposing secrets");

    const int writes = QKeychain::TestControl::writeStartCount;
    provisioner->run(Operation::Initialize, {}, receive);
    finishRead(initial);
    check(last.error == Error::AlreadyExists && QKeychain::TestControl::writeStartCount == writes,
          "repeated bootstrap must not overwrite a live authority");
    provisioner->run(Operation::AddClient, {}, receive);
    finishRead(initial);
    const QByteArray expanded = QKeychain::TestControl::pendingWrite->binaryData();
    const auto expandedRecords = ControlCredentialVault::decode(expanded);
    check(expandedRecords && expandedRecords->size() == 2
              && expandedRecords->first().secret == initialRecords->first().secret
              && expandedRecords->last().role == ControlCredentials::Role::Client,
          "adding a client must preserve existing credentials and not make the client an administrator");
    finishWrite();
    finishRead(expanded);
    check(last.error == Error::None && last.records.size() == 2, "client addition must verify readback");
    provisioner->run(Operation::Remove, expandedRecords->last().id, receive);
    finishRead(expanded);
    check(QKeychain::TestControl::pendingWrite->binaryData() == initial,
          "removing a selected credential must preserve unrelated identities exactly");
    finishWrite();
    finishRead(expanded); // model a mismatched/unchanged native store
    check(last.error == Error::ReadbackMismatch, "mismatched readback must never report revocation success");
    const int afterMismatch = QKeychain::TestControl::writeStartCount;
    provisioner->run(Operation::List, {}, receive);
    finishRead(expanded);
    check(last.error == Error::None && QKeychain::TestControl::writeStartCount == afterMismatch,
          "inspection after uncertain write must not perform a blind rollback");
    provisioner->run(Operation::Remove, QString(32, u'0'), receive);
    finishRead(expanded);
    check(last.error == Error::MissingCredential && QKeychain::TestControl::writeStartCount == afterMismatch,
          "unknown removal cannot rewrite storage");
    provisioner->run(Operation::AddAdmin, {}, receive);
    finishRead(expanded);
    finishWrite(QKeychain::AccessDeniedByUser);
    check(last.error == Error::Storage && last.records.isEmpty(), "storage denial cannot create an apparent credential");
    provisioner->run(Operation::Initialize, {}, receive);
    QKeychain::TestControl::failRead(QKeychain::AccessDenied, {});
    check(last.error == Error::Storage && QKeychain::TestControl::pendingWrite == nullptr,
          "failed read must not be treated as missing authority and overwritten");
    const int beforeInvalid = QKeychain::TestControl::readStartCount;
    provisioner->run(static_cast<Operation>(99), {}, receive);
    check(last.error == Error::InvalidInput && QKeychain::TestControl::readStartCount == beforeInvalid,
          "invalid operator action must fail before storage access");
    provisioner->run(Operation::List, {}, receive);
    const int beforeDestroy = callbacks;
    provisioner.reset();
    finishRead(initial);
    check(callbacks == beforeDestroy, "destroying provisioner must suppress pending callbacks");
    provisioner = std::make_unique<Provisioner>(*vault);
    provisioner->run(Operation::List, {}, receive);
    vault.reset();
    check(callbacks == beforeDestroy + 1 && last.error == Error::Storage,
          "destroying storage must complete the pending transaction as unavailable");
    finishRead(initial); // orphaned native job still owns its auto-cleanup
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void testStartup()
{
    using Operation = ControlCredentialProvisioner::Operation;
    const auto parse = [](const QStringList& arguments) {
        QCommandLineParser parser;
        parser.addOption({QStringLiteral("discover-local"), QStringLiteral("test")});
        parser.addOption({QStringLiteral("discover-sim"), QStringLiteral("test")});
        parser.addOption({QStringLiteral("allow-local-control"), QStringLiteral("test")});
        parser.addOption({QStringLiteral("allow-local-tx"), QStringLiteral("test")});
        AetherSDR::aetherd::addCredentialOptions(parser);
        check(parser.parse(QStringList{QStringLiteral("aetherd")} + arguments), "test CLI arguments must parse");
        return AetherSDR::aetherd::credentialOptions(parser);
    };
    const auto ordinary = parse({});
    check(ordinary.authorityId.isEmpty() && !ordinary.operation && ordinary.error.isEmpty(),
          "ordinary daemon startup must not provision or load credentials");
    const auto init = parse({QStringLiteral("--initialize-credentials")});
    check(init.error.isEmpty() && init.operation == Operation::Initialize
              && ControlCredentialVault::validAuthorityId(init.authorityId),
          "explicit setup must generate a valid nonsecret namespace");
    const QString authority(32, u'c');
    const QStringList selected{QStringLiteral("--credential-authority"), authority};
    const auto serve = parse(selected);
    check(serve.error.isEmpty() && !serve.operation && serve.authorityId == authority,
          "selected serving authority must be read-only startup, not bootstrap");
    const auto add = parse(selected + QStringList{QStringLiteral("--add-credential"), QStringLiteral("client")});
    check(add.error.isEmpty() && add.operation == Operation::AddClient, "explicit client role must remain a client");
    const auto txServe = parse(selected + QStringList{QStringLiteral("--allow-local-control"), QStringLiteral("--allow-local-tx")});
    check(txServe.error.isEmpty() && !txServe.operation && txServe.authorityId == authority,
          "serving TX options remain separate from offline credential actions");
    for (const QStringList& action : {
            QStringList{QStringLiteral("--initialize-credentials")},
            QStringList{QStringLiteral("--list-credentials")},
            QStringList{QStringLiteral("--add-credential"), QStringLiteral("client")},
            QStringList{QStringLiteral("--remove-credential"), QString(32, u'd')}}) {
        const auto invalid = parse(selected + action + QStringList{QStringLiteral("--allow-local-tx")});
        check(invalid.error == QStringLiteral("credential administration must be a single offline action without discovery/control/TX")
                  && !invalid.operation,
              "every offline action with TX must return the specific exclusivity diagnostic before vault access");
    }
    for (const QStringList& invalid : {
            QStringList{QStringLiteral("--add-credential"), QStringLiteral("client")},
            selected + selected,
            selected + QStringList{QStringLiteral("--add-credential"), QStringLiteral("transmit")},
            selected + QStringList{QStringLiteral("--list-credentials"), QStringLiteral("--discover-local")},
            selected + QStringList{QStringLiteral("--list-credentials"), QStringLiteral("--allow-local-control")},
            QStringList{QStringLiteral("--initialize-credentials"), QStringLiteral("--list-credentials")},
            QStringList{QStringLiteral("--credential-authority=" )},
            selected + QStringList{QStringLiteral("--remove-credential"), QStringLiteral("../unsafe")}}) {
        check(!parse(invalid).error.isEmpty(), "ambiguous/offline-violating options must fail before opening storage/radio");
    }
    ControlCredentialVault vault(authority);
    ControlCredentials credentials;
    const auto record = ControlCredentials::generate(ControlCredentials::Role::Client);
    const auto bytes = *ControlCredentialVault::encode({record});
    const int beforeWrites = QKeychain::TestControl::writeStartCount;
    QTimer::singleShot(0, [&] { finishRead(bytes); });
    check(AetherSDR::aetherd::loadCredentials(vault, credentials) && credentials.size() == 1,
          "serving startup must load existing records through native read");
    QTimer::singleShot(0, [] { QKeychain::TestControl::failRead(QKeychain::EntryNotFound, {}); });
    check(!AetherSDR::aetherd::loadCredentials(vault, credentials) && credentials.size() == 0
              && QKeychain::TestControl::writeStartCount == beforeWrites,
          "missing authority must leave startup closed without creating credentials");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void testCredentialRecipient()
{
    // Exercise the same hello/write gate as the real admin CLI without opening
    // a socket or reading native credentials. The peer callback stands in for
    // an OS error, foreign account or disconnected peer, not firmware.
    const QByteArray secret(ControlCredentials::kSecretBytes, 't');
    int checks = 0;
    int writes = 0;
    bool sameUser = false;
    const LocalPeerCheck peer = [&] { ++checks; return sameUser; };
    const LocalHelloExchange write = [&](const QJsonObject& request) -> std::optional<QJsonObject> {
        ++writes;
        check(sameUser && checks == 2, "peer identity must be verified before the credential-bearing write");
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        check(request.value(QStringLiteral("method")) == QStringLiteral("hello")
                  && params.value(QStringLiteral("auth")).toObject().value(QStringLiteral("token"))
                      == QString::fromLatin1(secret.toHex()), "verified hello must preserve the selected credential");
        return QJsonObject{{QStringLiteral("accepted"), true}};
    };
    check(!exchangeCredentialHello(secret, peer, write) && checks == 1 && writes == 0,
          "foreign or unavailable peer verification must send no credential bytes");
    check(!exchangeCredentialHello(secret, {}, write) && writes == 0,
          "missing native peer verifier must fail closed");
    check(!exchangeCredentialHello(secret, peer, {}) && checks == 1,
          "missing exchange cannot consume a verification or construct a handshake");
    check(!exchangeCredentialHello(secret.first(31), peer, write) && checks == 1 && writes == 0,
          "invalid secret must fail before peer verification or exchange");
    sameUser = true;
    check(exchangeCredentialHello(secret, peer, write).has_value() && writes == 1,
          "verified current-user peer permits exactly one credential-bearing hello");
    sameUser = false;
    check(!exchangeCredentialHello(secret, peer, write) && checks == 3 && writes == 1,
          "a later connection must reverify identity rather than inherit a prior result");
    check(!localServerIsCurrentUser(-1), "invalid native socket handle cannot identify a trusted server");
}
} // namespace

int main(int argc, char** argv)
{
    testDispatcherSelection(); // Must work before constructing QCoreApplication.
    QCoreApplication app(argc, argv);
    using Error = ControlCredentialVault::Error;
    using Result = ControlCredentialVault::Result;
    const QString authority(32, u'a');
    const auto record = ControlCredentials::generate(ControlCredentials::Role::GrantAdmin);
    const QByteArray data = *ControlCredentialVault::encode({record});
    ControlCredentialVault vault(authority);
    QKeychain::TestControl::reset();
    int deliveries = 0;
    Result last;
    const auto receive = [&](Result result) { last = std::move(result); ++deliveries; };
    vault.load(receive);
    QPointer<QKeychain::ReadPasswordJob> job = QKeychain::TestControl::pendingRead;
    check(job && !job->insecureFallback() && job->autoDelete()
              && job->service() == QStringLiteral("AetherSDR.aetherd")
              && job->key() == QStringLiteral("aetherd.v1.authority.") + authority,
          "native read must use its own namespace, auto-cleanup and no insecure fallback");
    vault.store({record}, receive);
    check(last.error == Error::Busy && deliveries == 1
              && QKeychain::TestControl::writeStartCount == 0,
          "an overlapping operation must fail without writing");
    finishRead(data);
    check(last.error == Error::None && deliveries == 2 && last.records.size() == 1
              && last.records.first().secret == record.secret,
          "native binary read must return only a fully validated record set");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!job, "completed job must not leak");

    vault.store({record}, receive);
    const auto* write = QKeychain::TestControl::pendingWrite;
    check(write && write->binaryData() == data && !write->insecureFallback()
              && write->key() == QStringLiteral("aetherd.v1.authority.") + authority,
          "native write must preserve binary credentials and prohibit plaintext fallback");
    vault.load(receive);
    check(last.error == Error::Busy, "in-flight writes exclude reads as well as writes");
    finishWrite();
    check(last.error == Error::None && last.records.isEmpty(), "write acknowledgment cannot echo secret records");
    for (const QKeychain::Error error : {QKeychain::EntryNotFound, QKeychain::NoBackendAvailable,
            QKeychain::NotImplemented, QKeychain::AccessDeniedByUser, QKeychain::AccessDenied,
            QKeychain::OtherError}) {
        vault.load(receive);
        QKeychain::TestControl::failRead(error, QStringLiteral("sensitive backend diagnostic"));
        const Error expected = error == QKeychain::EntryNotFound ? Error::NotFound
            : error == QKeychain::NoBackendAvailable || error == QKeychain::NotImplemented
                ? Error::Unavailable : Error::Storage;
        check(last.error == expected && last.records.isEmpty(), "storage failures must stay typed and secret-free");
    }
    vault.load(receive);
    finishRead(QByteArrayLiteral("corrupt"));
    check(last.error == Error::InvalidData && last.records.isEmpty(), "invalid storage cannot return partial records");
    vault.store({record}, receive);
    finishWrite(QKeychain::AccessDenied);
    check(last.error == Error::Storage, "failed writes cannot report provisioning success");

    // Completion releases the busy gate before invoking client code. That code
    // can perform readback, or destroy the adapter, without a use-after-free.
    bool readback = false;
    vault.store({record}, [&](Result result) {
        check(result.error == Error::None, "write should complete before readback");
        vault.load([&](Result loaded) { readback = loaded.error == Error::None; });
    });
    finishWrite();
    finishRead(data);
    check(readback, "a completion callback must be able to start readback");
    auto destroyed = std::make_unique<ControlCredentialVault>(authority);
    bool calledAfterDestruction = false;
    destroyed->load([&](Result) { calledAfterDestruction = true; });
    QPointer<QKeychain::ReadPasswordJob> orphan = QKeychain::TestControl::pendingRead;
    destroyed.reset();
    finishRead(data);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!calledAfterDestruction && !orphan, "late completion must drop dead receiver and still clean its job");
    destroyed = std::make_unique<ControlCredentialVault>(authority);
    destroyed->store({record}, [&](Result) { destroyed.reset(); });
    finishWrite();
    check(!destroyed, "write callback must be able to destroy adapter safely");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    testProvisioning();
    testStartup();
    testCredentialRecipient();
    return failures == 0 ? 0 : 1;
}
