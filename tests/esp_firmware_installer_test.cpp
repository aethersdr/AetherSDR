// EspFirmwareManifest and EspFirmwareInstaller against an in-memory flash: a
// manifest that would touch a protected region is refused, a device that is
// not the one the manifest is for is left untouched, an install writes its
// images and boot selection and nothing else, "start" changes only the boot
// selection, and a backup restores. No serial port, no device.

#include "core/EspFirmwareInstaller.h"
#include "core/EspFirmwareManifest.h"
#include "core/EspFlashTarget.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr quint32 kFlash = 0x1000000;
constexpr quint32 kSector = EspFlashTarget::kSectorSize;

// Layout of the example device: the vendor's firmware in slot 0 with its own
// settings and data, and two slots for the firmware being installed.
constexpr quint32 kTable = 0x8000;
constexpr quint32 kVendorSettings = 0x9000;
constexpr quint32 kOtadata = 0xE000;
constexpr quint32 kSlot0 = 0x10000;
constexpr quint32 kVendorData = 0x310000;
constexpr quint32 kSlot1 = 0x410000;
constexpr quint32 kSlot2 = 0x710000;

class FakeFlash : public EspFlashTarget {
public:
    QByteArray flash = QByteArray(kFlash, '\xFF');
    QString chip = QStringLiteral("ESP32-S3");
    quint32 size = kFlash;
    int writes = 0;
    QList<quint32> written;   // write addresses
    // A write here erases its sectors, then fails: power lost mid-write.
    quint32 failAt = 0xFFFFFFFFu;

    QString chipName() const override { return chip; }
    QByteArray mac() const override { return QByteArray::fromHex("0102030405AA"); }
    quint32 flashSize() const override { return size; }
    bool read(quint32 address, quint32 length, QByteArray* out, const Progress& progress,
              QString*) override
    {
        *out = flash.mid(address, length);
        if (progress)
            progress(length, length);
        return true;
    }
    bool write(quint32 address, const QByteArray& data, const Progress&, QString* error) override
    {
        if (address % kSector) {
            *error = QStringLiteral("unaligned");
            return false;
        }
        const quint32 end = (address + data.size() + kSector - 1) / kSector * kSector;
        flash.replace(address, end - address, QByteArray(end - address, '\xFF'));
        if (address == failAt) {
            *error = QStringLiteral("Interrupted after erase");
            return false;
        }
        flash.replace(address, data.size(), data);
        ++writes;
        written.append(address);
        return true;
    }
    bool matches(quint32 address, const QByteArray& data) override
    {
        return flash.mid(address, data.size()) == data;
    }
    bool restart(QString*) override { return true; }
};

QByteArray pattern(int size, char seed)
{
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i)
        b[i] = static_cast<char>(seed + i * 7);
    return b;
}

QByteArray sha256(const QByteArray& b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}

// A partition table: the vendor's first five rows, then the installer's.
QByteArray vendorRows()
{
    return pattern(5 * 32, 'v');
}

QByteArray table()
{
    QByteArray t = vendorRows() + pattern(3 * 32, 't');
    t.append(QByteArray(0xC00 - t.size(), '\xFF'));
    return t;
}

QByteArray appImage(char seed)
{
    QByteArray a = pattern(0x12345, seed);
    a[0] = static_cast<char>(0xE9);
    return a;
}

FakeFlash vendorDevice()
{
    FakeFlash d;
    d.flash.replace(kTable, 5 * 32, vendorRows());
    d.flash.replace(kVendorSettings, 0x5000, pattern(0x5000, 's'));
    QByteArray vendorApp = pattern(0x180000, 'a');
    vendorApp[0] = static_cast<char>(0xE9);
    d.flash.replace(kSlot0, vendorApp.size(), vendorApp);
    d.flash.replace(kVendorData, 0xE0000, pattern(0xE0000, 'd'));
    // The vendor's firmware starts: otadata names slot 0.
    d.flash.replace(kOtadata, 0x2000,
                    EspFirmwareInstaller::selectSlot(QByteArray(0x2000, '\xFF'), 0, 3));
    return d;
}

