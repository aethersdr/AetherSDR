#include "core/LocalMemoryStore.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

MemoryEntry sampleMemory(int index)
{
    MemoryEntry m;
    m.index = index;
    m.group = "Local Repeaters";
    m.owner = "KI6BCJ";
    m.channel = "42";
    m.importSource = "icom:radio-serial";
    m.importKey = "-1:42";
    m.freq = 146.94;
    m.name = "W6ABC Mt Diablo";
    m.mode = "FM";
    m.nativeFilter = 2;
    m.dataMode = 1;
    m.step = 5000;
    m.offsetDir = "down";
    m.repeaterOffset = 0.6;
    m.toneMode = "ctcss_tx";
    m.toneValue = 103.5;
    m.rxToneValue = 88.5;
    m.dtcsCode = 23;
    m.dtcsTxReverse = true;
    m.dtcsRxReverse = false;
    m.recallable = true;
    m.squelch = true;
    m.squelchLevel = 20;
    m.rxFilterLow = -8000;
    m.rxFilterHigh = 8000;
    m.rttyMark = 2125;
    m.rttyShift = 170;
    m.diglOffset = 2210;
    m.diguOffset = 1500;
    return m;
}

}  // namespace

int main()
{
    bool ok = true;

    // --- round-trip -----------------------------------------------------
    {
        QMap<int, MemoryEntry> memories;
        memories.insert(0, sampleMemory(0));
        memories.insert(7, sampleMemory(7));

        const QByteArray json =
            LocalMemoryStore::serialize(memories, "2026-07-29T14:00:00Z");
        const auto result = LocalMemoryStore::parse(json);

        ok &= expect(result.ok(), "round-trip parses without errors");
        ok &= expect(result.version == LocalMemoryStore::kFormatVersion,
                     "version preserved");
        ok &= expect(result.memories.size() == 2, "both slots round-tripped");
        // The sparse index is the whole point: slot 7 must come back as 7.
        ok &= expect(result.memories.contains(7), "sparse slot index preserved");

        const MemoryEntry& g = result.memories.value(7);
        ok &= expect(g == sampleMemory(7), "every MemoryEntry field preserved");
    }

    // A version bump is needed only for fields older writers cannot preserve.
    {
        MemoryEntry ordinary;
        ordinary.index = 0;
        ordinary.mode = "USB";
        ordinary.freq = 14.25;
        QMap<int, MemoryEntry> bank{{0, ordinary}};
        ok &= expect(LocalMemoryStore::parse(LocalMemoryStore::serialize(bank)).version == 1,
                     "ordinary memories keep the version-1 format");
        bank[0].recallable = false;
        ok &= expect(LocalMemoryStore::parse(LocalMemoryStore::serialize(bank)).version == 2,
                     "display-only safety metadata requires the protected version-2 format");
        bank[0] = ordinary;
        bank[0].nativeFilter = 2;
        ok &= expect(LocalMemoryStore::formatVersionFor(bank) == 2,
                     "native recall metadata requires version 2 even without provenance");
    }

    // --- the map key wins over a disagreeing index field ------------------
    {
        QMap<int, MemoryEntry> memories;
        MemoryEntry stale = sampleMemory(3);
        stale.index = 99;   // stale field, e.g. a caller that renumbered
        memories.insert(3, stale);

        const auto result = LocalMemoryStore::parse(LocalMemoryStore::serialize(memories));
        ok &= expect(result.memories.contains(3), "serialize uses the map key");
        ok &= expect(result.memories.value(3).index == 3, "index field rewritten to the key");
    }

    // --- tolerant parsing ------------------------------------------------
    {
        // Unknown future field ignored; absent fields take MemoryEntry defaults.
        const QByteArray json = R"({
            "format": "aether.memories",
            "version": 1,
            "memories": [
                { "index": 2, "freq": 14.074, "mode": "DIGU", "futureField": 42 }
            ]
        })";
        const auto result = LocalMemoryStore::parse(json);
        ok &= expect(result.ok(), "tolerant parse of a minimal entry");
        ok &= expect(result.memories.size() == 1, "minimal entry parsed");
        const MemoryEntry& m = result.memories.value(2);
        ok &= expect(m.freq == 14.074 && m.mode == "DIGU", "stated fields parsed");
        ok &= expect(m.step == 100, "absent step takes the MemoryEntry default");
        ok &= expect(m.rttyMark == 2125, "absent rttyMark takes the MemoryEntry default");
    }

    // --- an empty file is an empty bank, not a corrupt one -----------------
    {
        ok &= expect(LocalMemoryStore::parse("").ok(), "empty bytes parse clean");
        ok &= expect(LocalMemoryStore::parse("   \n").ok(), "whitespace-only parses clean");
        ok &= expect(LocalMemoryStore::parse("").memories.isEmpty(), "empty bytes give no slots");
    }

    // --- error handling ----------------------------------------------------
    {
        ok &= expect(!LocalMemoryStore::parse("{ not json ").ok(),
                     "malformed JSON reports an error");
        ok &= expect(!LocalMemoryStore::parse("[1,2,3]").ok(),
                     "non-object top level reports an error");

        const QByteArray wrongFormat =
            R"({"format":"aether.netschedule","version":1,"memories":[]})";
        ok &= expect(!LocalMemoryStore::parse(wrongFormat).ok(),
                     "a different format id reports an error");

        // A newer version must fail the WHOLE read. Parsing what we understand
        // and saving over it would delete the fields we do not.
        const QByteArray future =
            R"({"format":"aether.memories","version":999,
                "memories":[{"index":0,"freq":7.2}]})";
        const auto futureResult = LocalMemoryStore::parse(future);
        ok &= expect(!futureResult.ok(), "a newer version reports an error");
        ok &= expect(futureResult.memories.isEmpty(),
                     "a newer version yields no slots rather than a partial read");
        ok &= expect(futureResult.version == 999, "the newer version is reported back");
    }

    // --- malformed rows are skipped, not silently renumbered ---------------
    {
        const QByteArray json = R"({
            "format": "aether.memories",
            "version": 1,
            "memories": [
                { "index": 0, "freq": 7.2 },
                { "freq": 14.2 },
                { "index": -1, "freq": 21.2 },
                { "index": 0, "freq": 28.2 }
            ]
        })";
        const auto result = LocalMemoryStore::parse(json);
        ok &= expect(!result.ok(), "skipped rows are reported");
        ok &= expect(result.memories.size() == 1, "only the valid row is kept");
        ok &= expect(result.memories.value(0).freq == 7.2,
                     "the duplicate does not overwrite the first slot");
    }

    // --- file load / save --------------------------------------------------
    {
        QTemporaryDir dir;
        ok &= expect(dir.isValid(), "temp dir created");
        const QString path = dir.path() + "/nested/memories.json";

        // A missing file is a first run, not a failure.
        const auto missing = LocalMemoryStore::load(path);
        ok &= expect(missing.ok(), "a missing file loads clean");
        ok &= expect(missing.memories.isEmpty(), "a missing file is an empty bank");

        QMap<int, MemoryEntry> memories;
        memories.insert(4, sampleMemory(4));
        QString error;
        ok &= expect(LocalMemoryStore::save(path, memories, "2026-07-29T14:00:00Z", &error),
                     "save succeeds and creates the directory");
        ok &= expect(error.isEmpty(), "a successful save clears the error");
        ok &= expect(QFile::exists(path), "the file is on disk");

        const auto reloaded = LocalMemoryStore::load(path);
        ok &= expect(reloaded.ok(), "reload parses clean");
        ok &= expect(reloaded.memories.value(4) == sampleMemory(4),
                     "the slot survives a save/load cycle");
    }

    // --- an unwritable path fails loudly rather than silently --------------
    {
        QString error;
        const bool saved = LocalMemoryStore::save(
            QString(), QMap<int, MemoryEntry>{}, QString(), &error);
        ok &= expect(!saved, "an empty path fails");
        ok &= expect(!error.isEmpty(), "the failure reports a reason");
    }

    if (ok)
        std::cout << "local_memory_store_test: all checks passed\n";
    return ok ? 0 : 1;
}
