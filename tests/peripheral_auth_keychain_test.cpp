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
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app,
        [&](bool ok) { cleared = ok; });
    CHECK(QKeychain::TestControl::pendingDelete == nullptr);
    CHECK(!cleared);
    QKeychain::TestControl::pendingWrite->finish();
    drain();
    CHECK(saved);
    CHECK(QKeychain::TestControl::pendingDelete != nullptr);
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
    bool deleteDeniedReported = false;
    PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius, {}, {}, &app,
        [&](bool ok) { deleteDeniedReported = !ok; });
    CHECK(QKeychain::TestControl::pendingDelete != nullptr);
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
    return failures == 0 ? 0 : 1;
}
