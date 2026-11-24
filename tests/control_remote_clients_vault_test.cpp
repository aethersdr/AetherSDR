// Paired clients at run time (aetherd-remote-clients.md §3) through the real
// VaultPairingIssuer and vault adapter, with in-memory QtKeychain jobs (no OS
// vault): a revoke removes the record from the vault first and only then ends
// it live, closing its sessions; a local identity is never removable here; a
// refused write leaves the client working; a revoke waits behind a pairing's
// vault write instead of failing it.
#include "core/control/ControlCredentialVault.h"
#include "core/control/VaultPairingIssuer.h"

#include <qt6keychain/keychain.h>

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR::control;
using Role = ControlCredentials::Role;
using Result = RemoteClientAdmin::RevokeResult;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
bool finishRead(const QByteArray& data)
{
    QKeychain::ReadPasswordJob* job = QKeychain::TestControl::pendingRead;
    QKeychain::TestControl::pendingRead = nullptr;
    if (job) { job->completeBinary(data); }
    return job != nullptr;
}
bool finishWrite(QKeychain::Error error = QKeychain::NoError)
{
    QKeychain::WritePasswordJob* job = QKeychain::TestControl::pendingWrite;
    QKeychain::TestControl::pendingWrite = nullptr;
    if (job) { job->finish(error); }
    return job != nullptr;
}
QByteArray bytes(const QList<ControlCredentials::Record>& records)
{
    return ControlCredentialVault::encode(records).value_or(QByteArray());
}

struct Fixture {
    ControlCredentialVault vault{QStringLiteral("0123456789abcdef0123456789abcdef")};
    ControlCredentials credentials;
    VaultPairingIssuer store{vault, credentials};
    ControlCredentials::Record admin{ControlCredentials::generate(Role::GrantAdmin)};
    ControlCredentials::Record client{ControlCredentials::generate(Role::Client)};
    QByteArray laptopToken;
    ControlCredentials::Record laptop{ControlCredentials::generateRemote(Role::RemoteControl, &laptopToken)};
    Fixture()
    {
        QKeychain::TestControl::reset();
        laptop.name = QStringLiteral("Laptop");
        check(credentials.replace({admin, client, laptop}), "load a grant-admin, a TX client and a paired laptop");
    }
    QList<ControlCredentials::Record> all() const { return {admin, client, laptop}; }
};
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    {
        Fixture f;
        const auto listed = f.store.clients();
        check(listed.size() == 1 && listed.first().credentialId == f.laptop.id
                  && listed.first().name == "Laptop" && listed.first().scope == "remote-control",
              "clients() lists the paired laptop by id, name and scope, and no local identity");

        // What a live session holds; a session ends when its principal retires
        // (ControlSession's changed() handler; the loopback test drives it).
        const auto principal = f.credentials.verify(QJsonObject{{"scheme", "bearer"},
            {"token", QString::fromLatin1(f.laptopToken.toHex())}});
        check(principal.valid(), "the laptop's credential is live");

        std::optional<Result> result;
        f.store.revoke(f.laptop.id, [&](Result r) { result = r; });
        check(finishRead(bytes(f.all())), "revoke reads the vault");
        check(!result && f.store.clients().size() == 1 && principal.valid(),
              "nothing is retired live before the vault write");
        check(finishWrite(), "revoke writes the vault without the laptop");
        check(finishRead(bytes({f.admin, f.client})), "and reads it back");
        check(result == Result::Revoked && f.store.clients().isEmpty() && !principal.valid(),
              "once stored, the laptop is retired live: the principal its sessions hold is retired");
        check(!f.credentials.verify(QJsonObject{{"scheme", "bearer"},
                  {"token", QString::fromLatin1(f.laptopToken.toHex())}}).valid(),
              "its token no longer verifies");
    }
    {
        Fixture f;
        const int readsBefore = QKeychain::TestControl::readStartCount;
        std::optional<Result> adminResult, clientResult, unknownResult;
        f.store.revoke(f.admin.id, [&](Result r) { adminResult = r; });
        f.store.revoke(f.client.id, [&](Result r) { clientResult = r; });
        f.store.revoke(QStringLiteral("ffffffffffffffffffffffffffffffff"), [&](Result r) { unknownResult = r; });
        check(adminResult == Result::NotFound && clientResult == Result::NotFound
                  && unknownResult == Result::NotFound && QKeychain::TestControl::readStartCount == readsBefore,
              "the grant-admin and a TX client read as unknown, and the vault is never touched");
        check(f.credentials.size() == 3, "every local identity is still live");
    }
    {
        Fixture f;
        std::optional<Result> result;
        f.store.revoke(f.laptop.id, [&](Result r) { result = r; });
        check(finishRead(bytes(f.all())) && finishWrite(QKeychain::AccessDenied),
              "the vault refuses the write");
        check(result == Result::Failed && f.store.clients().size() == 1
                  && f.credentials.verify(QJsonObject{{"scheme", "bearer"},
                         {"token", QString::fromLatin1(f.laptopToken.toHex())}}).valid(),
              "a refused removal reports failure and the laptop still works, as it would after a restart");
    }
    {
        Fixture f;
        std::optional<PairingIssuer::Issued> issued;
        std::optional<Result> result;
        f.store.issue(QStringLiteral("Tablet"), [&](std::optional<PairingIssuer::Issued> i) { issued = i; });
        f.store.revoke(f.laptop.id, [&](Result r) { result = r; });
        check(QKeychain::TestControl::pendingRead != nullptr && !result,
              "a revoke queued behind a pairing write waits instead of failing it as busy");
        check(finishRead(bytes(f.all())), "the pairing reads the vault");
        const QByteArray written = QKeychain::TestControl::pendingWrite
            ? QKeychain::TestControl::pendingWrite->binaryData() : QByteArray();
        check(!written.isEmpty() && finishWrite() && finishRead(written),
              "the pairing writes its record and reads it back");
        check(issued && f.store.clients().size() == 2, "the pairing completes: two paired clients");
        check(QKeychain::TestControl::pendingRead != nullptr && !result, "then the queued revoke starts");
        const auto afterPair = ControlCredentialVault::decode(written).value_or(QList<ControlCredentials::Record>{});
        QList<ControlCredentials::Record> afterRevoke;
        for (const auto& r : afterPair) { if (r.id != f.laptop.id) { afterRevoke.append(r); } }
        check(finishRead(written) && finishWrite() && finishRead(bytes(afterRevoke)),
              "the revoke reads the paired vault, writes it without the laptop and reads it back");
        check(result == Result::Revoked && f.store.clients().size() == 1
                  && f.store.clients().first().name == "Tablet",
              "and completes: the tablet stays, the laptop is gone");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
