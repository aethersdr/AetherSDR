#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LocalMemoryStore.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("memory-import-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    // Seed an exact empty bank so no host-side legacy import is consulted.
    const QJsonObject empty = QJsonDocument::fromJson(LocalMemoryStore::serialize({})).object();
    check(AppSettings::instance().setRadioFeature("local", "", "MemoryBank", 1, empty),
          "isolated bank seeded");

    MemoryDelta imported;
    imported.index = 0; // Deliberately collides with the manual client slot.
    imported.importSource = QStringLiteral("icom:radio-a");
    imported.importKey = QStringLiteral("-1:42");
    imported.name = QStringLiteral("Native name");
    imported.owner = QStringLiteral("IC-7300MK2");
    imported.group = QStringLiteral("Native group");
    imported.mode = QStringLiteral("DIGU");
    imported.freq = 14.074;
    imported.nativeFilter = 2;
    imported.dataMode = 1;
    imported.recallable = false;
    int importedIndex = -1;
    {
        RadioModel model;
        // Inject the real backend signal. No connectRadio, sockets or peer.
        model.sendCmdPublic("memory create", {});
        model.sendCmdPublic("memory set 0 name=Manual freq=7.2 mode=LSB", {});
        model.backend()->memoryChanged(imported);
        check(model.memories().size() == 2 && model.memories().value(0).name == "Manual",
              "native slot collision allocates a new row and preserves the manual row");
        for (const MemoryEntry& memory : model.memories()) {
            if (memory.importSource == *imported.importSource) {
                importedIndex = memory.index;
            }
        }
        check(importedIndex == 1, "import uses the lowest free client slot");
        model.sendCmdPublic(QString("memory set %1 name=Operator owner=Trustee group=Local")
                                .arg(importedIndex), {});
        // Include stripped control bytes: lookup and persistence must normalize
        // identically, or the next Sync allocates a duplicate forever.
        imported.importSource->append(QChar(1));
        imported.importKey->append(QChar(0x7f));
        imported.freq = 14.25;
        imported.mode = QStringLiteral("CWL");
        imported.dataMode = 0;
        imported.recallable = true;
        model.backend()->memoryChanged(imported);
        const MemoryEntry updated = model.memories().value(importedIndex);
        check(model.memories().size() == 2 && updated.freq == 14.25,
              "resync updates the existing row instead of duplicating it");
        check(updated.name == "Operator" && updated.owner == "Trustee" && updated.group == "Local",
              "resync preserves operator annotations");
        check(updated.mode == "CWL" && updated.recallable,
              "explicit sync repairs an ordinary CW-R row using the codec decision");
        check(updated.importSource == "icom:radio-a" && updated.importKey == "-1:42",
              "stored provenance matches the normalized lookup");

        MemoryDelta invalid = imported;
        invalid.importSource = QString(QChar(1));
        invalid.index = 0;
        invalid.removed = true;
        model.backend()->memoryChanged(invalid);
        check(model.memories().contains(0) && model.memories().size() == 2,
              "empty normalized identity cannot fall through to a native-slot removal");

        MemoryDelta other = imported;
        other.importSource = QStringLiteral("icom:radio-b");
        model.backend()->memoryChanged(other);
        check(model.memories().size() == 3,
              "same native channel on another radio gets a separate slot");
        other.removed = true;
        model.backend()->memoryChanged(other);
        model.backend()->memoryChanged(other);
        check(model.memories().size() == 2 && model.memories().contains(importedIndex),
              "empty channel removes only its matching source and is idempotent");
    }
    {
        // Resync BEFORE the constructor's queued local-bank publication: the
        // authoritative persisted annotations must survive an empty GUI cache.
        RadioModel reopened;
        imported.freq = 14.3;
        reopened.backend()->memoryChanged(imported);
        check(reopened.memories().value(importedIndex).name == "Operator"
                  && reopened.memories().value(importedIndex).freq == 14.3,
              "resync after restart reuses persisted identity and annotations");
        imported.removed = true;
        reopened.backend()->memoryChanged(imported);
        check(!reopened.memories().contains(importedIndex),
              "cleared native channel removes the imported cache row");
    }
    const QJsonObject saved = AppSettings::instance().radioFeatureExact("local", "", "MemoryBank");
    const auto parsed = LocalMemoryStore::parse(QJsonDocument(saved).toJson());
    check(parsed.memories.size() == 1 && parsed.memories.value(0).name == "Manual",
          "deletion persists and manual row survives restart");
    if (failures == 0) {
        std::printf("memory_import_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
