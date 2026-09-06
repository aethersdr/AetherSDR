#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LocalMemoryStore.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR::icom {
struct IcomCivBackendTestAccess {
    static void prepare(IcomCivBackend& backend, std::uint8_t address = 0xB6)
    {
        backend.m_model = modelForCivAddress(address);
        // Never start the session: command construction only, no sockets.
        backend.m_session = std::make_unique<IcomSession>();
    }
    static bool dataMode(const IcomCivBackend& backend) { return backend.m_dataMode; }
    static void setImportSource(IcomCivBackend& backend)
    {
        backend.m_memoryImportSource = QStringLiteral("icom:ordering-test");
    }
    static void injectFinalMemory(IcomCivBackend& backend, const CivFrame& frame)
    {
        setImportSource(backend);
        backend.m_memoryRefreshActive = true;
        backend.m_memoryRefreshReplies.clear();
        backend.m_memoryRefreshTotal = 1;
        emit backend.memoryRefreshStarted(1);
        // Feed the frame handler directly. The inert session is never started,
        // no scheduler work is queued, and the connection flag is reset before
        // teardown so there is no simulated transport or keying operation.
        backend.m_connected = true;
        backend.onCivFrame(frame, backend.m_sessionGeneration);
        backend.m_connected = false;
    }
};
}

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void seedBank(const QMap<int, MemoryEntry>& entries = {}, int version = 1,
              const QString& savedAt = {})
{
    QJsonObject doc = QJsonDocument::fromJson(
        LocalMemoryStore::serialize(entries, savedAt)).object();
    doc["version"] = version;
    check(AppSettings::instance().setRadioFeature("local", "", "MemoryBank", version, doc),
          "isolated bank seeded");
}

void testModeEdits()
{
    seedBank();
    RadioModel model;
    MemoryDelta imported;
    imported.index = 0;
    imported.importSource = QStringLiteral("icom:mode-edit");
    imported.importKey = QStringLiteral("-1:42");
    imported.mode = QStringLiteral("DIGU");
    imported.dataMode = 1;
    imported.nativeFilter = 2;
    imported.freq = 14.074;
    imported.offsetDir = QStringLiteral("simplex");
    imported.toneMode = QStringLiteral("off");
    model.backend()->memoryChanged(imported);
    for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("DIGU"),
                               QStringLiteral("LSB"), QStringLiteral("DIGL"),
                               QStringLiteral("FM"), QStringLiteral("DFM")}) {
        model.sendCmdPublic(QStringLiteral("memory set 0 mode=%1").arg(mode), {});
        const MemoryEntry edited = model.memories().value(0);
        const bool expectedData = mode == "DIGU" || mode == "DIGL" || mode == "DFM";
        check((edited.dataMode != 0) == expectedData,
              "local mode edit keeps the stored native DATA flag consistent");
        icom::IcomCivBackend backend;
        icom::IcomCivBackendTestAccess::prepare(backend);
        backend.setSliceMode(0, edited.mode);
        MemoryRecallDetails details;
        details.filterPreset = edited.nativeFilter;
        details.dataMode = edited.dataMode != 0;
        details.direction = edited.offsetDir;
        details.toneMode = edited.toneMode;
        check(backend.applyMemoryRecallDetails(details), "edited native recall details accepted");
        check(icom::IcomCivBackendTestAccess::dataMode(backend) == expectedData,
              "native details do not undo the operator's edited mode");
    }
    model.localMemoryBank().flush();
    const auto saved = LocalMemoryStore::parse(QJsonDocument(
        AppSettings::instance().radioFeatureExact("local", "", "MemoryBank")).toJson());
    check(saved.memories.value(0).mode == "DFM" && saved.memories.value(0).dataMode == 1,
          "edited native mode and DATA flag persist together");
    model.sendCmdPublic("memory create", {});
    model.sendCmdPublic("memory set 1 mode=DIGU", {});
    check(model.memories().value(1).dataMode == 0,
          "ordinary local memories do not acquire unnecessary native schema fields");
}

