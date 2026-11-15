#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrManager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

constexpr const char* kProfilesKey = "KiwiSdrRxAntennas";

// CSV import/export never touches passwords, so a fully no-op store is
// enough here (unlike kiwi_sdr_manager_password_test.cpp, which exercises
// the store's async read/write/remove machinery directly).
class NullCredentialStore final : public IKiwiSdrCredentialStore {
public:
    bool isPersistent() const override { return false; }
    void read(const QString&, QObject*, Callback) override {}
    void write(const QString&, const QString&, QObject*, Callback) override {}
    void remove(const QString&, QObject*, Callback) override {}
};

std::shared_ptr<IKiwiSdrCredentialStore> makeStore()
{
    return std::make_shared<NullCredentialStore>();
}

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
    TestSettingsProfile settingsProfile(QStringLiteral("aether-kiwi-csv-test"));
    if (!settingsProfile.isValid()) {
        return fail("could not create isolated settings home");
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    clearProfiles();

    // Round trip: export then import into a fresh manager reproduces every
    // visible field, minting a brand-new id rather than reusing the
    // exported one (#4586 — the CSV never carries an id).
    {
        KiwiSdrManager exporter(nullptr, makeStore());
        const QString sourceId =
            exporter.addProfile("Test RX", "kiwi.example.test:8073");
        KiwiSdrAntennaProfile p = exporter.profile(sourceId);
        p.autoConnect = true;
        p.keepAudioDuringTx = true;
        p.waterfallAutoScale = false;
        p.waterfallMinDbm = -120;
        p.waterfallMaxDbm = -20;
        p.waterfallRate = 2;
        exporter.updateProfile(p);
        const QByteArray csv = exporter.exportProfilesCsv();

        clearProfiles();
        KiwiSdrManager importer(nullptr, makeStore());
        const KiwiSdrCsvImportResult result = importer.importProfilesCsv(csv);
        if (!result.ok() || result.addedCount != 1 || result.mergedCount != 0) {
            return fail("round-trip import did not add exactly one new profile");
        }
        const QVector<KiwiSdrAntennaProfile> profiles = importer.profiles();
        if (profiles.size() != 1) {
            return fail("round-trip import produced the wrong profile count");
        }
        const KiwiSdrAntennaProfile& imported = profiles.constFirst();
        if (imported.id == sourceId) {
            return fail("import reused the exported id instead of minting a fresh one");
        }
        if (imported.name != QStringLiteral("Test RX")
            || imported.endpoint != QStringLiteral("kiwi.example.test:8073")
            || !imported.autoConnect || !imported.keepAudioDuringTx
            || imported.waterfallAutoScale || imported.waterfallMinDbm != -120
            || imported.waterfallMaxDbm != -20 || imported.waterfallRate != 2) {
            return fail("round-trip import did not preserve the profile's fields");
        }
    }

    // Merge: a row whose endpoint matches an existing profile updates that
    // profile in place — keeping its id — instead of creating a duplicate.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const QString id =
            manager.addProfile("Old Name", "merge.example.test:8073");

        const QByteArray csv = QByteArrayLiteral(
            "FORMAT_VERSION,NAME,ENDPOINT,AUTO_CONNECT\r\n"
            "1,New Name,merge.example.test:8073,True\r\n");
        const KiwiSdrCsvImportResult result = manager.importProfilesCsv(csv);
        if (!result.ok() || result.addedCount != 0 || result.mergedCount != 1) {
            return fail("importing a matching endpoint did not merge in place");
        }
        if (manager.profiles().size() != 1) {
            return fail("merge created a duplicate instead of updating in place");
        }
        const KiwiSdrAntennaProfile updated = manager.profile(id);
        if (updated.id != id) {
            return fail("merge did not preserve the existing profile's id");
        }
        if (updated.name != QStringLiteral("New Name") || !updated.autoConnect) {
            return fail("merge did not apply the imported fields");
        }
    }

    // Out-of-range waterfall values in a hand-edited CSV are clamped, not
    // rejected — the same behaviour as typing them into the UI dialog.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const QByteArray csv = QByteArrayLiteral(
            "FORMAT_VERSION,NAME,ENDPOINT,WATERFALL_MIN_DBM,WATERFALL_MAX_DBM,WATERFALL_RATE\r\n"
            "1,Clamped,clamp.example.test:8073,-9999,9999,99\r\n");
        const KiwiSdrCsvImportResult result = manager.importProfilesCsv(csv);
        if (!result.ok() || result.addedCount != 1) {
            return fail("a valid row with out-of-range numeric fields was rejected");
        }
        const KiwiSdrAntennaProfile p = manager.profiles().constFirst();
        if (p.waterfallMinDbm < -260 || p.waterfallMaxDbm > 30
            || p.waterfallRate > 4 || p.waterfallRate < 0) {
            return fail("out-of-range waterfall values were not clamped on import");
        }
    }

    // A row missing a required field fails with a line-numbered error and
    // does not add a partial profile.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const QByteArray csv = QByteArrayLiteral(
            "FORMAT_VERSION,NAME,ENDPOINT\r\n"
            "1,,missing-name.example.test:8073\r\n");
        const KiwiSdrCsvImportResult result = manager.importProfilesCsv(csv);
        if (result.ok() || !manager.profiles().isEmpty()) {
            return fail("a row missing NAME should fail without adding a profile");
        }
    }

    // Passwords live in IKiwiSdrCredentialStore, never in the exported CSV.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const QString id =
            manager.addProfile("Secret", "secret.example.test:8073");
        manager.setProfilePassword(id, "hunter2");
        const QByteArray csv = manager.exportProfilesCsv();
        if (csv.contains("hunter2")) {
            return fail("exported CSV leaked a receiver password");
        }
    }

    std::printf("All tests passed.\n");
    return 0;
}
