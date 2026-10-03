#include "gui/PeripheralAuthStore.h"

#include <qt6keychain/keychain.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (false)

QString record(const QString& endpoint, const QString& code)
{
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("endpoint"), endpoint},
                             {QStringLiteral("code"), code}};
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void drain()
{
    QCoreApplication::processEvents();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QKeychain::TestControl::reset();
    const QString first = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.10"), 9010);
    const QString second = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.11"), QStringLiteral("192.0.2.11"), 9010);

    // An unavailable OS backend at read time still permits endpoint-scoped
    // removal of the session value; the vault is retried afterwards. Runs first
    // so the AG slot has not completed a read yet.
    {
        const auto ag = PeripheralAuthStore::Device::AntennaGenius;
        PeripheralAuthStore::ClearResult unavailableResult = PeripheralAuthStore::ClearResult::Failed;
        PeripheralAuthStore::clearForEndpoint(ag, second, &app,
            [&](PeripheralAuthStore::ClearResult result) { unavailableResult = result; });
        CHECK(QKeychain::TestControl::readStartCount == 1);
        QKeychain::TestControl::failRead(QKeychain::NoBackendAvailable, QStringLiteral("none"));
        drain();
        CHECK(unavailableResult == PeripheralAuthStore::ClearResult::SessionCleared);
        CHECK(QKeychain::TestControl::pendingDelete == nullptr);
        PeripheralAuthStore::clearForEndpoint(ag, second, &app,
            [&](PeripheralAuthStore::ClearResult result) { unavailableResult = result; });
        CHECK(QKeychain::TestControl::readStartCount == 2); // the vault is retried
        QKeychain::TestControl::failRead(QKeychain::AccessDenied, QStringLiteral("denied"));
        drain();
        CHECK(unavailableResult == PeripheralAuthStore::ClearResult::Failed);
        QKeychain::TestControl::reset();
    }

    CHECK(!PeripheralAuthStore::cachedStatus(PeripheralAuthStore::Device::Tgxl, first));
    CHECK(QKeychain::TestControl::readStartCount == 0); // metadata must not prompt the vault
    PeripheralAuthStore::LoadResult loaded;
    PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, first, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    CHECK(QKeychain::TestControl::readStartCount == 1);
    CHECK(QKeychain::TestControl::pendingRead != nullptr);
    CHECK(!QKeychain::TestControl::pendingRead->insecureFallback());
    QKeychain::TestControl::failRead(QKeychain::AccessDenied, QStringLiteral("denied"));
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Unavailable);

    PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, first, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    CHECK(QKeychain::TestControl::readStartCount == 2); // denial is retryable
    QKeychain::TestControl::completeRead(record(first, QStringLiteral("known-code")));
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Found);
    CHECK(loaded.code == QStringLiteral("known-code"));
    const auto available = PeripheralAuthStore::cachedStatus(PeripheralAuthStore::Device::Tgxl, first);
    CHECK(available && available->status == PeripheralAuthStore::LoadStatus::Found
          && available->persistent);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    CHECK(loaded.code.isEmpty());
    CHECK(QKeychain::TestControl::readStartCount == 2);

    // A legacy plain value carries no peer identity and cannot be reused.
    PeripheralAuthStore::load(PeripheralAuthStore::Device::Pgxl, first, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    QKeychain::TestControl::completeRead(QStringLiteral("legacy-plain-code"));
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);

    // A save made during an OS read must win over the older read completion.
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, first, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    bool saved = false;
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, second,
        QStringLiteral("new-code"), &app, [&](bool ok) { saved = ok; });
    CHECK(QKeychain::TestControl::pendingWrite != nullptr);
    CHECK(!QKeychain::TestControl::pendingWrite->insecureFallback());
    CHECK(QKeychain::TestControl::pendingWrite->textData()
          == record(second, QStringLiteral("new-code")));
    QKeychain::TestControl::completeRead(record(first, QStringLiteral("old-code")));
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.code == QStringLiteral("new-code"));
    bool cleared = false;
    // A cached reply queued before Clear must not deliver the old secret.
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app,
        [&](bool ok) { cleared = ok; });
    CHECK(QKeychain::TestControl::pendingDelete == nullptr);
    CHECK(!cleared);
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    CHECK(loaded.code.isEmpty());
    // Deletion suppresses reads even before its job starts (a write is active).
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    CHECK(loaded.code.isEmpty());
    QKeychain::TestControl::pendingWrite->finish();
    drain();
    CHECK(saved);
    CHECK(QKeychain::TestControl::pendingDelete != nullptr);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    CHECK(loaded.code.isEmpty());
    QKeychain::TestControl::pendingDelete->finish(QKeychain::EntryNotFound);
    drain();
    CHECK(cleared);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);

    // A denied delete must report failure so the dialog never claims the
    // persistent secret was removed from the OS vault.
    bool secondSave = false;
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, second,
        QStringLiteral("another-code"), &app, [&](bool ok) { secondSave = ok; });
    CHECK(QKeychain::TestControl::pendingWrite != nullptr);
    QKeychain::TestControl::pendingWrite->finish();
    drain();
    CHECK(secondSave);
    CHECK(PeripheralAuthStore::cachedStatus(PeripheralAuthStore::Device::AntennaGenius, second)->persistent);
    bool deleteDeniedReported = false;
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app,
        [&](bool ok) { deleteDeniedReported = !ok; });
    CHECK(QKeychain::TestControl::pendingDelete != nullptr);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    CHECK(loaded.code.isEmpty());
    CHECK(PeripheralAuthStore::cachedStatus(PeripheralAuthStore::Device::AntennaGenius, second)->status
          == PeripheralAuthStore::LoadStatus::Missing);
    QKeychain::TestControl::pendingDelete->finish(QKeychain::AccessDenied);
    drain();
    CHECK(deleteDeniedReported);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Found);
    CHECK(loaded.code == QStringLiteral("another-code"));

    // An older successful delete must not erase a newer session save.
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app, {});
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, second,
        QStringLiteral("newer-code"), &app, {});
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.code == QStringLiteral("newer-code"));
    QKeychain::TestControl::pendingDelete->finish();
    drain();
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.code == QStringLiteral("newer-code"));
    QKeychain::TestControl::pendingWrite->finish();
    drain();

    // Two queued clears: the first success remains authoritative if the next fails.
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app, {});
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app, {});
    QKeychain::TestControl::pendingDelete->finish();
    drain();
    QKeychain::TestControl::pendingDelete->finish(QKeychain::AccessDenied);
    drain();
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
    // No OS backend must not prevent local removal, even if this device
    // never had a stored code. It is NOT proof that a vault entry was deleted.
    using ClearResult = PeripheralAuthStore::ClearResult;
    ClearResult clearResult = ClearResult::Failed;
    PeripheralAuthStore::clear(PeripheralAuthStore::Device::Pgxl, &app,
        [&](ClearResult result) { clearResult = result; });
    QKeychain::TestControl::pendingDelete->finish(QKeychain::NoBackendAvailable);
    drain();
    CHECK(clearResult == ClearResult::SessionCleared);

    for (QKeychain::Error error : {QKeychain::NoBackendAvailable, QKeychain::NotImplemented}) {
        PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, second,
            QStringLiteral("stored-before-backend-loss"), &app, {});
        QKeychain::TestControl::pendingWrite->finish();
        drain();
        PeripheralAuthStore::clear(PeripheralAuthStore::Device::AntennaGenius, &app,
            [&](ClearResult result) { clearResult = result; });
        QKeychain::TestControl::pendingDelete->finish(error);
        drain();
        CHECK(clearResult == ClearResult::SessionCleared);
        PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
            [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
        drain();
        CHECK(loaded.status == PeripheralAuthStore::LoadStatus::Missing);
        CHECK(loaded.code.isEmpty());
        // Restoring the backend permits an explicit, confirmed delete retry.
        PeripheralAuthStore::clear(PeripheralAuthStore::Device::AntennaGenius, &app,
            [&](ClearResult result) { clearResult = result; });
        QKeychain::TestControl::pendingDelete->finish();
        drain();
        CHECK(clearResult == ClearResult::Cleared);
    }
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, second,
        QStringLiteral("retained-on-denial"), &app, {});
    QKeychain::TestControl::pendingWrite->finish();
    drain();
    PeripheralAuthStore::clear(PeripheralAuthStore::Device::AntennaGenius, &app,
        [&](ClearResult result) { clearResult = result; });
    QKeychain::TestControl::pendingDelete->finish(QKeychain::AccessDenied);
    drain();
    CHECK(clearResult == ClearResult::Failed);
    PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius, second, &app,
        [&](const PeripheralAuthStore::LoadResult& result) { loaded = result; });
    drain();
    CHECK(loaded.code == QStringLiteral("retained-on-denial"));
    QKeychain::TestControl::pendingDelete = nullptr; // Forget the completed fake job.
    // Endpoint-scoped deletion cannot delete the other shared AG/SS record.
    const auto shared = PeripheralAuthStore::Device::AntennaGenius;
    PeripheralAuthStore::clearForEndpoint(shared, first, &app,
        [&](ClearResult result) { clearResult = result; });
    drain();
    CHECK(clearResult == ClearResult::Cleared);
    CHECK(QKeychain::TestControl::pendingDelete == nullptr);
    PeripheralAuthStore::clearForEndpoint(shared, {}, &app,
        [&](ClearResult result) { clearResult = result; });
    drain();
    CHECK(clearResult == ClearResult::UnknownOwner);
    CHECK(QKeychain::TestControl::pendingDelete == nullptr);
    PeripheralAuthStore::load(shared, second, &app,
        [&](const auto& result) { loaded = result; });
    drain();
    CHECK(loaded.code == "retained-on-denial");
    PeripheralAuthStore::clearForEndpoint(shared, second, &app,
        [&](ClearResult result) { clearResult = result; });
    drain();
    CHECK(QKeychain::TestControl::pendingDelete != nullptr);
    QKeychain::TestControl::pendingDelete->finish();
    drain();
    CHECK(clearResult == ClearResult::Cleared);

    QKeychain::TestControl::pendingDelete = nullptr;
    // A queued conditional delete cannot erase a replacement saved afterwards.
    PeripheralAuthStore::save(shared, first, "first-code", &app);
    PeripheralAuthStore::clearForEndpoint(shared, first, &app,
        [&](ClearResult result) { clearResult = result; });
    drain();
    PeripheralAuthStore::save(shared, second, "replacement-code", &app);
    QKeychain::TestControl::pendingWrite->finish();
    CHECK(QKeychain::TestControl::pendingDelete == nullptr);
    CHECK(QKeychain::TestControl::pendingWrite != nullptr);
    QKeychain::TestControl::pendingWrite->finish();
    drain();
    PeripheralAuthStore::load(shared, second, &app,
        [&](const auto& result) { loaded = result; });
    drain();
    CHECK(loaded.code == "replacement-code");

    return failures == 0 ? 0 : 1;
}
