// The CTR2-Max's own firmware, installed from the file Lynovation publishes:
// only an intact ESP32-S3 image carrying the CTR2-Max firmware's markers is
// taken; only its application is written, into app0, laid out by the unit's
// own partition table (Lynovation's original five rows, or the eight with
// AetherCTR2's slotCount), and app0 is selected to start; the bootloader, the
// table, the settings, the registration and AetherCTR2 are never written.
// The window's flow waits for download mode, backs up first, keeps the
// latest backups only, and needs no registration. Synthetic images and an
// in-memory flash; no serial port, no network.

#include "Ctr2MaxRegistrationFixtures.h"
#include "core/Ctr2MaxStockCatalog.h"
#include "core/EspFirmwareCatalog.h"
#include "core/Ctr2MaxRegistration.h"
#include "core/Ctr2MaxStockFirmware.h"
#include "core/EspFirmwareInstaller.h"
#include "core/EspFirmwareJob.h"
#include "core/EspFlashTarget.h"
#include "core/ZipArchive.h"
#include "models/FirmwareInstallModel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <cstdio>
#include <memory>

extern "C" {
#include "monocypher-ed25519.h"
}

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

constexpr quint32 kFlash = 0x1000000;   // 16 MB, as the CTR2-Max

void putLe32(QByteArray* b, qsizetype at, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        (*b)[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// An ESP image: one segment holding payload, its appended SHA-256.
QByteArray espImage(const QByteArray& payload, quint16 chip = 9)
{
    QByteArray seg = payload;
    while (seg.size() % 4)
        seg.append('\0');
    QByteArray img(24, '\0');
    img[0] = static_cast<char>(0xE9);
    img[1] = 1;   // one segment
    img[12] = static_cast<char>(chip & 0xFF);
    img[13] = static_cast<char>(chip >> 8);
    img[23] = 1;   // hash appended
    QByteArray head(8, '\0');
    putLe32(&head, 0, 0x42000020);
    putLe32(&head, 4, static_cast<quint32>(seg.size()));
    img += head + seg;
    const qsizetype body = (img.size() + 1 + 15) / 16 * 16;
    img.append(QByteArray(body - img.size(), '\0'));
    img += QCryptographicHash::hash(img, QCryptographicHash::Sha256);
    return img;
}

QByteArray app(const QByteArray& version = "v1.02.00l", const QByteArray& extra = {})
{
    QByteArray p("\0CTR2-Max\0Lynovation, LLC\0", 26);
    p += version + QByteArray(1, '\0') + extra + QByteArray(4000, 'x');
    return espImage(p);
}

struct Row {
    const char* name;
    quint8 type;
    quint8 subtype;
    quint32 offset;
    quint32 size;
};

const QList<Row> kRows5 = {
    {"nvs", 1, 2, 0x9000, 0x5000},          {"otadata", 1, 0, 0xE000, 0x2000},
    {"app0", 0, 0x10, 0x10000, 0x300000},   {"spiffs", 1, 0x82, 0x310000, 0xE0000},
    {"coredump", 1, 3, 0x3F0000, 0x10000},
};

QList<Row> rows8()
{
    QList<Row> r = kRows5;
    r.append({"actr2_nvs", 1, 2, 0x400000, 0x10000});
    r.append({"app1", 0, 0x11, 0x410000, 0x300000});
    r.append({"app2", 0, 0x12, 0x710000, 0x300000});
    return r;
}

QByteArray table(const QList<Row>& rows)
{
    QByteArray t(Ctr2MaxStockFirmware::kTableSize, '\xFF');
    for (int i = 0; i < rows.size(); ++i) {
        QByteArray e(32, '\0');
        e[0] = static_cast<char>(0xAA);
        e[1] = 0x50;
        e[2] = static_cast<char>(rows[i].type);
        e[3] = static_cast<char>(rows[i].subtype);
        putLe32(&e, 4, rows[i].offset);
        putLe32(&e, 8, rows[i].size);
        e.replace(12, int(qstrlen(rows[i].name)), rows[i].name);
        t.replace(i * 32, 32, e);
    }
    return t;
}

// A merged image as Lynovation publishes it: bootloader, table, app0.
QByteArray merged(const QByteArray& application, const QList<Row>& rows)
{
    QByteArray img(0x10000, '\xFF');
    const QByteArray boot = espImage(QByteArray(3000, 'b'));
    img.replace(0, boot.size(), boot);
    img.replace(0x8000, Ctr2MaxStockFirmware::kTableSize, table(rows));
    return img + application;
}

class MemFlash : public EspFlashTarget {
public:
    explicit MemFlash(QByteArray mac = QByteArray::fromHex("B81F3FC45888")) : m_mac(std::move(mac)) {}
    QString chipName() const override { return QStringLiteral("ESP32-S3"); }
    QByteArray mac() const override { return m_mac; }
    quint32 flashSize() const override { return kFlash; }
    bool read(quint32 address, quint32 length, QByteArray* out, const Progress&, QString*) override
    {
        *out = flash.mid(address, length);
        return true;
    }
    bool write(quint32 address, const QByteArray& data, const Progress&, QString*) override
    {
        const quint32 end = (address + data.size() + kSectorSize - 1) / kSectorSize * kSectorSize;
        flash.replace(address, end - address, QByteArray(end - address, '\xFF'));
        flash.replace(address, data.size(), data);
        writes.append({address, end});
        return true;
    }
    bool matches(quint32 address, const QByteArray& data) override
    {
        return flash.mid(address, data.size()) == data;
    }
    bool restart(QString*) override
    {
        ++restarts;
        return true;
    }

    QByteArray flash = QByteArray(kFlash, '\x5A');
    QList<QPair<quint32, quint32>> writes;
    int restarts = 0;

private:
    QByteArray m_mac;
};

// A unit laid out by rows, its boot selection naming slot.
void layOut(MemFlash* f, const QList<Row>& rows, int slot, int slotCount)
{
    f->flash.replace(0x8000, Ctr2MaxStockFirmware::kTableSize, table(rows));
    const QByteArray ota =
        EspFirmwareInstaller::selectSlot(QByteArray(0x2000, '\xFF'), slot, slotCount);
    f->flash.replace(0xE000, ota.size(), ota);
}

int startsSlot(const MemFlash& f, int slotCount)
{
    QList<int> all;
    for (int i = 0; i < slotCount; ++i)
        all.append(i);
    return EspFirmwareInstaller::newestOf(f.flash.mid(0xE000, 0x2000), all, slotCount);
}

bool install(const Ctr2MaxStockFirmware& fw, MemFlash* f, QString* problem)
{
    EspFirmwareManifest m;
    QHash<QString, QByteArray> files;
    QByteArray t;
    f->read(Ctr2MaxStockFirmware::kTableOffset, Ctr2MaxStockFirmware::kTableSize, &t, {}, problem);
    if (!fw.plan(t, f->flashSize(), &m, &files, problem))
        return false;
    EspFirmwareInstaller installer(f, {});
    return installer.check(m, problem) && installer.install(m, files, problem);
}

void testReadFile()
{
    Ctr2MaxStockFirmware fw;
    QString problem;
    const QByteArray bin = merged(app(), rows8());
    check(Ctr2MaxStockFirmware::read(bin, &fw, &problem), "a merged image is read");
    check(fw.version() == QStringLiteral("v1.02.00l"), "its version is found");
    check(fw.app() == app(), "and its application extracted, without the padding after it");

    // The download: a zip with manuals and settings beside the one .bin.
    const QByteArray zip = writeStoredZip({{QStringLiteral("Manuals/manual.pdf"), "%PDF"},
                                           {QStringLiteral("CTR2-Max_v10200l.bin"), bin},
                                           {QStringLiteral("READ_ME.txt"), "read me"}});
    Ctr2MaxStockFirmware fromZip;
    check(Ctr2MaxStockFirmware::read(zip, &fromZip, &problem) && fromZip.app() == app(),
          "the zip as downloaded is read");
    check(!Ctr2MaxStockFirmware::read(writeStoredZip({{QStringLiteral("a.pdf"), "x"}}), &fw,
                                      &problem),
          "a zip with no .bin is refused");
    check(!Ctr2MaxStockFirmware::read(writeStoredZip({{QStringLiteral("a.bin"), bin},
                                                      {QStringLiteral("b.bin"), bin}}),
                                      &fw, &problem),
          "a zip with two .bin files is refused");

    QByteArray damaged = bin;
    damaged[0x10000 + 200] = static_cast<char>(damaged[0x10000 + 200] ^ 1);
    check(!Ctr2MaxStockFirmware::read(damaged, &fw, &problem)
              && problem.contains(QStringLiteral("checksum")),
          "a damaged application is refused by its checksum");
    QByteArray other = merged(app(), rows8());
    other.replace(0x10000, app().size(), espImage(QByteArray("CTR2-Max Lynovation", 19), 5));
    check(!Ctr2MaxStockFirmware::read(other, &fw, &problem), "firmware for another chip is refused");
    check(!Ctr2MaxStockFirmware::read(merged(espImage(QByteArray(5000, 'y')), rows8()), &fw,
                                      &problem),
          "ESP32-S3 firmware that is not the CTR2-Max's is refused");
    check(!Ctr2MaxStockFirmware::read(app(), &fw, &problem),
          "an application image on its own (not as Lynovation publishes it) is refused");
    QList<Row> small = kRows5;
    small[2].size = 0x1000;
    check(!Ctr2MaxStockFirmware::read(merged(app(), small), &fw, &problem),
          "firmware larger than its slot is refused");
}

// Lynovation's newer files carry AetherCTR2's rows; units in the field have
// the original five. Either way only app0 and one boot-selection sector
// change, and app0 starts.
void testInstallKeepsEverythingElse()
{
    struct Case {
        const char* what;
        QList<Row> file;
        QList<Row> unit;
        int before;
    };
    const QList<Case> cases = {
        {"8-row file on a 5-row unit", rows8(), kRows5, 0},
        {"5-row file on a 5-row unit", kRows5, kRows5, 0},
        {"8-row file on an 8-row unit running AetherCTR2", rows8(), rows8(), 1},
        {"5-row file on an 8-row unit running AetherCTR2", kRows5, rows8(), 2},
    };
    for (const Case& c : cases) {
        Ctr2MaxStockFirmware fw;
        QString problem;
        Ctr2MaxStockFirmware::read(merged(app(), c.file), &fw, &problem);
        MemFlash f;
        const int slotCount = c.unit.size() == kRows5.size() ? 1 : 3;
        layOut(&f, c.unit, c.before, slotCount);
        const QByteArray before = f.flash;
        const bool ok = install(fw, &f, &problem);
        check(ok, c.what);
        if (!ok) {
            std::fprintf(stderr, "  %s\n", qPrintable(problem));
            continue;
        }
        check(f.flash.mid(0x10000, fw.app().size()) == fw.app(), "the application is in app0");
        check(startsSlot(f, slotCount) == 0, "and app0 is selected to start");
        int otadataWrites = 0;
        bool inside = true;
        for (const auto& w : f.writes) {
            const bool app0 = w.first >= 0x10000 && w.second <= 0x310000;
            const bool ota = w.first >= 0xE000 && w.second <= 0x10000;
            otadataWrites += ota ? int((w.second - w.first) / 0x1000) : 0;
            inside = inside && (app0 || ota);
        }
        check(inside && otadataWrites == 1, "only app0 and one boot-selection sector are written");
        check(f.flash.left(0xE000) == before.left(0xE000),
              "the bootloader, table and settings are as they were");
        check(f.flash.mid(0x310000) == before.mid(0x310000),
              "the registration volume and AetherCTR2's slots are as they were");
    }
}

void testUnitLayoutMustAgree()
{
    Ctr2MaxStockFirmware fw;
    QString problem;
    Ctr2MaxStockFirmware::read(merged(app(), rows8()), &fw, &problem);
    const auto refused = [&](QList<Row> unit, const char* what) {
        MemFlash f;
        layOut(&f, unit, 0, 1);
        const QByteArray before = f.flash;
        problem.clear();
        check(!install(fw, &f, &problem) && f.flash == before, what);
    };
    QList<Row> moved = kRows5;
    moved[2].offset = 0x20000;
    refused(moved, "a unit whose app0 is elsewhere is refused, and nothing written");
    QList<Row> settings = kRows5;
    settings[0].size = 0x6000;
    refused(settings, "a unit whose settings are laid out otherwise is refused");
    refused(kRows5.mid(0, 3), "a unit without the registration volume is refused");
    QList<Row> gap = rows8();
    gap.removeAt(6);   // app1: ota_0 and ota_2 only
    refused(gap, "a unit whose slots skip one is refused (the boot selection would miscount)");

    // A plan cannot reach past what it protects, even if asked to.
    MemFlash f;
    layOut(&f, kRows5, 0, 1);
    EspFirmwareManifest m;
    QHash<QString, QByteArray> files;
    check(fw.plan(f.flash.mid(0x8000, Ctr2MaxStockFirmware::kTableSize), kFlash, &m, &files,
                  &problem),
          "a plan for a 5-row unit");
    m.images[0].offset = 0x310000;   // over the registration
    EspFirmwareInstaller installer(&f, {});
    const QByteArray before = f.flash;
    check(!installer.install(m, files, &problem) && f.flash == before,
          "the installer refuses to write over a region the plan protects");
}

// The window's flow: download mode first, the backup, no registration.
struct Devices {
    QList<FirmwareInstallModel::Loader> loaders;
    QList<Ctr2HidPort::DeviceInfo> hid;
};

bool waitFinished(FirmwareInstallModel& model, bool* ok, QString* message)
{
    bool done = false;
    const auto c = QObject::connect(&model, &FirmwareInstallModel::finished, &model,
                                    [&](bool o, const QString& m) {
                                        done = true;
                                        *ok = o;
                                        *message = m;
                                    });
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    QObject::disconnect(c);
    return done;
}

void testWindowFlow()
{
    QTemporaryDir dir;
    FirmwareInstallModel::setFirmwareFolderForTesting(dir.filePath(QStringLiteral("kept")));
    const QString path = dir.filePath(QStringLiteral("CTR2-Max_v10200l.zip"));
    {
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write(writeStoredZip({{QStringLiteral("CTR2-Max.bin"), merged(app(), rows8())}}));
    }
    auto devices = std::make_shared<Devices>();
    auto flash = std::make_shared<MemFlash>();
    layOut(flash.get(), kRows5, 0, 1);
    std::atomic<int> opens{0};

    FirmwareInstallModel model(nullptr);
    model.setJobTargetOpener([flash, &opens](const QString&, QString*, bool*) {
        ++opens;
        // The job owns what it opens; this one shares the test's flash.
        struct Shared : EspFlashTarget {
            std::shared_ptr<MemFlash> f;
            QString chipName() const override { return f->chipName(); }
            QByteArray mac() const override { return f->mac(); }
            quint32 flashSize() const override { return f->flashSize(); }
            bool read(quint32 a, quint32 l, QByteArray* o, const Progress& p, QString* e) override
            {
                return f->read(a, l, o, p, e);
            }
            bool write(quint32 a, const QByteArray& d, const Progress& p, QString* e) override
            {
                return f->write(a, d, p, e);
            }
            bool matches(quint32 a, const QByteArray& d) override { return f->matches(a, d); }
            bool restart(QString* e) override { return f->restart(e); }
        };
        auto s = std::make_unique<Shared>();
        s->f = flash;
        return std::unique_ptr<EspFlashTarget>(std::move(s));
    });
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });

    QString problem;
    check(!model.openStockFirmware(dir.filePath(QStringLiteral("missing.zip")), &problem),
          "a missing file is refused");
    check(model.openStockFirmware(path, &problem), "the downloaded zip opens");
    check(model.haveStockFirmware()
              && model.stockFirmwareName() == QStringLiteral("CTR2-Max firmware v1.02.00l"),
          "it is named by its version");

    // Four earlier backups of this unit and one of another: the latest
    // three of this unit are kept after the install.
    // A folder of the test's own: never the operator's Documents.
    QTemporaryDir backupRoot;
    FirmwareInstallModel::setBackupFolderForTesting(backupRoot.filePath(QStringLiteral("backups")));
    check(FirmwareInstallModel::backupFolder().startsWith(backupRoot.path()),
          "the test's backups go to its own folder");
    QDir().mkpath(FirmwareInstallModel::backupFolder());
    const QDir backups(FirmwareInstallModel::backupFolder());
    for (const char* name : {"flash-backup-B81F3FC45888-20260101-000001.bin",
                             "flash-backup-B81F3FC45888-20260101-000002.bin",
                             "flash-backup-B81F3FC45888-20260101-000003.bin",
                             "flash-backup-B81F3FC45888-20260101-000004.bin",
                             "flash-backup-AAAAAAAAAAAA-20260101-000001.bin",
                             "flash-backup-B81F3FC45888-mine.bin"}) {
        QFile f(backups.filePath(QString::fromLatin1(name)));
        f.open(QIODevice::WriteOnly);
    }

    // Its stock firmware is running: the install waits for download mode.
    devices->hid = {{QStringLiteral("hid"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     {}}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    check(model.device() == FirmwareInstallModel::Device::Stock, "the stock firmware is found");
    check(model.installStockFirmware(model.target(), &problem) && model.waitingForDownloadMode()
              && model.busy(),
          "the install waits for download mode");
    check(opens == 0, "and opens nothing meanwhile");
    bool cancelled = false;
    QString cancelMessage;
    const auto c = QObject::connect(&model, &FirmwareInstallModel::finished, &model,
                                    [&](bool o, const QString& m) {
                                        cancelled = !o;
                                        cancelMessage = m;
                                    });
    model.cancel();
    QObject::disconnect(c);
    check(!model.busy() && opens == 0, "a cancel while waiting ends it, with nothing opened");
    check(cancelled && cancelMessage.contains(QStringLiteral("Nothing was written")),
          "and says so, so the window leaves its waiting state");

    // The operator follows the alert at once: the window still holds the
    // stock firmware's target when the unit is already in download mode.
    const FirmwareInstallModel::Target stale = model.target();
    devices->hid.clear();
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    check(model.installStockFirmware(stale, &problem) && model.waitingForDownloadMode(),
          "an install confirmed on the stock firmware is not refused once the unit is in "
          "download mode");
    model.cancel();
    devices->loaders.clear();
    devices->hid = {{QStringLiteral("hid"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     {}}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    check(model.installStockFirmware(model.target(), &problem), "waiting again");
    devices->hid.clear();
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });   // looks again, as its poll does
    check(opens == 0 && model.waitingForDownloadMode(),
          "a port that has just appeared is not opened at once (udev may not have set its "
          "permissions yet)");
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });   // the next poll
    bool ok = false;
    QString message;
    check(waitFinished(model, &ok, &message) && ok,
          "found again on the next poll, the install runs by itself");
    if (!ok)
        std::fprintf(stderr, "  %s\n", qPrintable(message));
    check(opens == 1 && flash->restarts == 1, "once, and the unit is restarted");
    check(flash->flash.mid(0x10000, app().size()) == app(),
          "the firmware is installed on an unregistered unit: no registration is required");
    const QString backup = model.lastBackupPath();
    QFile b(backup);
    check(b.open(QIODevice::ReadOnly) && b.size() == kFlash && QFile::exists(backup + ".json"),
          "the whole flash was backed up first, with its description");
    const QStringList left = backups.entryList({QStringLiteral("flash-backup-*.bin")}, QDir::Files);
    check(left.size() == 5 && left.contains(QFileInfo(backup).fileName())
              && left.contains(QStringLiteral("flash-backup-B81F3FC45888-20260101-000004.bin"))
              && left.contains(QStringLiteral("flash-backup-B81F3FC45888-20260101-000003.bin"))
              && !left.contains(QStringLiteral("flash-backup-B81F3FC45888-20260101-000002.bin"))
              && left.contains(QStringLiteral("flash-backup-AAAAAAAAAAAA-20260101-000001.bin"))
              && left.contains(QStringLiteral("flash-backup-B81F3FC45888-mine.bin")),
          "the latest three backups of this unit are kept; others' and the operator's stay");

    // Two units in download mode: the install does not guess.
    devices->loaders.append({QStringLiteral("ttyACM1"), QStringLiteral("AA:AA:AA:AA:AA:AA")});
    devices->hid = {};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    devices->loaders = {};
    devices->hid = {{QStringLiteral("hid"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     {}}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    check(model.installStockFirmware(model.target(), &problem), "waiting for a stock unit");
    devices->hid.clear();
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")},
                        {QStringLiteral("ttyACM1"), QStringLiteral("AA:AA:AA:AA:AA:AA")}};
    model.setDeviceSources([devices] { return devices->loaders; },
                           [devices] { return devices->hid; });
    check(model.waitingForDownloadMode() && opens == 1,
          "two units in download mode: it keeps waiting, opening neither");
    model.cancel();
    FirmwareInstallModel::setBackupFolderForTesting({});
    FirmwareInstallModel::setFirmwareFolderForTesting({});
}

// The job owns what it opens; this target shares the test's flash.
struct SharedFlash : EspFlashTarget {
    std::shared_ptr<MemFlash> f;
    QString chipName() const override { return f->chipName(); }
    QByteArray mac() const override { return f->mac(); }
    quint32 flashSize() const override { return f->flashSize(); }
    bool read(quint32 a, quint32 l, QByteArray* o, const Progress& p, QString* e) override
    {
        return f->read(a, l, o, p, e);
    }
    bool write(quint32 a, const QByteArray& d, const Progress& p, QString* e) override
    {
        return f->write(a, d, p, e);
    }
    bool matches(quint32 a, const QByteArray& d) override { return f->matches(a, d); }
    bool restart(QString* e) override { return f->restart(e); }
};

// AetherCTR2 as its releases lay it out: its own two slots beside app0.
EspFirmwareManifest aetherManifest(const QByteArray& image)
{
    EspFirmwareManifest m;
    m.product = QStringLiteral("AetherCTR2");
    m.version = QStringLiteral("9.9.9");
    m.chip = QStringLiteral("ESP32-S3");
    m.flashSize = kFlash;
    m.protect = {{QStringLiteral("stock firmware"), 0x10000, 0x300000, {}},
                 {QStringLiteral("stock settings"), 0x310000, 0xE0000, {}}};
    m.otadata = 0xE000;
    m.appSlots = {0x10000, 0x410000, 0x710000};
    m.ownSlots = {1, 2};
    m.images = {{QStringLiteral("app.bin"), 0x410000, static_cast<quint32>(image.size()),
                 QCryptographicHash::hash(image, QCryptographicHash::Sha256)}};
    return m;
}

// The AetherCTR2 tab's flow on a unit running its stock firmware: it waits
// for download mode, backs up, checks the registration, installs; the backup
// is then listed for the unit and installs as a restore.
void testReleaseAndRestore()
{
    QTemporaryDir backupRoot;
    FirmwareInstallModel::setBackupFolderForTesting(backupRoot.filePath(QStringLiteral("b")));
    auto devices = std::make_shared<Devices>();
    auto flash = std::make_shared<MemFlash>();
    layOut(flash.get(), rows8(), 0, 3);
    const QByteArray volume = qUncompress(
        QByteArray::fromBase64(Ctr2MaxRegistrationFixtures::kRegisteredPartition));
    flash->flash.replace(Ctr2MaxRegistration::kOffset, volume.size(), volume);
    const QByteArray before = flash->flash;
    std::atomic<int> opens{0};

    FirmwareInstallModel model(nullptr);
    model.setJobTargetOpener([flash, &opens](const QString&, QString*, bool*) {
        ++opens;
        auto s = std::make_unique<SharedFlash>();
        s->f = flash;
        return std::unique_ptr<EspFlashTarget>(std::move(s));
    });
    const QByteArray image = espImage(QByteArray("AetherCTR2 test image", 21));
    model.setReleaseForTesting(aetherManifest(image), {{QStringLiteral("app.bin"), image}});
    const auto look = [&] {
        model.setDeviceSources([devices] { return devices->loaders; },
                               [devices] { return devices->hid; });
    };
    devices->hid = {{QStringLiteral("hid"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     {}}};
    look();
    QString problem;
    check(model.installRelease(model.target(), &problem) && model.waitingForDownloadMode()
              && opens == 0,
          "an AetherCTR2 install on a stock unit waits for download mode");
    devices->hid.clear();
    // The serial port may report the MAC in lower case: the same unit.
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("b8:1f:3f:c4:58:88")}};
    look();
    check(model.deviceMac() == QStringLiteral("B8:1F:3F:C4:58:88"),
          "a MAC the port reports in lower case is taken as the unit's (HID's are upper case)");
    look();   // the next poll
    bool ok = false;
    QString message;
    check(waitFinished(model, &ok, &message) && ok,
          "then installs by itself, whatever the case of the MAC the port reports");
    if (!ok)
        std::fprintf(stderr, "  %s\n", qPrintable(message));
    check(flash->flash.mid(0x410000, image.size()) == image && startsSlot(*flash, 3) == 1,
          "AetherCTR2 is in its slot and selected");
    check(QFile::exists(model.lastBackupPath()), "after a backup, made without asking");

    const QList<FirmwareInstallModel::Backup> listed = model.backups();
    check(listed.size() == 1 && listed.first().path == model.lastBackupPath()
              && listed.first().mac == QStringLiteral("B8:1F:3F:C4:58:88"),
          "the backup is listed for this unit");
    // Backups saved by hand with Back Up in earlier versions: in the folder
    // above (Documents), named by date, and under any name in the folder.
    {
        const QString made = model.lastBackupPath();
        const QDir above(QFileInfo(FirmwareInstallModel::backupFolder()).path());
        const QString old = above.filePath(QStringLiteral("flash-backup-B81F3FC45888-2026-10-01.bin"));
        const QString named = QDir(FirmwareInstallModel::backupFolder())
                                  .filePath(QStringLiteral("before the update.bin"));
        QFile::copy(made, old);
        QFile::copy(made + ".json", old + ".json");
        QFile::copy(made, named);
        QFile::copy(made + ".json", named + ".json");
        QFile::copy(made, above.filePath(QStringLiteral("no-description.bin")));
        const QList<FirmwareInstallModel::Backup> all = model.backups();
        const auto has = [&](const QString& path) {
            return std::any_of(all.cbegin(), all.cend(),
                               [&](const FirmwareInstallModel::Backup& b) { return b.path == path; });
        };
        check(all.size() == 3 && has(old) && has(named),
              "backups saved by hand earlier are listed, whatever their name; a .bin without "
              "its description is not");
        QFile::remove(old);
        QFile::remove(old + ".json");
        QFile::remove(named);
        QFile::remove(named + ".json");
        QFile::remove(above.filePath(QStringLiteral("no-description.bin")));
    }
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("AA:AA:AA:AA:AA:AA")}};
    look();
    check(model.backups().isEmpty(), "and not for another unit");
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")}};
    look();
    check(model.restoreBackup(model.target(), listed.first().path, &problem),
          "installing the backup restores it");
    check(waitFinished(model, &ok, &message) && ok && flash->flash == before,
          "the whole flash is as it was before the install");

    // Without a registration the AetherCTR2 install is refused, nothing written.
    flash->flash.replace(Ctr2MaxRegistration::kOffset, volume.size(),
                         QByteArray(volume.size(), '\x5A'));
    const QByteArray unregistered = flash->flash;
    check(model.installRelease(model.target(), &problem), "on an unregistered unit");
    check(waitFinished(model, &ok, &message) && !ok && flash->flash == unregistered,
          "the AetherCTR2 install is refused, and nothing is written");
    FirmwareInstallModel::setBackupFolderForTesting({});
}