QString hex(quint32 v)
{
    return QStringLiteral("0x%1").arg(v, 0, 16);
}

QJsonObject region(const QString& name, quint32 offset, quint32 size)
{
    return {{"name", name}, {"offset", hex(offset)}, {"size", hex(size)}};
}

QJsonObject manifestJson(const QHash<QString, QByteArray>& files)
{
    QJsonObject req = region(QStringLiteral("vendor partitions"), kTable, 5 * 32);
    req["sha256"] = QString::fromLatin1(sha256(vendorRows()).toHex());
    QJsonArray images;
    const auto image = [&](const QString& file, quint32 offset) {
        images.append(QJsonObject{{"file", file},
                                  {"offset", hex(offset)},
                                  {"size", files.value(file).size()},
                                  {"sha256", QString::fromLatin1(sha256(files.value(file)).toHex())}});
    };
    image(QStringLiteral("table.bin"), kTable);
    image(QStringLiteral("app.bin"), kSlot1);
    return {
        {"format", 1},
        {"product", "Example"},
        {"version", "1.0.0"},
        {"chip", "ESP32-S3"},
        {"flashSize", hex(kFlash)},
        {"protect", QJsonArray{region(QStringLiteral("vendor settings"), kVendorSettings, 0x5000),
                               region(QStringLiteral("vendor firmware"), kSlot0, 0x300000),
                               region(QStringLiteral("vendor data"), kVendorData, 0xE0000)}},
        {"require", QJsonArray{req}},
        {"boot", QJsonObject{{"otadata", hex(kOtadata)},
                             {"slots", QJsonArray{hex(kSlot0), hex(kSlot1), hex(kSlot2)}},
                             {"ownSlots", QJsonArray{1, 2}}}},
        {"images", images},
    };
}

QHash<QString, QByteArray> files()
{
    return {{QStringLiteral("table.bin"), table()}, {QStringLiteral("app.bin"), appImage('x')}};
}

EspFirmwareManifest parsed(const QJsonObject& json)
{
    EspFirmwareManifest m;
    QString problem;
    const bool ok = EspFirmwareManifest::parse(QJsonDocument(json).toJson(), &m, &problem);
    if (!ok)
        std::fprintf(stderr, "parse: %s\n", qPrintable(problem));
    return m;
}

bool parses(const QJsonObject& json, QString* problem)
{
    EspFirmwareManifest m;
    return EspFirmwareManifest::parse(QJsonDocument(json).toJson(), &m, problem);
}

// The vendor's regions read as they did before.
bool vendorIntact(const FakeFlash& d, const FakeFlash& before)
{
    return d.flash.mid(kVendorSettings, 0x5000) == before.flash.mid(kVendorSettings, 0x5000)
           && d.flash.mid(kSlot0, 0x300000) == before.flash.mid(kSlot0, 0x300000)
           && d.flash.mid(kVendorData, 0xE0000) == before.flash.mid(kVendorData, 0xE0000)
           && d.flash.mid(kTable, 5 * 32) == vendorRows();
}

