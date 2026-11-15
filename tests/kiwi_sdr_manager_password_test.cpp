#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

using namespace AetherSDR;

namespace {

constexpr const char* kProfilesKey = "KiwiSdrRxAntennas";

class FakeCredentialStore final : public IKiwiSdrCredentialStore {
public:
    enum class OperationType { Read, Write, Remove };
    struct Operation {
        OperationType type;
        QString key;
        QString value;
        QPointer<QObject> context;
        Callback callback;
    };

    explicit FakeCredentialStore(bool persistent)
        : m_persistent(persistent)
    {
    }

    bool isPersistent() const override { return m_persistent; }

    void read(const QString& key, QObject* context, Callback callback) override
    {
        m_operations.append(
            {OperationType::Read, key, {}, context, std::move(callback)});
    }

    void write(const QString& key, const QString& value, QObject* context,
               Callback callback) override
    {
        m_operations.append(
            {OperationType::Write, key, value, context, std::move(callback)});
    }

    void remove(const QString& key, QObject* context,
                Callback callback) override
    {
        m_operations.append(
            {OperationType::Remove, key, {}, context, std::move(callback)});
    }

    int pendingCount() const { return m_operations.size(); }
    const Operation& next() const { return m_operations.first(); }

    void completeNext(const KiwiSdrCredentialResult& result)
    {
        Operation operation = m_operations.takeFirst();
        if (operation.context) {
            operation.callback(result);
        }
    }

private:
    bool m_persistent{false};
    QList<Operation> m_operations;
};

int fail(const QString& message)
{
    qCritical().noquote() << message;
    return 1;
}

void clearProfiles()
{
    AppSettings::instance().remove(QString::fromLatin1(kProfilesKey));
    AppSettings::instance().save();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-kiwi-password-test"));
    if (!settingsProfile.isValid()) {
        return fail("could not create isolated settings home");
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    clearProfiles();

    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        KiwiSdrManager manager(nullptr, store);
        const QString first = manager.addProfile("First", "first.test:8073");
        const QString second = manager.addProfile("Second", "second.test:8073");
        manager.setProfilePassword(first, "alpha");
        manager.setProfilePassword(second, "bravo");
        if (manager.profilePassword(first) != "alpha"
            || manager.profilePassword(second) != "bravo"
            || store->pendingCount() != 2) {
            return fail("per-profile passwords were not isolated");
        }
        store->completeNext({});
        store->completeNext({});
        if (manager.profilePasswordPersistenceState(first)
                != KiwiSdrPasswordPersistenceState::Stored
            || manager.profilePasswordPersistenceState(second)
                != KiwiSdrPasswordPersistenceState::Stored) {
            return fail("successful writes were not reported as stored");
        }

        manager.setProfilePassword(first, "first-edit");
        manager.setProfilePassword(first, "replacement");
        if (store->pendingCount() != 2
            || store->next().value != "first-edit") {
            return fail("password revisions were not handed to the ordered store");
        }
        store->completeNext({});
        if (store->pendingCount() != 1
            || store->next().value != "replacement") {
            return fail("latest password revision did not remain ordered after the active write");
        }
        store->completeNext({});
        if (manager.profilePassword(first) != "replacement"
            || manager.profilePasswordPersistenceState(first)
                != KiwiSdrPasswordPersistenceState::Stored) {
            return fail("latest password revision did not win");
        }

        manager.setProfilePassword(first, "replacement");
        if (store->pendingCount() != 0
            || manager.profilePasswordPersistenceState(first)
                != KiwiSdrPasswordPersistenceState::Stored) {
            return fail("unchanged stored password queued another write");
        }

        manager.setProfilePassword(first, "cannot-save");
        store->completeNext(
            {KiwiSdrCredentialResultCode::Error, {}, "keychain locked"});
        if (manager.profilePassword(first) != "cannot-save"
            || manager.profilePasswordPersistenceState(first)
                != KiwiSdrPasswordPersistenceState::Error
            || !manager.profilePasswordPersistenceDetail(first)
                    .contains("keychain locked")) {
            return fail("write failure did not preserve the session password and surface an error");
        }

        manager.setProfilePassword(first, "cannot-save");
        if (store->pendingCount() != 1
            || store->next().type != FakeCredentialStore::OperationType::Write
            || manager.profilePasswordPersistenceState(first)
                != KiwiSdrPasswordPersistenceState::Saving) {
            return fail("unchanged password could not retry after a storage error");
        }
        store->completeNext({});
        if (manager.profilePasswordPersistenceState(first)
            != KiwiSdrPasswordPersistenceState::Stored) {
            return fail("password retry did not restore stored state");
        }

        manager.setProfilePassword(second, "remove-me");
        store->completeNext({});
        QSignalSpy persistenceSpy(
            &manager, &KiwiSdrManager::profilePasswordPersistenceChanged);
        manager.removeProfile(second);
        if (store->pendingCount() != 1
            || store->next().type != FakeCredentialStore::OperationType::Remove) {
            return fail("profile removal did not queue credential deletion");
        }
        store->completeNext(
            {KiwiSdrCredentialResultCode::Error, {}, "delete denied"});
        if (persistenceSpy.isEmpty()
            || persistenceSpy.last().at(1).value<KiwiSdrPasswordPersistenceState>()
                != KiwiSdrPasswordPersistenceState::Error
            || !persistenceSpy.last().at(2).toString().contains("delete denied")) {
            return fail("credential deletion failure was silent");
        }

        manager.removeProfile(first);
        store->completeNext({});
    }

    clearProfiles();
    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        {
            KiwiSdrManager manager(nullptr, store);
            const QString id =
                manager.addProfile("Shutdown", "shutdown.test:8073");
            manager.setProfilePassword(id, "first-edit");
            manager.setProfilePassword(id, "replacement");
        }
        if (store->pendingCount() != 2
            || store->next().type != FakeCredentialStore::OperationType::Write
            || store->next().value != "first-edit") {
            return fail("manager teardown discarded a handed-off password revision");
        }
        store->completeNext({});
        if (store->pendingCount() != 1
            || store->next().value != "replacement") {
            return fail("latest password revision did not survive manager teardown");
        }
        store->completeNext({});
    }