void testVersionOrder()
{
    using F = Ctr2MaxStockFirmware;
    check(F::newer("v10.00.00", "v9.99.99"), "versions compare by number, not as text");
    check(F::newer("v1.02.00n", "v1.02.00l") && F::newer("v1.02.00l", "v1.01.03"),
          "a later letter and a later minor version are newer");
    check(F::newer("v1.02.00a", "v1.02.00") && !F::newer("v1.02.00", "v1.02.00a"),
          "a letter is newer than none");
    check(F::newer("v1.00.00", "") && !F::newer("", "v1.00.00"), "an unnamed version is oldest");
    check(!F::newer("v1.02.00n", "v1.02.00n"), "a version is not newer than itself");
}

// Each Lynovation file opened is kept, one copy per version, so an earlier
// version can be installed again after its download is gone.
void testFirmwareStore()
{
    QTemporaryDir root;
    FirmwareInstallModel::setFirmwareFolderForTesting(root.filePath(QStringLiteral("kept")));
    QTemporaryDir downloads;
    const auto download = [&](const QByteArray& version) {
        const QString path = downloads.filePath(QStringLiteral("CTR2-Max_%1.zip").arg(
            QString::fromLatin1(version)));
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write(writeStoredZip({{QStringLiteral("Manuals/manual.pdf"), "%PDF"},
                                  {QStringLiteral("CTR2-Max.bin"),
                                   merged(app(version), rows8())}}));
        return path;
    };
    FirmwareInstallModel model(nullptr);
    QString problem;
    check(model.openStockFirmware(download("v1.02.00n"), &problem)
              && model.openStockFirmware(download("v1.01.03"), &problem)
              && model.openStockFirmware(download("v1.02.00l"), &problem),
          "three versions open");
    check(model.openStockFirmware(download("v1.02.00n"), &problem), "and one of them again");
    const QList<FirmwareInstallModel::StoredFirmware> kept = model.storedFirmware();
    check(kept.size() == 3 && kept[0].name == QStringLiteral("CTR2-Max firmware v1.02.00n")
              && kept[1].name == QStringLiteral("CTR2-Max firmware v1.02.00l")
              && kept[2].name == QStringLiteral("CTR2-Max firmware v1.01.03"),
          "each version is kept once, newest first");
    QFile bin(kept[2].path);
    check(bin.open(QIODevice::ReadOnly) && bin.readAll() == merged(app("v1.01.03"), rows8()),
          "as the firmware image itself, without the manuals");
    QDir(downloads.path()).removeRecursively();
    check(model.chooseStockFirmware(kept[2].path, &problem)
              && model.stockFirmwareName() == QStringLiteral("CTR2-Max firmware v1.01.03"),
          "an earlier version is chosen from the kept copy, its download gone");
    FirmwareInstallModel fresh(nullptr);
    fresh.begin();
    check(fresh.storedFirmware().size() == 3, "and is there the next time the window opens");
    check(!model.openStockFirmware(root.filePath(QStringLiteral("missing.zip")), &problem)
              && model.storedFirmware().size() == 3,
          "a file that is not the firmware is not kept");
    FirmwareInstallModel::setFirmwareFolderForTesting({});
}