void testRefreshResults()
{
    for (const int scenario : {0, 1, 2, 3}) {
        QMap<int, MemoryEntry> entries;
        if (scenario == 1) {
            for (int i = 0; i < LocalMemoryBank::kMaxSlots; ++i) {
                MemoryEntry entry;
                entry.index = i;
                entries.insert(i, entry);
            }
        }
        seedBank(entries, scenario == 2 ? 99 : 1);
        RadioModel model;
        model.publishLocalMemories();
        bool finished = false;
        bool success = false;
        int completed = -1;
        int warnings = 0;
        QObject::connect(&model, &RadioModel::configurationWarning, &model,
                         [&warnings](const QString&) { ++warnings; });
        QObject::connect(&model, &RadioModel::memoryRefreshFinished, &model,
                         [&](bool ok, int count, int) {
            finished = true;
            success = ok;
            completed = count;
            if (scenario == 0 && ok) {
                const auto saved = LocalMemoryStore::parse(QJsonDocument(
                    AppSettings::instance().radioFeatureExact("local", "", "MemoryBank")).toJson());
                check(saved.memories.size() == 1,
                      "success is emitted only after the final row is saved");
            }
        });
        model.backend()->memoryRefreshStarted(1);
        MemoryDelta imported;
        imported.index = 0;
        imported.importSource = QStringLiteral("icom:refresh-result");
        imported.importKey = QStringLiteral("-1:42");
        imported.mode = QStringLiteral("USB");
        imported.freq = 14.25;
        model.backend()->memoryChanged(imported);
        if (scenario == 3) {
            seedBank({}, 1, QStringLiteral("foreign-writer"));
        }
        model.backend()->memoryRefreshProgress(1, 1);
        model.backend()->memoryRefreshFinished(true, 1, 1);
        check(finished && success == (scenario == 0),
              "full, newer-schema and foreign-write refusals cannot report Sync success");
        if (scenario != 0) {
            check(warnings > 0 && completed == 0,
                  "refused Sync reports a warning and no stored slots");
        }
    }
}

void testDisconnectedRecall()
{
    seedBank();
    RadioModel model;
    check(model.automationApplySliceFixture(0, QStringLiteral("A"), nullptr),
          "disconnected-only slice fixture accepted");
    SliceModel* slice = model.slice(0);
    check(slice != nullptr, "disconnected slice fixture created through the model seam");
    if (!slice) {
        return;
    }
    slice->setMode(QStringLiteral("USB"));
    slice->setFrequency(14.25);
    MemoryDelta imported;
    imported.index = 0;
    imported.importSource = QStringLiteral("icom:offline");
    imported.importKey = QStringLiteral("-1:42");
    imported.freq = 7.05;
    imported.mode = QStringLiteral("CWL");
    imported.nativeFilter = 2;
    imported.offsetDir = QStringLiteral("simplex");
    imported.toneMode = QStringLiteral("off");
    model.backend()->memoryChanged(imported);
    int result = -1;
    model.sendCmdPublic("memory apply 0", [&](int code, const QString&) { result = code; });
    QCoreApplication::processEvents();
    check(result > 0, "disconnected native recall returns a refusal");
    check(slice->frequency() == 14.25 && slice->mode() == "USB",
          "disconnected native recall refuses before changing the slice");
}

void testIcomMemoryCapabilities()
{
    for (const std::uint8_t address : {0xA2, 0xA4, 0xB6}) {
        icom::IcomCivBackend backend;
        icom::IcomCivBackendTestAccess::prepare(backend, address);
        const RadioCapabilities caps = backend.capabilities();
        check(!caps.persistsMemories && !caps.canWriteMemories && caps.canRefreshMemories,
              "IC-9700, IC-705 and IC-7300MK2 use the client bank plus explicit Sync");
    }
}

