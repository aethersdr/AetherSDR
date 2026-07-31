#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LocalMemoryBank.h"
#include "core/LocalMemoryStore.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

// The bank's ONE shared document (RFC #4603 PR 6). Cases used to isolate via
// per-case file paths; now they isolate by resetting the document.
QJsonObject bankDocument()
{
    return AppSettings::instance().radioFeatureExact(
        LocalMemoryStore::documentFamily(), QString(),
        LocalMemoryStore::documentFeature());
}

void resetBankDocument()
{
    AppSettings::instance().removeRadioFeature(
        LocalMemoryStore::documentFamily(), QString(),
        LocalMemoryStore::documentFeature());
}

// A foreign process's write, simulated at the same layer it would really
// happen: a whole-envelope document replace.
bool writeForeignDocument(const QMap<int, MemoryEntry>& entries,
                          const QString& savedAtIso)
{
    const QJsonObject envelope =
        QJsonDocument::fromJson(LocalMemoryStore::serialize(entries, savedAtIso))
            .object();
    return AppSettings::instance().setRadioFeature(
        LocalMemoryStore::documentFamily(), QString(),
        LocalMemoryStore::documentFeature(), LocalMemoryStore::kFormatVersion,
        envelope);
}

// The kv tail the client actually builds (MemoryCommands::buildMemoryUpdateData):
// space-encoded free text, so splitting the tail on spaces is unambiguous.
QString setTail(int index, const QString& name, double freqMhz, const QString& mode)
{
    return QString("memory set %1 group=Default owner=KI6BCJ freq=%2 name=%3 mode=%4 step=100")
        .arg(index)
        .arg(freqMhz, 0, 'f', 6)
        .arg(QString(name).replace(' ', QChar(0x7f)))
        .arg(mode);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-local-memory-bank-test"));
    if (!profile.isValid()) {
        std::cerr << "FAIL: create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    bool ok = true;

    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temp dir created");
    const QString path = dir.path() + "/memories.json";

    // --- slot allocation --------------------------------------------------
    {
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(path);

        const auto first = bank.handleCommand("memory create");
        ok &= expect(first.handled, "create is handled");
        ok &= expect(first.code == 0, "create succeeds");
        ok &= expect(first.body == "0", "the first slot is 0");
        ok &= expect(first.delta.has_value() && first.delta->index == 0,
                     "create yields a delta for the new slot");

        const auto second = bank.handleCommand("memory create");
        ok &= expect(second.body == "1", "the second slot is 1");

        // Freeing a hole and re-creating must reuse it rather than grow the
        // numbering — the slot number is what the browse panel shows.
        const auto removed = bank.handleCommand("memory remove 0");
        ok &= expect(removed.handled && removed.code == 0, "remove succeeds");
        ok &= expect(removed.delta.has_value() && removed.delta->removed,
                     "remove yields a removal delta");
        ok &= expect(bank.handleCommand("memory create").body == "0",
                     "the freed slot is reused");
    }

    // --- set / apply ------------------------------------------------------
    {
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/set.json");
        bank.handleCommand("memory create");

        const auto set = bank.handleCommand(setTail(0, "Bay Net", 14.074, "DIGU"));
        ok &= expect(set.handled && set.code == 0, "set on an existing slot succeeds");
        ok &= expect(set.delta.has_value(), "set yields a delta");
        ok &= expect(set.delta->freq.has_value() && *set.delta->freq == 14.074,
                     "the delta carries the frequency");
        ok &= expect(set.delta->mode.value_or(QString()) == "DIGU",
                     "the delta carries the mode");
        // Text rides raw — the 0x7f decode belongs to RadioModel, not here.
        ok &= expect(set.delta->name.value_or(QString()).contains(QChar(0x7f)),
                     "free text is carried wire-encoded, for the model to decode");

        const auto apply = bank.handleCommand("memory apply 0");
        ok &= expect(apply.handled && apply.code == 0, "apply on an existing slot succeeds");
        ok &= expect(apply.recallIndex == 0, "apply asks the caller to recall slot 0");
        ok &= expect(!apply.delta.has_value(), "apply changes no stored state");
    }

    // --- rejections -------------------------------------------------------
    {
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/reject.json");

        const auto missingSet = bank.handleCommand("memory set 5 freq=7.200000");
        ok &= expect(missingSet.handled, "a set for a missing slot is still handled");
        ok &= expect(missingSet.code != 0, "a set for a missing slot fails");
        ok &= expect(!missingSet.body.isEmpty(), "the failure carries a reason");

        ok &= expect(bank.handleCommand("memory apply 5").code != 0,
                     "an apply for a missing slot fails");
        ok &= expect(bank.handleCommand("memory set x freq=7.2").code != 0,
                     "a non-numeric id fails");

        bank.handleCommand("memory create");
        ok &= expect(bank.handleCommand("memory set 0").code != 0,
                     "a set with no fields fails");

        // Removing something already gone is NOT an error: the dialog's
        // create→set→remove cleanup can land here after the slot went, and
        // failing it would report a cleanup problem that does not exist.
        ok &= expect(bank.handleCommand("memory remove 42").code == 0,
                     "removing a missing slot succeeds");
    }

    // --- commands the bank does not own are left alone --------------------
    {
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/passthrough.json");
        ok &= expect(!bank.handleCommand("memory list").handled,
                     "an unknown memory verb is not answered");
        ok &= expect(!bank.handleCommand("slice tune 0 14.074000").handled,
                     "a non-memory command is not answered");
    }

    // --- persistence across bank instances (through the DOCUMENT) ---------
    {
        resetBankDocument();
        const QString persistPath = dir.path() + "/persist.json";
        MemoryEntry stored;
        stored.freq = 7.200;
        stored.name = "Dummy Load";   // decoded form, as RadioModel records it
        stored.mode = "LSB";

        {
            LocalMemoryBank bank;
            bank.setFilePath(persistPath);
            bank.handleCommand("memory create");
            bank.record(0, stored);
            bank.flush();
        }

        ok &= expect(!bankDocument().isEmpty(), "flush wrote the bank document");
        ok &= expect(!QFile::exists(persistPath),
                     "flush writes the settings store, never a file");

        LocalMemoryBank reopened;
        reopened.setFilePath(persistPath);
        reopened.load();
        ok &= expect(reopened.entries().size() == 1, "the reopened bank has the slot");
        const MemoryEntry& back = reopened.entries().value(0);
        ok &= expect(back.freq == 7.200 && back.name == "Dummy Load" && back.mode == "LSB",
                     "the stored channel survives a reopen");
        ok &= expect(back.index == 0, "the slot index survives a reopen");

        // A reopened bank must allocate AROUND what it loaded, not on top of it.
        ok &= expect(reopened.handleCommand("memory create").body == "1",
                     "create allocates past the loaded slots");
    }

    // --- a bank this build cannot read is never overwritten ----------------
    // Variant 1: the LEGACY FILE is too new — the import refuses and the
    // bank is read-only, exactly as the file-era behavior was.
    {
        resetBankDocument();
        const QString futurePath = dir.path() + "/future.json";
        const QByteArray future =
            R"({"format":"aether.memories","version":999,
                "memories":[{"index":0,"freq":7.2,"name":"Precious"}]})";
        {
            QFile f(futurePath);
            ok &= expect(f.open(QIODevice::WriteOnly), "future-version fixture written");
            f.write(future);
        }

        LocalMemoryBank bank;
        bank.setFilePath(futurePath);
        bank.load();
        ok &= expect(bank.entries().isEmpty(), "a too-new bank reads as empty");
        // The read WAS attempted (so nothing re-reads and thrashes m_entries);
        // what it is not, is writable. These are two facts and used to be one
        // flag, which is what let a create/set pair fail with "no memory in
        // slot 0" on this fixture.
        ok &= expect(bank.isLoaded(), "a too-new bank counts as read");
        ok &= expect(!bank.isWritable(), "a too-new bank is not writable");
        ok &= expect(!bank.lastError().isEmpty(), "a too-new bank reports why");

        // A create/set PAIR — the shape every Add and CSV-import row takes. The
        // second command must not find an empty bank because the first
        // command's load() re-ran and cleared it.
        const auto created = bank.handleCommand("memory create");
        ok &= expect(created.handled, "an unreadable bank still answers");
        ok &= expect(created.code != 0, "create is refused, not silently accepted");
        ok &= expect(created.body.contains("could not be read"),
                     "the refusal explains that the file is unreadable");
        ok &= expect(!created.body.contains("no memory in slot"),
                     "the refusal is not the misleading empty-slot error");

        // An edit made against it must not be written anywhere — that would
        // replace channels this build cannot represent with the empty bank it
        // read (as a document, since files are never written now).
        bank.record(0, MemoryEntry{});
        bank.flush();

        QFile f(futurePath);
        ok &= expect(f.open(QIODevice::ReadOnly), "the fixture is still readable");
        ok &= expect(f.readAll() == future, "the unreadable bank was left untouched");
        ok &= expect(bankDocument().isEmpty(),
                     "no document is created from a bank this build cannot read");
    }

    // Variant 3 (nigelfenton's probe, PR #4623): the ROW version is newer
    // while the envelope body is current — the divergent pair a future build
    // could write. Must be read-only, exactly like the body-version case.
    {
        resetBankDocument();
        QMap<int, MemoryEntry> precious;
        {
            MemoryEntry e;
            e.index = 0;
            e.freq = 7.2;
            e.name = "Precious";
            precious.insert(0, e);
        }
        const QJsonObject validV1Envelope =
            QJsonDocument::fromJson(
                LocalMemoryStore::serialize(precious, "2026-07-01T00:00:00Z"))
                .object();
        ok &= expect(AppSettings::instance().setRadioFeature(
                         LocalMemoryStore::documentFamily(), QString(),
                         LocalMemoryStore::documentFeature(), 999,
                         validV1Envelope),
                     "a row=999/body=1 divergent document is planted");

        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/unused-row-version.json");
        bank.load();
        ok &= expect(!bank.isWritable(),
                     "a newer ROW version is read-only even with a current "
                     "envelope body");
        bank.record(5, MemoryEntry{});
        bank.flush();
        ok &= expect(bankDocument()
                             .value(QStringLiteral("memories"))
                             .toArray()
                             .size()
                         == 1,
                     "the divergent document survives the refused edit — "
                     "'Precious' is not overwritten");
        resetBankDocument();
    }

    // Variant 2 (document era): the DOCUMENT is too new — same refusal, and
    // the newer document survives byte-for-byte.
    {
        resetBankDocument();
        QJsonObject futureDoc =
            QJsonDocument::fromJson(
                R"({"format":"aether.memories","version":999,
                    "memories":[{"index":0,"freq":7.2,"name":"Precious"}]})")
                .object();
        ok &= expect(AppSettings::instance().setRadioFeature(
                         LocalMemoryStore::documentFamily(), QString(),
                         LocalMemoryStore::documentFeature(), 999, futureDoc),
                     "a future-version document is planted");

        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/unused-legacy.json");
        bank.load();
        ok &= expect(bank.entries().isEmpty(), "a too-new document reads as empty");
        ok &= expect(!bank.isWritable(), "a too-new document is not writable");
        bank.record(0, MemoryEntry{});
        bank.flush();
        ok &= expect(bankDocument()
                             .value(QStringLiteral("memories"))
                             .toArray()
                             .size()
                         == 1,
                     "the newer document survives the refused edit intact");
        resetBankDocument();
    }

    // --- record() skips a genuine no-op ------------------------------------
    {
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/noop.json");
        bank.handleCommand("memory create");

        MemoryEntry entry;
        entry.freq = 21.074;
        bank.record(0, entry);
        bank.flush();
        ok &= expect(!bankDocument().isEmpty(), "the first record saved");

        // The envelope carries a savedAt stamp, so an unnecessary rewrite is
        // visible even when the entry content would be identical.
        const QJsonObject afterFirstSave = bankDocument();

        // Re-asserting the same values (what the dialog does via
        // handleMemoryStatus after every successful write) must not dirty it.
        bank.record(0, entry);
        bank.flush();
        ok &= expect(bankDocument() == afterFirstSave,
                     "re-recording identical values does not rewrite the document");

        // A real change still saves.
        entry.freq = 21.075;
        bank.record(0, entry);
        bank.flush();
        const QJsonObject afterChange = bankDocument();
        ok &= expect(afterChange != afterFirstSave,
                     "a changed value does rewrite the document");
        ok &= expect(QJsonDocument(afterChange).toJson().contains("21.075"),
                     "the rewrite carries the new value");
        ok &= expect(bank.lastError().isEmpty(),
                     "a legitimate consecutive save is not mistaken for a foreign write");
    }

    // --- a CSV-import-shaped run of many records --------------------------
    {
        // MemoryDialog's import is a create→set chain, re-entered per record.
        // Allocation has to keep up across the whole file: every record must
        // land in its own slot, in order, with none reused or skipped.
        resetBankDocument();
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/bulk.json");

        const int kRecords = 500;
        bool allOk = true;
        for (int i = 0; i < kRecords; ++i) {
            const auto created = bank.handleCommand("memory create");
            if (!created.handled || created.code != 0
                || created.body.toInt() != i) {
                allOk = false;
                break;
            }
            const auto set = bank.handleCommand(
                setTail(i, QString("Chan %1").arg(i), 14.0 + i * 0.001, "USB"));
            if (!set.handled || set.code != 0)
                allOk = false;
            // RadioModel records the decoded entry; stand in for it here.
            MemoryEntry entry;
            entry.index = i;
            entry.freq = 14.0 + i * 0.001;
            entry.name = QString("Chan %1").arg(i);
            entry.mode = "USB";
            bank.record(i, entry);
            if (!allOk)
                break;
        }
        ok &= expect(allOk, "500 import-shaped records allocate sequential slots");
        ok &= expect(bank.entries().size() == kRecords, "every record is in the bank");
        ok &= expect(bank.entries().value(499).name == "Chan 499",
                     "the last record survived");

        bank.flush();
        LocalMemoryBank reopened;
        reopened.setFilePath(dir.path() + "/bulk.json");
        reopened.load();
        ok &= expect(reopened.entries().size() == kRecords,
                     "the whole bulk bank round-trips through the document");
    }

    // --- an UNPARSEABLE bank is never overwritten --------------------------
    //
    // The version guard covered the unlikely case. "We could not read this file
    // at all" was unprotected: load() marked the bank writable anyway, so the
    // next flush() replaced a damaged file with the empty bank parsed out of it.
    {
        struct Case { const char* what; QByteArray bytes; };
        const QList<Case> cases = {
            {"truncated JSON",
             QByteArray(R"({"format":"aether.memories","version":1,"memories":[{"index":0,)")},
            {"a JSON array at the root",
             QByteArray(R"([{"index":0,"freq":7.2}])")},
            {"a foreign format id",
             QByteArray(R"({"format":"someone.else","version":1,
                            "memories":[{"index":0,"freq":7.2,"name":"Theirs"}]})")},
        };

        for (int ci = 0; ci < cases.size(); ++ci) {
            const Case& c = cases.at(ci);
            const QString p = dir.path() + QString("/damaged-%1.json").arg(ci);
            {
                QFile f(p);
                ok &= expect(f.open(QIODevice::WriteOnly), "damaged fixture written");
                f.write(c.bytes);
            }

            resetBankDocument();
            LocalMemoryBank bank;
            bank.setFilePath(p);
            bank.load();
            ok &= expect(!bank.isWritable(), c.what);
            // The foreign-format case also must not ADOPT the memories it parsed
            // out of somebody else's file.
            ok &= expect(bank.entries().isEmpty(),
                         "an unreadable bank exposes no channels");

            bank.record(0, MemoryEntry{});
            bank.flush();

            QFile f(p);
            ok &= expect(f.open(QIODevice::ReadOnly), "the damaged fixture still opens");
            ok &= expect(f.readAll() == c.bytes,
                         "a damaged bank is left byte-for-byte intact");
            ok &= expect(bankDocument().isEmpty(),
                         "no document is created from a damaged legacy file");
        }
    }

    // --- a concurrent writer is detected, not clobbered --------------------
    //
    // The bank is replaced wholesale and read once per process, so two
    // instances sharing a settings store would silently discard each other's
    // channels. The savedAt baseline turns that into a visible refusal.
    {
        resetBankDocument();
        LocalMemoryBank first;
        first.setFilePath(dir.path() + "/shared-unused.json");
        first.handleCommand("memory create");
        first.flush();
        ok &= expect(!bankDocument().isEmpty(), "the first instance saved");

        // A second process writes its own channels behind our back — at the
        // document layer, where it would really happen.
        QMap<int, MemoryEntry> theirsMap;
        {
            MemoryEntry e;
            e.index = 5;
            e.freq = 14.074;
            e.name = "Theirs";
            theirsMap.insert(5, e);
        }
        ok &= expect(writeForeignDocument(theirsMap, "2026-07-30T00:00:00.000Z"),
                     "the other instance wrote the shared bank");
        const QJsonObject theirs = bankDocument();

        // Our next edit must NOT replace their document.
        first.record(1, MemoryEntry{});
        first.flush();

        ok &= expect(bankDocument() == theirs,
                     "a foreign write is not clobbered by our flush");
        ok &= expect(first.lastError().contains("changed by another"),
                     "the refusal names the concurrent-write reason");

        // And our own save must re-baseline, or the very next flush would
        // mistake our own savedAt for somebody else's and refuse forever.
        LocalMemoryBank fresh;
        fresh.setFilePath(dir.path() + "/shared-unused.json");
        fresh.load();
        fresh.record(9, MemoryEntry{});
        fresh.flush();
        fresh.record(10, MemoryEntry{});
        fresh.flush();
        LocalMemoryBank reread;
        reread.setFilePath(dir.path() + "/shared-unused.json");
        reread.load();
        ok &= expect(reread.entries().contains(9) && reread.entries().contains(10),
                     "consecutive flushes both land after a clean load");
    }

    // --- the legacy file imports once and stays frozen ---------------------
    {
        resetBankDocument();
        const QString legacyPath = dir.path() + "/legacy-import.json";
        QMap<int, MemoryEntry> legacyMap;
        {
            MemoryEntry e;
            e.index = 3;
            e.freq = 3.573;
            e.name = "Legacy Net";
            legacyMap.insert(3, e);
        }
        const QByteArray legacyBytes =
            LocalMemoryStore::serialize(legacyMap, "2026-07-01T00:00:00Z");
        {
            QFile f(legacyPath);
            ok &= expect(f.open(QIODevice::WriteOnly), "legacy fixture written");
            f.write(legacyBytes);
        }

        LocalMemoryBank bank;
        bank.setFilePath(legacyPath);
        bank.load();
        ok &= expect(bank.entries().value(3).name == "Legacy Net",
                     "legacy channels import on first load");
        ok &= expect(!bankDocument().isEmpty(),
                     "the import lands in the document immediately (no edit "
                     "needed to claim it)");
        {
            QFile f(legacyPath);
            ok &= expect(f.open(QIODevice::ReadOnly)
                             && f.readAll() == legacyBytes,
                         "the legacy file stays byte-for-byte frozen");
        }

        // With the document in place, the legacy file is never consulted
        // again — even when it still exists.
        LocalMemoryBank again;
        again.setFilePath(legacyPath);
        again.load();
        again.record(7, MemoryEntry{});
        again.flush();
        LocalMemoryBank verify;
        verify.setFilePath(legacyPath);
        verify.load();
        ok &= expect(verify.entries().contains(3) && verify.entries().contains(7),
                     "post-import edits accumulate in the document, on top of "
                     "the imported channels");
    }

    if (ok)
        std::cout << "local_memory_bank_test: all checks passed\n";
    return ok ? 0 : 1;
}
