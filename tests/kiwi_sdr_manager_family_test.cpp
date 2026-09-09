#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrManager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

constexpr const char* kProfilesKey = "KiwiSdrRxAntennas";

// Family persistence never touches passwords, so a fully no-op store is
// enough here (same rationale as kiwi_sdr_manager_csv_test.cpp).
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

void seedProfilesJson(const QString& json)
{
    AppSettings::instance().setValue(QString::fromLatin1(kProfilesKey), json);
    AppSettings::instance().save();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-kiwi-family-test"));
    if (!settingsProfile.isValid()) {
        return fail("could not create isolated settings home");
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    clearProfiles();

    // Legacy settings JSON carries no "family" key: every profile loads as
    // Kiwi so existing installs keep their behaviour after the upgrade.
    {
        seedProfilesJson(QStringLiteral(
            R"({"profiles":[{"id":"legacy-1","name":"Legacy",)"
            R"("endpoint":"kiwi.example.test:8073","autoConnect":false}]})"));
        KiwiSdrManager manager(nullptr, makeStore());
        if (manager.profiles().size() != 1) {
            return fail("legacy JSON did not load exactly one profile");
        }
        if (manager.profiles().constFirst().family
            != KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi) {
            return fail("legacy profile without a family key did not default to Kiwi");
        }
    }

    // A Web-888 family survives a save/load round trip, and an unknown family
    // id falls back to Kiwi rather than failing the load.
    {
        seedProfilesJson(QStringLiteral(
            R"({"profiles":[{"id":"fork-1","name":"Fork",)"
            R"("endpoint":"web888.example.test:8074","family":"web888"},)"
            R"({"id":"odd-1","name":"Odd",)"
            R"("endpoint":"other.example.test:8073","family":"nonsense"}]})"));
        KiwiSdrManager manager(nullptr, makeStore());
        if (manager.profiles().size() != 2) {
            return fail("family JSON did not load exactly two profiles");
        }
        if (manager.profiles().at(0).family
            != KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888) {
            return fail("family=web888 did not load as Web-888");
        }
        if (manager.profiles().at(1).family
            != KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi) {
            return fail("unknown family id did not fall back to Kiwi");
        }
        // saveSettings() is private; a public no-op update persists the
        // profiles so the round trip below reads what was written.
        KiwiSdrAntennaProfile touch = manager.profile(QStringLiteral("fork-1"));
        manager.updateProfile(touch);
    }
    {
        KiwiSdrManager reloaded(nullptr, makeStore());
        const QVector<KiwiSdrAntennaProfile> profiles = reloaded.profiles();
        if (profiles.size() != 2
            || profiles.at(0).family
                != KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888
            || profiles.at(1).family
                != KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi) {
            return fail("save/load round trip did not preserve receiver families");
        }
    }

    // addProfile takes the family, updateProfile applies a family change, and
    // the CSV round trip carries RECEIVER_TYPE both ways.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const QString kiwiId = manager.addProfile("Kiwi RX", "kiwi.example.test:8073");
        const QString forkId = manager.addProfile(
            "Fork RX", "web888.example.test:8074",
            KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888);
        if (manager.profile(forkId).family
            != KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888
            || manager.profile(kiwiId).family
                != KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi) {
            return fail("addProfile did not record the requested receiver family");
        }

        KiwiSdrAntennaProfile flipped = manager.profile(kiwiId);
        flipped.family = KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888;
        manager.updateProfile(flipped);
        if (manager.profile(kiwiId).family
            != KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888) {
            return fail("updateProfile did not apply a receiver-family change");
        }
        // Flip back so the export below carries one row of each family.
        flipped.family = KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi;
        manager.updateProfile(flipped);

        const QByteArray csv = manager.exportProfilesCsv();
        if (!csv.contains("WEB888") || !csv.contains("KIWI")) {
            return fail("exported CSV did not carry RECEIVER_TYPE values");
        }
        clearProfiles();
        KiwiSdrManager importer(nullptr, makeStore());
        const KiwiSdrCsvImportResult result = importer.importProfilesCsv(csv);
        if (!result.ok() || result.addedCount != 2) {
            return fail("family CSV round-trip import failed");
        }
        for (const KiwiSdrAntennaProfile& p : importer.profiles()) {
            const bool expectWeb888 = p.endpoint.contains(
                QStringLiteral("web888.example.test"));
            const auto expected = expectWeb888
                ? KiwiSdrProtocol::KiwiSdrReceiverFamily::Web888
                : KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi;
            if (p.family != expected) {
                return fail("CSV import did not preserve the receiver family");
            }
        }
    }

    // An unrecognized non-empty RECEIVER_TYPE is an import error, not a
    // silent Kiwi fallback; a row without the column still imports as Kiwi.
    {
        clearProfiles();
        KiwiSdrManager manager(nullptr, makeStore());
        const KiwiSdrCsvImportResult bad = manager.importProfilesCsv(
            QByteArrayLiteral(
                "FORMAT_VERSION,NAME,ENDPOINT,RECEIVER_TYPE\r\n"
                "1,Typo,typo.example.test:8073,WEB88\r\n"));
        if (bad.ok() || !manager.profiles().isEmpty()) {
            return fail("an unrecognized RECEIVER_TYPE should fail the import");
        }
        const KiwiSdrCsvImportResult legacy = manager.importProfilesCsv(
            QByteArrayLiteral(
                "FORMAT_VERSION,NAME,ENDPOINT\r\n"
                "1,Legacy,legacy.example.test:8073\r\n"));
        if (!legacy.ok() || legacy.addedCount != 1
            || manager.profiles().constFirst().family
                != KiwiSdrProtocol::KiwiSdrReceiverFamily::Kiwi) {
            return fail("a CSV without RECEIVER_TYPE did not import as Kiwi");
        }
    }

    std::printf("All tests passed.\n");
    return 0;
}