void testFinalReplyOrdering()
{
    seedBank();
    RadioModel model;
    icom::IcomCivBackend backend;
    icom::IcomCivBackendTestAccess::prepare(backend);
    QObject::connect(&backend, &IRadioBackend::memoryRefreshStarted,
                     model.backend(), &IRadioBackend::memoryRefreshStarted);
    QObject::connect(&backend, &IRadioBackend::memoryChanged,
                     model.backend(), &IRadioBackend::memoryChanged);
    QObject::connect(&backend, &IRadioBackend::memoryRefreshFinished,
                     model.backend(), &IRadioBackend::memoryRefreshFinished);
    int expectedRows = 1;
    int finishes = 0;
    QObject::connect(&model, &RadioModel::memoryRefreshFinished, &model,
                     [&](bool success, int completed, int total) {
        ++finishes;
        check(success && completed == 1 && total == 1, "final reply completes its sweep");
        const auto saved = LocalMemoryStore::parse(QJsonDocument(
            AppSettings::instance().radioFeatureExact("local", "", "MemoryBank")).toJson());
        check(saved.memories.size() == expectedRows,
              "occupied and empty final native replies are saved before completion");
    });
    // IC-7300MK2 documented 47-byte record, ordinary USB/FIL2, channel 42.
    // This is decoder input, never a peer or an on-wire exchange.
    icom::CivFrame frame;
    frame.to = icom::kControllerAddress;
    frame.from = 0xB6;
    frame.cmd = 0x1A;
    frame.hasSub = true;
    frame.sub = 0x00;
    frame.data.resize(47, 0);
    frame.data[1] = 0x42;
    const std::vector<std::uint8_t> freq = icom::encodeFreq(14'250'000);
    std::copy(freq.begin(), freq.end(), frame.data.begin() + 3);
    frame.data[8] = 0x01;
    frame.data[9] = 0x02;
    frame.data[12] = 0x10;
    frame.data[15] = 0x10;
    icom::IcomCivBackendTestAccess::injectFinalMemory(backend, frame);
    expectedRows = 0;
    frame.data = {0x00, 0x42, 0xFF};
    icom::IcomCivBackendTestAccess::injectFinalMemory(backend, frame);
    check(finishes == 2, "both real decoder paths completed");
}

void testRefreshGuards()
{
    icom::IcomCivBackend backend;
    icom::IcomCivBackendTestAccess::prepare(backend, 0xA4);
    int warnings = 0;
    int started = 0;
    QObject::connect(&backend, &IRadioBackend::configurationWarning, &backend,
                     [&](const QString&) { ++warnings; });
    QObject::connect(&backend, &IRadioBackend::memoryRefreshStarted, &backend,
                     [&](int total) { started = total; });
    const QString nativeGroup = backend.capabilities().memoryGroups.value(1);
    check(!nativeGroup.isEmpty(), "IC-705 advertises a native group");
    backend.refreshMemories(nativeGroup);
    check(warnings == 1 && started == 0, "missing stable identity visibly refuses Sync");
    icom::IcomCivBackendTestAccess::setImportSource(backend);
    backend.refreshMemories(QStringLiteral("not a native group"));
    check(warnings == 2 && started == 0, "invalid native group visibly refuses Sync");
    // Scheduler dispatch refuses transport work while disconnected; group
    // resolution and request counting still run on the inert session.
    backend.refreshMemories(QStringLiteral(" %1 ").arg(nativeGroup.toLower()));
    check(warnings == 2 && started == 100,
          "backend accepts the same trimmed case-insensitive native group as the UI");
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
    testModeEdits();
    testRefreshResults();
    testDisconnectedRecall();
    testIcomMemoryCapabilities();
    testFinalReplyOrdering();
    testRefreshGuards();
    if (failures == 0) {
        std::printf("memory_import_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