void testOtadata()
{
    using I = EspFirmwareInstaller;
    const QByteArray blank(0x2000, '\xFF');
    check(I::readOtadata(blank)[0].valid == false && I::readOtadata(blank)[1].valid == false,
          "blank otadata has no valid entry");

    QByteArray o = I::selectSlot(blank, 1, 3);
    check(I::readOtadata(o)[0].seq == 2 && I::readOtadata(o)[0].valid,
          "from blank, slot 1 of 3 is sequence 2 in the first entry");
    // As ESP-IDF computes it: crc32_le(UINT32_MAX) over the sequence number.
    check(static_cast<quint8>(o[28]) == 0x74 && static_cast<quint8>(o[31]) == 0x55,
          "the entry's CRC is ESP-IDF's");

    o = I::selectSlot(o, 2, 3);
    check(I::readOtadata(o)[1].seq == 3 && I::readOtadata(o)[0].seq == 2,
          "the next selection is written over the other entry");
    o = I::selectSlot(o, 0, 3);
    check(I::readOtadata(o)[0].seq == 4 && I::readOtadata(o)[1].seq == 3,
          "selecting slot 0 keeps the entry for slot 2");
    check(I::newestOf(o, {1, 2}, 3) == 2, "the newest entry naming an own slot is found");
    check(I::newestOf(o, {0}, 3) == 0, "and the one naming slot 0");
    check(I::newestOf(blank, {1, 2}, 3) == -1, "no own slot in blank otadata");

    QByteArray corrupt = o;
    corrupt[28] = static_cast<char>(corrupt[28] ^ 1);
    check(!I::readOtadata(corrupt)[0].valid, "an entry with a bad CRC is not valid");
}

void testManifest()
{
    const QHash<QString, QByteArray> f = files();
    QString problem;
    check(parses(manifestJson(f), &problem), "the example manifest parses");

    QJsonObject j = manifestJson(f);
    j["format"] = 2;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("newer")),
          "a newer format asks for a newer AetherSDR");

    j = manifestJson(f);
    QJsonArray images = j["images"].toArray();
    QJsonObject app = images[1].toObject();
    app["offset"] = hex(kSlot0 - kSector);
    images[1] = app;
    j["images"] = images;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("vendor firmware")),
          "an image running into a protected region is refused");

    j = manifestJson(f);
    QJsonArray protect = j["protect"].toArray();
    protect.append(region(QStringLiteral("tail"), kSlot1 + 0x12345 + 4, 4));
    j["protect"] = protect;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("tail")),
          "erasing the image's last sector counts: a region in it is protected");

    j = manifestJson(f);
    protect = j["protect"].toArray();
    protect.append(region(QStringLiteral("boot"), kOtadata, 0x2000));
    j["protect"] = protect;
    check(!parses(j, &problem), "a protected boot selection is refused");

    j = manifestJson(f);
    QJsonObject boot = j["boot"].toObject();
    boot["ownSlots"] = QJsonArray{0};
    j["boot"] = boot;
    check(!parses(j, &problem), "an own slot inside a protected region is refused");

    j = manifestJson(f);
    images = j["images"].toArray();
    app = images[1].toObject();
    app["file"] = QStringLiteral("../app.bin");
    images[1] = app;
    j["images"] = images;
    check(!parses(j, &problem), "an image file outside the manifest's folder is refused");

    j = manifestJson(f);
    images = j["images"].toArray();
    app = images[1].toObject();
    app["offset"] = hex(kSlot1 + 4);
    images[1] = app;
    j["images"] = images;
    check(!parses(j, &problem), "an image not on a sector boundary is refused");

    check(!parses(QJsonObject{{"format", 1}}, &problem), "an empty manifest is refused");
}

void testInstall()
{
    const QHash<QString, QByteArray> f = files();
    const EspFirmwareManifest m = parsed(manifestJson(f));
    FakeFlash d = vendorDevice();
    const FakeFlash before = d;
    QString problem;
    QStringList steps;
    EspFirmwareInstaller installer(&d, [&](const QString& step, qint64, qint64) {
        if (!steps.contains(step))
            steps.append(step);
        return true;
    });
    check(installer.install(m, f, &problem), "install succeeds");
    check(d.flash.mid(kTable, table().size()) == table(), "the partition table is written");
    check(d.flash.mid(kSlot1, f.value("app.bin").size()) == f.value("app.bin"),
          "the app is written to its slot");
    check(EspFirmwareInstaller::newestOf(d.flash.mid(kOtadata, 0x2000), {0, 1, 2}, 3) == 1,
          "the boot selection starts the first own slot");
    check(vendorIntact(d, before), "the vendor's firmware, settings and data are untouched");
    check(EspFirmwareInstaller::newestOf(d.flash.mid(kOtadata, 0x2000), {0}, 3) == 0,
          "the vendor's boot entry is kept");
    check(steps.contains(QStringLiteral("Writing app.bin")), "progress names the step");

    const int writesBefore = d.writes;
    check(installer.install(m, f, &problem), "installing again succeeds");
    check(d.writes - writesBefore == 1, "and rewrites only the boot selection");
}