    clearProfiles();
    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        {
            KiwiSdrManager manager(nullptr, store);
            const QString id =
                manager.addProfile("Delete", "delete.test:8073");
            manager.setProfilePassword(id, "pending-write");
            manager.removeProfile(id);
        }
        if (store->pendingCount() != 2
            || store->next().type != FakeCredentialStore::OperationType::Write) {
            return fail("profile removal did not preserve the ordered write/delete handoff");
        }
        store->completeNext({});
        if (store->pendingCount() != 1
            || store->next().type != FakeCredentialStore::OperationType::Remove) {
            return fail("credential deletion did not survive manager teardown");
        }
        store->completeNext({});
    }

    clearProfiles();
    QString persistedProfileId;
    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        KiwiSdrManager manager(nullptr, store);
        persistedProfileId = manager.addProfile("Deferred", "deferred.test:8073");
    }
    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        KiwiSdrManager manager(nullptr, store);
        if (store->pendingCount() != 1
            || store->next().type != FakeCredentialStore::OperationType::Read) {
            return fail("startup did not request the saved profile password");
        }
        manager.connectProfile(persistedProfileId);
        if (manager.isProfilePasswordLoaded(persistedProfileId)
            || manager.state(persistedProfileId)
                != KiwiSdrClient::State::Disconnected) {
            return fail("connection was not deferred while the password was loading");
        }
        store->completeNext(
            {KiwiSdrCredentialResultCode::Success, "loaded-secret", {}});
        if (!manager.isProfilePasswordLoaded(persistedProfileId)
            || manager.profilePassword(persistedProfileId) != "loaded-secret") {
            return fail("deferred password read did not complete");
        }
    }
    {
        auto store = std::make_shared<FakeCredentialStore>(true);
        KiwiSdrManager manager(nullptr, store);
        manager.connectProfile(persistedProfileId);
        store->completeNext(
            {KiwiSdrCredentialResultCode::Error, {}, "read denied"});
        if (!manager.isProfilePasswordLoaded(persistedProfileId)
            || manager.profilePasswordPersistenceState(persistedProfileId)
                != KiwiSdrPasswordPersistenceState::Error
            || manager.state(persistedProfileId)
                != KiwiSdrClient::State::Disconnected
            || !manager.profilePasswordPersistenceDetail(persistedProfileId)
                    .contains("read denied")) {
            return fail("credential read failure did not stop deferred connection and surface an error");
        }
        manager.removeProfile(persistedProfileId);
        store->completeNext({});
    }

    clearProfiles();
    QString sessionProfileId;
    {
        auto store = std::make_shared<FakeCredentialStore>(false);
        KiwiSdrManager manager(nullptr, store);
        sessionProfileId = manager.addProfile("Session", "session.test:8073");
        manager.setProfilePassword(sessionProfileId, "session-secret");
        if (manager.profilePasswordPersistenceState(sessionProfileId)
                != KiwiSdrPasswordPersistenceState::SessionOnly
            || AppSettings::instance()
                   .value(QString::fromLatin1(kProfilesKey)).toString()
                   .contains("session-secret")) {
            return fail("no-keychain fallback was not session-only");
        }
        store->completeNext({});
    }
    {
        auto store = std::make_shared<FakeCredentialStore>(false);
        KiwiSdrManager manager(nullptr, store);
        if (store->pendingCount() != 1) {
            return fail("session-only restart did not perform a fresh credential read");
        }
        store->completeNext(
            {KiwiSdrCredentialResultCode::NotFound, {}, {}});
        if (manager.profilePassword(sessionProfileId).size() != 0
            || manager.profilePasswordPersistenceState(sessionProfileId)
                != KiwiSdrPasswordPersistenceState::NoPassword) {
            return fail("session-only password survived a simulated restart");
        }
        manager.removeProfile(sessionProfileId);
        store->completeNext({});
    }

    clearProfiles();
    return 0;
}
