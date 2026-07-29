#include "core/LocalMemoryBank.h"
#include "core/LocalMemoryStore.h"

#include <QCoreApplication>
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
    QCoreApplication app(argc, argv);
    bool ok = true;

    QTemporaryDir dir;
    ok &= expect(dir.isValid(), "temp dir created");
    const QString path = dir.path() + "/memories.json";

    // --- slot allocation --------------------------------------------------
    {
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
        LocalMemoryBank bank;
        bank.setFilePath(dir.path() + "/passthrough.json");
        ok &= expect(!bank.handleCommand("memory list").handled,
                     "an unknown memory verb is not answered");
        ok &= expect(!bank.handleCommand("slice tune 0 14.074000").handled,
                     "a non-memory command is not answered");
    }

    // --- persistence across bank instances --------------------------------
    {
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

        ok &= expect(QFile::exists(persistPath), "flush wrote the bank");

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
    {
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
        ok &= expect(!bank.isLoaded(), "a too-new bank is not marked loaded");
        ok &= expect(!bank.lastError().isEmpty(), "a too-new bank reports why");

        // An edit made against it must not be written back — that would replace
        // channels this build cannot represent with the empty bank it read.
        bank.record(0, MemoryEntry{});
        bank.flush();

        QFile f(futurePath);
        ok &= expect(f.open(QIODevice::ReadOnly), "the fixture is still readable");
        ok &= expect(f.readAll() == future, "the unreadable bank was left untouched");
    }

    // --- record() skips a genuine no-op ------------------------------------
    {
        const QString noopPath = dir.path() + "/noop.json";
        LocalMemoryBank bank;
        bank.setFilePath(noopPath);
        bank.handleCommand("memory create");

        MemoryEntry entry;
        entry.freq = 21.074;
        bank.record(0, entry);
        bank.flush();
        ok &= expect(QFile::exists(noopPath), "the first record saved");

        // Overwrite the file with a sentinel. A rewrite would destroy it —
        // deterministic where comparing mtimes would depend on filesystem
        // timestamp granularity.
        {
            QFile f(noopPath);
            ok &= expect(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         "sentinel written over the bank file");
            f.write("sentinel");
        }

        // Re-asserting the same values (what the dialog does via
        // handleMemoryStatus after every successful write) must not dirty it.
        bank.record(0, entry);
        bank.flush();

        QFile f(noopPath);
        ok &= expect(f.open(QIODevice::ReadOnly), "the sentinel file is readable");
        ok &= expect(f.readAll() == "sentinel",
                     "re-recording identical values does not rewrite the file");

        // A real change still saves.
        entry.freq = 21.075;
        bank.record(0, entry);
        bank.flush();
        QFile changed(noopPath);
        ok &= expect(changed.open(QIODevice::ReadOnly), "the rewritten file is readable");
        ok &= expect(changed.readAll() != "sentinel", "a changed value does rewrite the file");
    }

    // --- a CSV-import-shaped run of many records --------------------------
    {
        // MemoryDialog's import is a create→set chain, re-entered per record.
        // Allocation has to keep up across the whole file: every record must
        // land in its own slot, in order, with none reused or skipped.
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
                     "the whole bulk bank round-trips through the file");
    }

    if (ok)
        std::cout << "local_memory_bank_test: all checks passed\n";
    return ok ? 0 : 1;
}