void testInstallRefusals()
{
    const QHash<QString, QByteArray> f = files();
    const EspFirmwareManifest m = parsed(manifestJson(f));
    QString problem;

    FakeFlash d = vendorDevice();
    d.chip = QStringLiteral("ESP32-C3");
    EspFirmwareInstaller a(&d, {});
    check(!a.install(m, f, &problem) && d.writes == 0, "another chip is refused, nothing written");

    d = vendorDevice();
    d.size = 0x400000;
    EspFirmwareInstaller b(&d, {});
    check(!b.install(m, f, &problem) && d.writes == 0, "another flash size is refused");

    d = vendorDevice();
    d.flash[kTable + 3] = 'Z';
    EspFirmwareInstaller c(&d, {});
    check(!c.install(m, f, &problem) && d.writes == 0
              && problem.contains(QStringLiteral("vendor partitions")),
          "a device without the required layout is refused");

    d = vendorDevice();
    QHash<QString, QByteArray> wrong = f;
    wrong[QStringLiteral("app.bin")][100] = 'Q';
    EspFirmwareInstaller e(&d, {});
    check(!e.install(m, wrong, &problem) && d.writes == 0, "a file that is not the one named is refused");

    // A table that changes the required rows: the manifest is consistent, so
    // only comparing what the install leaves behind catches it.
    d = vendorDevice();
    QHash<QString, QByteArray> badTable = f;
    badTable[QStringLiteral("table.bin")][10] = 'Q';
    const EspFirmwareManifest m2 = parsed(manifestJson(badTable));
    EspFirmwareInstaller g(&d, {});
    check(!g.install(m2, badTable, &problem) && d.writes == 0,
          "an install that would change required bytes is refused");

    // A manifest built in code, never parsed: the write-time guard holds.
    d = vendorDevice();
    EspFirmwareManifest unchecked = m;
    unchecked.images[1].offset = kSlot0;
    const FakeFlash before = d;
    EspFirmwareInstaller h(&d, {});
    check(!h.install(unchecked, f, &problem) && vendorIntact(d, before),
          "nothing is written over a protected region, even from an unchecked manifest");
}

void testStart()
{
    const QHash<QString, QByteArray> f = files();
    const EspFirmwareManifest m = parsed(manifestJson(f));
    QString problem;

    FakeFlash d = vendorDevice();
    EspFirmwareInstaller installer(&d, {});
    check(!installer.start(m, &problem) && problem.contains(QStringLiteral("not installed")),
          "start refuses when the firmware is not installed");
    check(d.writes == 0, "and writes nothing");

    check(installer.install(m, f, &problem), "install");
    // The firmware updated itself into slot 2, then switched to the vendor's.
    d.flash.replace(kSlot2, f.value("app.bin").size(), f.value("app.bin"));
    QByteArray o = EspFirmwareInstaller::selectSlot(d.flash.mid(kOtadata, 0x2000), 2, 3);
    o = EspFirmwareInstaller::selectSlot(o, 0, 3);
    d.flash.replace(kOtadata, 0x2000, o);

    const FakeFlash before = d;
    const int writes = d.writes;
    check(installer.start(m, &problem), "start succeeds");
    check(EspFirmwareInstaller::newestOf(d.flash.mid(kOtadata, 0x2000), {0, 1, 2}, 3) == 2,
          "start selects the firmware's current slot, not the first");
    check(d.writes - writes == 1 && (d.written.last() & ~(kSector * 2 - 1)) == kOtadata,
          "and writes only the boot selection");
    check(vendorIntact(d, before) && d.flash.mid(kSlot1, 0x600000) == before.flash.mid(kSlot1, 0x600000),
          "everything else is as it was");
}