// Restart into: what is installed, the boot selection only, laid out by the
// unit's own table; the firmware it runs now is refused.
void testRestartInto()
{
    using Firmware = FirmwareInstallModel::Firmware;
    auto devices = std::make_shared<Devices>();
    auto flash = std::make_shared<MemFlash>();
    layOut(flash.get(), rows8(), 1, 3);   // starts AetherCTR2
    const QByteArray image = espImage(QByteArray("AetherCTR2", 10));
    flash->flash.replace(0x410000, image.size(), image);
    flash->flash.replace(0x10000, app().size(), app());   // the stock firmware
    FirmwareInstallModel model(nullptr);
    model.setJobTargetOpener([flash](const QString&, QString*, bool*) {
        auto s = std::make_unique<SharedFlash>();
        s->f = flash;
        return std::unique_ptr<EspFlashTarget>(std::move(s));
    });
    const auto look = [&] {
        model.setDeviceSources([devices] { return devices->loaders; },
                               [devices] { return devices->hid; });
    };
    const auto run = [&](Firmware f, const char* what) {
        flash->writes.clear();
        QString problem;
        bool ok = false;
        QString message;
        const bool started = model.restartInto(model.target(), f, &problem);
        check(started && waitFinished(model, &ok, &message) && ok, what);
        if (!ok)
            std::fprintf(stderr, "  %s %s\n", qPrintable(problem), qPrintable(message));
        bool onlyOtadata = !flash->writes.isEmpty();
        for (const auto& w : flash->writes)
            onlyOtadata = onlyOtadata && w.first >= 0xE000 && w.second <= 0x10000;
        check(onlyOtadata, "only the boot selection is written");
    };
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")}};
    look();
    QString problem;
    check(!model.restartInto(model.target(), Firmware::Bootloader, &problem),
          "Bootloader is refused in download mode: it runs it now");
    const QByteArray before = flash->flash;
    run(Firmware::Stock, "from download mode into the stock firmware");
    check(startsSlot(*flash, 3) == 0 && flash->flash.mid(0x10000) == before.mid(0x10000),
          "app0 is selected, and nothing else changed");
    run(Firmware::AetherCtr2, "and back into AetherCTR2");
    check(startsSlot(*flash, 3) == 1, "AetherCTR2's slot is selected");

    // From the stock firmware: AetherCTR2 waits for download mode, Bootloader
    // takes the unit's buttons, and Stock Firmware runs now.
    devices->loaders.clear();
    devices->hid = {{QStringLiteral("hid"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     {}}};
    look();
    check(!model.restartInto(model.target(), Firmware::Stock, &problem)
              && !model.restartInto(model.target(), Firmware::Bootloader, &problem),
          "from the stock firmware, Stock and Bootloader are not the model's to do");
    check(model.restartInto(model.target(), Firmware::AetherCtr2, &problem)
              && model.waitingForDownloadMode(),
          "AetherCTR2 waits for download mode");
    model.cancel();

    // A unit without AetherCTR2's slots, or without AetherCTR2 in them.
    devices->hid.clear();
    devices->loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("B8:1F:3F:C4:58:88")}};
    look();
    layOut(flash.get(), kRows5, 0, 1);
    const QByteArray five = flash->flash;
    bool ok = true;
    QString message;
    check(model.restartInto(model.target(), Firmware::AetherCtr2, &problem)
              && waitFinished(model, &ok, &message) && !ok && flash->flash == five,
          "a unit laid out without AetherCTR2 is refused, nothing written");
    layOut(flash.get(), rows8(), 0, 3);
    flash->flash.replace(0x410000, 0x1000, QByteArray(0x1000, '\xFF'));
    const QByteArray empty = flash->flash;
    check(model.restartInto(model.target(), Firmware::AetherCtr2, &problem)
              && waitFinished(model, &ok, &message) && !ok && flash->flash == empty,
          "and one whose AetherCTR2 slot is empty");
}

