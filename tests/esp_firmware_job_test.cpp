// EspFirmwareJob against a flash in memory: it acts only on the device the
// operator confirmed (checked by MAC once the port is open, before anything
// is written), and refuses a device whose MAC cannot be read. No serial port.

#include "core/EspFirmwareJob.h"
#include "core/EspFlashTarget.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>

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

constexpr quint32 kFlash = 0x10000;

struct Log {
    std::atomic<int> writes{0};
};

class MemFlash : public EspFlashTarget {
public:
    MemFlash(QByteArray mac, std::shared_ptr<Log> log) : m_mac(std::move(mac)), m_log(std::move(log)) {}
    QString chipName() const override { return QStringLiteral("ESP32-S3"); }
    QByteArray mac() const override { return m_mac; }
    quint32 flashSize() const override { return kFlash; }
    bool read(quint32 address, quint32 length, QByteArray* out, const Progress&, QString*) override
    {
        *out = m_flash.mid(address, length);
        return true;
    }
    bool write(quint32 address, const QByteArray& data, const Progress&, QString*) override
    {
        m_flash.replace(address, data.size(), data);
        ++m_log->writes;
        return true;
    }
    bool matches(quint32, const QByteArray&) override { return false; }
    bool restart(QString*) override { return true; }

private:
    QByteArray m_mac;
    std::shared_ptr<Log> m_log;
    QByteArray m_flash = QByteArray(kFlash, '\x5A');
};

struct Outcome {
    bool done{false};
    bool ok{false};
    QString message;
};

// Runs one job against a device with deviceMac.
Outcome run(EspFirmwareJob::Request request, const QByteArray& deviceMac, std::shared_ptr<Log> log)
{
    EspFirmwareJob job;
    job.setTargetOpener([deviceMac, log](const QString&, QString*, bool*) {
        return std::unique_ptr<EspFlashTarget>(new MemFlash(deviceMac, log));
    });
    Outcome o;
    QObject::connect(&job, &EspFirmwareJob::finished, [&o](bool ok, const QString& message) {
        o = {true, ok, message};
    });
    check(job.start(request), "the job starts");
    QElapsedTimer t;
    t.start();
    while (!o.done && t.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    check(o.done, "the job finishes");
    return o;
}

// A backup of the device with mac, with its description beside it.
QString makeBackup(const QTemporaryDir& dir, const QByteArray& mac)
{
    const QByteArray flash(kFlash, '\x11');
    const QString path = dir.filePath(QStringLiteral("backup.bin"));
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(flash);
    f.close();
    QFile json(path + QStringLiteral(".json"));
    json.open(QIODevice::WriteOnly);
    json.write(QJsonDocument(QJsonObject{
                                 {"chip", "ESP32-S3"},
                                 {"mac", QString::fromLatin1(mac.toHex(':'))},
                                 {"flashSize", double(kFlash)},
                                 {"sha256", QString::fromLatin1(QCryptographicHash::hash(
                                                flash, QCryptographicHash::Sha256).toHex())},
                             })
                   .toJson());
    return path;
}

void testConfirmedDeviceOnly()
{
    const QByteArray a = QByteArray::fromHex("AAAAAAAAAAAA");
    const QByteArray b = QByteArray::fromHex("BBBBBBBBBBBB");
    QTemporaryDir dir;

    // The operator confirmed A; B is the device on the port now.
    auto log = std::make_shared<Log>();
    EspFirmwareJob::Request r;
    r.portName = QStringLiteral("ttyTEST");
    r.action = EspFirmwareJob::Action::Restore;
    r.restoreFrom = makeBackup(dir, b);   // even a backup that would fit B
    r.expectedMac = a;
    Outcome o = run(r, b, log);
    check(!o.ok && o.message.contains(QStringLiteral("not")) && log->writes == 0,
          "another device than the one confirmed: nothing is written");

    r.action = EspFirmwareJob::Action::Backup;
    r.backupPath = dir.filePath(QStringLiteral("drift.bin"));
    o = run(r, b, log);
    check(!o.ok && !QFile::exists(r.backupPath), "nor is it backed up");

    // The confirmed device itself.
    r.action = EspFirmwareJob::Action::Restore;
    r.expectedMac = b;
    o = run(r, b, log);
    check(o.ok && log->writes > 0, "the confirmed device is restored");
}

void testUnreadableMac()
{
    QTemporaryDir dir;
    auto log = std::make_shared<Log>();
    EspFirmwareJob::Request r;
    r.portName = QStringLiteral("ttyTEST");
    r.action = EspFirmwareJob::Action::Backup;
    r.backupPath = dir.filePath(QStringLiteral("nomac.bin"));
    const Outcome o = run(r, {}, log);
    check(!o.ok && o.message.contains(QStringLiteral("MAC")) && !QFile::exists(r.backupPath),
          "a device whose MAC cannot be read is not backed up: its backup could never be restored");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testConfirmedDeviceOnly();
    testUnreadableMac();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("esp_firmware_job_test: all checks passed\n");
    return 0;
}