void testBackupRestore()
{
    FakeFlash d = vendorDevice();
    QString problem;
    EspFirmwareInstaller installer(&d, {});
    QByteArray saved;
    check(installer.backup(&saved, &problem) && saved == d.flash, "the backup is the whole flash");

    check(installer.install(parsed(manifestJson(files())), files(), &problem), "install");
    check(d.flash != saved, "the install changed the flash");
    const int writes = d.writes;
    check(installer.restore(saved, &problem) && d.flash == saved, "restore puts it back");
    check(d.writes - writes <= 4, "restore writes only what differs");

    check(!installer.restore(saved.left(0x400000), &problem) && problem.contains(QStringLiteral("backup")),
          "a backup of another size is refused");
}

// A plan that does not hold together is refused before anything is written,
// even with every image's hash intact: writes that share a sector, and a
// required region under the boot selection (rewritten by install and start).
void testPlanRefusals()
{
    const QHash<QString, QByteArray> f = files();
    QString problem;

    // A second image one sector into the app: it would erase part of it.
    QHash<QString, QByteArray> two = f;
    two[QStringLiteral("extra.bin")] = pattern(0x100, 'e');
    QJsonObject j = manifestJson(two);
    QJsonArray images = j["images"].toArray();
    images.append(QJsonObject{{"file", "extra.bin"},
                              {"offset", hex(kSlot1 + kSector)},
                              {"size", two.value("extra.bin").size()},
                              {"sha256", QString::fromLatin1(sha256(two.value("extra.bin")).toHex())}});
    j["images"] = images;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("overlap")),
          "overlapping images are refused when parsed");
    {
        EspFirmwareManifest unchecked = parsed(manifestJson(f));
        EspFirmwareManifest::Image extra;
        extra.file = QStringLiteral("extra.bin");
        extra.offset = kSlot1 + kSector;
        extra.size = 0x100;
        extra.sha256 = sha256(two.value("extra.bin"));
        unchecked.images.append(extra);
        FakeFlash d = vendorDevice();
        EspFirmwareInstaller i(&d, {});
        check(!i.install(unchecked, two, &problem) && d.writes == 0,
              "and an unchecked manifest with them writes nothing");
    }

    // An image in the boot selection's sectors.
    j = manifestJson(f);
    images = j["images"].toArray();
    QJsonObject table = images[0].toObject();
    table["offset"] = hex(kOtadata + kSector);
    images[0] = table;
    j["images"] = images;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("boot.otadata")),
          "an image under the boot selection is refused");

    // A required region over the boot selection.
    j = manifestJson(f);
    QJsonArray require = j["require"].toArray();
    QJsonObject boot = region(QStringLiteral("boot selection"), kOtadata, 0x20);
    boot["sha256"] = QString::fromLatin1(sha256(vendorDevice().flash.mid(kOtadata, 0x20)).toHex());
    require.append(boot);
    j["require"] = require;
    check(!parses(j, &problem) && problem.contains(QStringLiteral("boot selection")),
          "a required region under the boot selection is refused");
    {
        EspFirmwareManifest unchecked = parsed(manifestJson(f));
        EspFirmwareManifest::Region r;
        r.name = QStringLiteral("boot selection");
        r.offset = kOtadata;
        r.size = 0x20;
        FakeFlash d = vendorDevice();
        r.sha256 = sha256(d.flash.mid(kOtadata, 0x20));
        unchecked.require.append(r);
        EspFirmwareInstaller i(&d, {});
        check(!i.install(unchecked, f, &problem) && d.writes == 0,
              "and an unchecked manifest with one installs nothing");
        check(!i.start(unchecked, &problem) && d.writes == 0, "nor starts");
    }

    // A boot selection not on a sector boundary erases whole sectors around
    // it: a required region in the last of them must count as under it.
    {
        EspFirmwareManifest unchecked = parsed(manifestJson(f));
        // Its bytes are [kOtadata - kSector + 0x100, kOtadata + kSector + 0x100);
        // writing them erases through kOtadata + 2 * kSector.
        unchecked.otadata = kOtadata - kSector + 0x100;
        EspFirmwareManifest::Region r;
        r.name = QStringLiteral("neighbor");
        r.offset = kOtadata + kSector + 0x200;            // past its bytes, inside its last sector
        r.size = 0x20;
        FakeFlash d = vendorDevice();
        r.sha256 = sha256(d.flash.mid(r.offset, r.size));
        unchecked.require.append(r);
        EspFirmwareInstaller i(&d, {});
        check(!i.install(unchecked, f, &problem) && d.writes == 0,
              "a required region in a sector the boot selection erases is refused");
    }
}