struct Key {
    QByteArray secret{64, '\0'};
    QByteArray pub{32, '\0'};
    explicit Key(char seed)
    {
        uint8_t s[32];
        for (int i = 0; i < 32; ++i)
            s[i] = static_cast<uint8_t>(seed + i);
        crypto_ed25519_key_pair(reinterpret_cast<uint8_t*>(secret.data()),
                                reinterpret_cast<uint8_t*>(pub.data()), s);
    }
    QByteArray sign(const QByteArray& message) const   // base64, as openssl | base64
    {
        QByteArray sig(64, '\0');
        crypto_ed25519_sign(reinterpret_cast<uint8_t*>(sig.data()),
                            reinterpret_cast<const uint8_t*>(secret.constData()),
                            reinterpret_cast<const uint8_t*>(message.constData()),
                            static_cast<size_t>(message.size()));
        return sig.toBase64() + "\n";
    }
};

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

// Lynovation's own list of releases, as proposed to Lynovation: signed with
// Lynovation's key, each release a .bin on Lynovation's site by URL, size
// and SHA-256. Nothing from a list not signed by a trusted key is used, and
// a .bin must be the file the list names; one downloaded is kept and chosen
// as a file opened by hand would be. No trusted key: nothing is fetched.
void testLynovationCatalog()
{
    QTemporaryDir site;
    QTemporaryDir kept;
    FirmwareInstallModel::setFirmwareFolderForTesting(kept.filePath(QStringLiteral("kept")));
    const Key lynovation('L');
    const Key other('O');
    const QByteArray n = merged(app("v1.02.00n"), rows8());
    const QByteArray m = merged(app("v1.02.00m"), rows8());
    writeFile(site.filePath(QStringLiteral("CTR2-Max_v10200n.bin")), n);
    writeFile(site.filePath(QStringLiteral("CTR2-Max_v10200m.bin")), m);
    const auto release = [](const char* version, const char* file, const QByteArray& bin) {
        return QJsonObject{{"version", QString::fromLatin1(version)},
                           {"file", QString::fromLatin1(file)},
                           {"size", double(bin.size())},
                           {"sha256", QString::fromLatin1(
                                          QCryptographicHash::hash(bin, QCryptographicHash::Sha256)
                                              .toHex())}};
    };
    const QJsonObject product{
        {"name", "CTR2-Max firmware"}, {"device", "CTR2-Max"}, {"chip", "ESP32-S3"},
        {"releases", QJsonArray{release("v1.02.00n", "CTR2-Max_v10200n.bin", n),
                                release("v1.02.00m", "CTR2-Max_v10200m.bin", m),
                                QJsonObject{{"version", "v9"}, {"file", "x.bin"}}}}};
    const QJsonObject elsewhere{{"name", "Other"}, {"device", "CTR2-Duo"}, {"chip", "ESP32-S3"},
                                {"releases", QJsonArray{release("v1", "CTR2-Max_v10200n.bin", n)}}};
    const QByteArray json = QJsonDocument(QJsonObject{{"format", 1},
                                                      {"products", QJsonArray{product, elsewhere}}})
                                .toJson();
    const QString catalogPath = site.filePath(QStringLiteral("catalog.json"));
    writeFile(catalogPath, json);
    const QUrl base = QUrl::fromLocalFile(catalogPath);

    Ctr2MaxStockCatalog catalog;
    QString problem;
    Ctr2MaxStockCatalog::setTrustedKeysForTesting({});
    check(!Ctr2MaxStockCatalog::parse(json, lynovation.sign(json), base, &catalog, &problem),
          "with no Lynovation key built in, no list is taken");
    Ctr2MaxStockCatalog::setTrustedKeysForTesting({lynovation.pub});
    check(!Ctr2MaxStockCatalog::parse(json, other.sign(json), base, &catalog, &problem),
          "a list signed by another key is refused");
    check(!Ctr2MaxStockCatalog::parse(json + " ", lynovation.sign(json), base, &catalog, &problem),
          "a list changed after signing is refused");
    check(Ctr2MaxStockCatalog::parse(json, lynovation.sign(json), base, &catalog, &problem)
              && catalog.releases.size() == 2 && catalog.releases[0].version == "v1.02.00n"
              && catalog.releases[0].file == QUrl::fromLocalFile(site.filePath(
                                                 QStringLiteral("CTR2-Max_v10200n.bin"))),
          "Lynovation's signed list is read: the CTR2-Max's releases, newest first, by URL; "
          "incomplete entries and other devices are passed over");

    // A .bin that is not the one the list names.
    Ctr2MaxStockDownloader downloader;
    QString failure;
    bool ready = false;
    QObject::connect(&downloader, &Ctr2MaxStockDownloader::failed, &downloader,
                     [&](const QString& message) { failure = message; });
    QObject::connect(&downloader, &Ctr2MaxStockDownloader::releaseReady, &downloader,
                     [&] { ready = true; });
    Ctr2MaxStockCatalog::Release wrong = catalog.releases[0];
    wrong.sha256 = QCryptographicHash::hash("x", QCryptographicHash::Sha256);
    downloader.fetchRelease(wrong);
    QElapsedTimer t;
    t.start();
    while (failure.isEmpty() && !ready && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(!ready && !failure.isEmpty(), "a .bin whose hash is not the list's is refused");
    failure.clear();
    Ctr2MaxStockCatalog::Release shorter = catalog.releases[0];
    shorter.size -= 1;
    downloader.fetchRelease(shorter);
    t.restart();
    while (failure.isEmpty() && !ready && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(!ready && !failure.isEmpty(), "and one larger than the list says");

    // The window's model: the list arrives, a release downloads, is kept, chosen.
    writeFile(catalogPath + QStringLiteral(".sig"), lynovation.sign(json));
    Ctr2MaxStockCatalog::setCatalogUrlForTesting(base);
    FirmwareInstallModel model(nullptr);
    model.begin();
    t.restart();
    while (model.stockReleases().size() != 2 && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(model.stockReleases().size() == 2, "the window lists Lynovation's releases");
    QString keptPath;
    QObject::connect(&model, &FirmwareInstallModel::stockDownloaded, &model,
                     [&](const QString& path) { keptPath = path; });
    model.downloadStockRelease(1);   // v1.02.00m, an earlier one
    t.restart();
    while (keptPath.isEmpty() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(!keptPath.isEmpty() && model.stockFirmwarePath() == keptPath
              && model.stockFirmwareName() == QStringLiteral("CTR2-Max firmware v1.02.00m"),
          "a release downloads from Lynovation's site, and is chosen");
    check(model.storedFirmware().size() == 1 && model.storedFirmware()[0].version == "v1.02.00m",
          "and kept, to go back to later");

    // A list entry whose file is another version is refused, nothing kept.
    {
        QJsonObject wrongProduct = product;
        wrongProduct["releases"] = QJsonArray{release("v1.02.00x", "CTR2-Max_v10200n.bin", n)};
        const QByteArray wrongJson =
            QJsonDocument(QJsonObject{{"format", 1}, {"products", QJsonArray{wrongProduct}}}).toJson();
        const QString wrongPath = site.filePath(QStringLiteral("wrong.json"));
        writeFile(wrongPath, wrongJson);
        writeFile(wrongPath + QStringLiteral(".sig"), lynovation.sign(wrongJson));
        Ctr2MaxStockCatalog::setCatalogUrlForTesting(QUrl::fromLocalFile(wrongPath));
        FirmwareInstallModel mislabeled(nullptr);
        mislabeled.begin();
        t.restart();
        while (mislabeled.stockReleases().isEmpty() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QString why;
        QObject::connect(&mislabeled, &FirmwareInstallModel::stockDownloadFailed, &mislabeled,
                         [&](const QString& message) { why = message; });
        const int keptBefore = mislabeled.storedFirmware().size();
        mislabeled.downloadStockRelease(0);
        t.restart();
        while (why.isEmpty() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        check(why.contains(QStringLiteral("v1.02.00x")) && why.contains(QStringLiteral("v1.02.00n"))
                  && mislabeled.storedFirmware().size() == keptBefore,
              "a file that is not the version the list names is refused, and not kept");
        Ctr2MaxStockCatalog::setCatalogUrlForTesting(base);
    }

    // A kept file removed from the folder is gone from the list next time.
    QFile::remove(keptPath);
    model.begin();
    check(model.storedFirmware().isEmpty(), "a kept file removed by hand leaves the list");

    // No trusted key: the window fetches nothing.
    Ctr2MaxStockCatalog::setTrustedKeysForTesting({});
    FirmwareInstallModel unkeyed(nullptr);
    unkeyed.begin();
    check(!unkeyed.downloadingStockRelease(), "with no Lynovation key, nothing is fetched");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    check(unkeyed.stockReleases().isEmpty(), "and nothing is listed");

    Ctr2MaxStockCatalog::setCatalogUrlForTesting({});
    Ctr2MaxStockCatalog::setTrustedKeysForTesting({});
    FirmwareInstallModel::setFirmwareFolderForTesting({});
}

} // namespace

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    // The AetherCTR2 release list a model fetches when it begins: a file that
    // is not there, so no test reaches the network.
    QTemporaryDir nowhere;
    EspFirmwareCatalog::setCatalogUrlForTesting(
        QUrl::fromLocalFile(nowhere.filePath(QStringLiteral("none/catalog.json"))));
    QCoreApplication app(argc, argv);
    testReadFile();
    testInstallKeepsEverythingElse();
    testUnitLayoutMustAgree();
    testWindowFlow();
    testReleaseAndRestore();
    testFirmwareStore();
    testRestartInto();
    testVersionOrder();
    testLynovationCatalog();
    if (g_failures)
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
    else
        std::printf("ctr2max_stock_firmware_test: all passed\n");
    return g_failures ? 1 : 0;
}