// A boot selection interrupted after its erase leaves the entry the device
// started from in place.
void testInterruptedBootSelection()
{
    const QHash<QString, QByteArray> f = files();
    const EspFirmwareManifest m = parsed(manifestJson(f));
    QString problem;
    FakeFlash d = vendorDevice();
    EspFirmwareInstaller installer(&d, {});
    check(installer.install(m, f, &problem), "install");

    // The vendor's entry is sequence 1 in the first sector, the install's is
    // sequence 2 in the second; the next selection replaces the first.
    int interrupted = 0;
    for (quint32 at : {kOtadata, kOtadata + kSector}) {
        FakeFlash e = d;
        e.failAt = at;
        EspFirmwareInstaller i(&e, {});
        problem.clear();
        const bool ok = i.start(m, &problem);
        const QList<EspFirmwareInstaller::OtaEntry> entries =
            EspFirmwareInstaller::readOtadata(e.flash.mid(kOtadata, 0x2000));
        const int valid = int(entries[0].valid) + int(entries[1].valid);
        if (ok)
            continue;   // that sector was not the one to replace
        ++interrupted;
        check(!ok && problem.contains(QStringLiteral("Interrupted")),
              "an interrupted boot selection reports the failure");
        check(valid >= 1, "an interrupted boot selection leaves a valid entry");
        check(EspFirmwareInstaller::newestOf(e.flash.mid(kOtadata, 0x2000), {0, 1, 2}, 3) == 1,
              "and the device still starts what it started before");
    }
    check(interrupted == 1, "exactly one otadata sector is written");
}

// Cancelling before a write starts writes nothing.
void testCancelBeforeWrite()
{
    const QHash<QString, QByteArray> f = files();
    const EspFirmwareManifest m = parsed(manifestJson(f));
    QString problem;
    const auto refuse = [](const QString&, qint64, qint64) { return false; };
    problem.clear();

    FakeFlash d = vendorDevice();
    QByteArray saved = d.flash;
    saved[0] = 'R';
    EspFirmwareInstaller restore(&d, refuse);
    check(!restore.restore(saved, &problem) && d.writes == 0 && !problem.isEmpty(),
          "a cancelled restore writes nothing, not even its first chunk");

    d = vendorDevice();
    EspFirmwareInstaller ok(&d, {});
    check(ok.install(m, f, &problem), "install");
    const int writes = d.writes;
    problem.clear();
    EspFirmwareInstaller start(&d, refuse);
    check(!start.start(m, &problem) && d.writes == writes && !problem.isEmpty(),
          "a cancelled start leaves the boot selection alone");

    d = vendorDevice();
    problem.clear();
    EspFirmwareInstaller install(&d, refuse);
    check(!install.install(m, f, &problem) && d.writes == 0 && !problem.isEmpty(),
          "a cancelled install writes nothing");
}

} // namespace

int main()
{
    testOtadata();
    testManifest();
    testInstall();
    testInstallRefusals();
    testStart();
    testBackupRestore();
    testPlanRefusals();
    testInterruptedBootSelection();
    testCancelBeforeWrite();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("esp_firmware_installer_test: all checks passed\n");
    return 0;
}
