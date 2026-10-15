// FirmwareInstallModel acts only on the device the operator confirmed: an
// action captured for one CTR2-Max is refused, with nothing opened or
// written, when another is connected by the time it is confirmed (even on
// the same port name), and a port that turns out to hold another device is
// refused by MAC before anything is written. Injected device lists and an
// in-memory flash; no serial port, no network.

#include "core/EspFlashTarget.h"
#include "models/FirmwareInstallModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
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

struct Log {
    std::atomic<int> opens{0};
    std::atomic<int> writes{0};
};

class MemFlash : public EspFlashTarget {
public:
    MemFlash(QByteArray mac, std::shared_ptr<Log> log) : m_mac(std::move(mac)), m_log(std::move(log)) {}
    QString chipName() const override { return QStringLiteral("ESP32-S3"); }
    QByteArray mac() const override { return m_mac; }
    quint32 flashSize() const override { return 0x10000; }
    bool read(quint32 address, quint32 length, QByteArray* out, const Progress&, QString*) override
    {
        *out = QByteArray(0x10000, '\x22').mid(address, length);
        return true;
    }
    bool write(quint32, const QByteArray&, const Progress&, QString*) override
    {
        ++m_log->writes;
        return true;
    }
    bool matches(quint32, const QByteArray&) override { return false; }
    bool restart(QString*) override { return true; }

private:
    QByteArray m_mac;
    std::shared_ptr<Log> m_log;
};

bool waitFinished(FirmwareInstallModel& model, bool* ok, QString* message)
{
    bool done = false;
    QObject::connect(&model, &FirmwareInstallModel::finished, &model,
                     [&](bool o, const QString& m) {
                         done = true;
                         *ok = o;
                         *message = m;
                     });
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done;
}

void testConfirmedDevice()
{
    using Loader = FirmwareInstallModel::Loader;
    const QString port = QStringLiteral("ttyACM0");
    const QString a = QStringLiteral("AA:AA:AA:AA:AA:AA");
    const QString b = QStringLiteral("BB:BB:BB:BB:BB:BB");
    auto loaders = std::make_shared<QList<Loader>>(QList<Loader>{{port, a}});
    auto onPort = std::make_shared<QByteArray>(QByteArray::fromHex("AAAAAAAAAAAA"));
    auto log = std::make_shared<Log>();
    QTemporaryDir dir;

    FirmwareInstallModel model(nullptr);
    model.setJobTargetOpener([onPort, log](const QString&, QString*) {
        ++log->opens;
        return std::unique_ptr<EspFlashTarget>(new MemFlash(*onPort, log));
    });
    model.setDeviceSources([loaders] { return *loaders; },
                           [] { return QList<Ctr2HidPort::DeviceInfo>(); });
    model.setReleaseForTesting(EspFirmwareManifest{}, {});
    check(model.device() == FirmwareInstallModel::Device::Bootloader && model.deviceMac() == a,
          "the device in download mode is found");

    // The operator starts an action on A; B replaces it, on the same port name,
    // while the confirmation is open.
    const FirmwareInstallModel::Target target = model.target();
    *loaders = {{port, b}};
    *onPort = QByteArray::fromHex("BBBBBBBBBBBB");
    QString problem;
    check(!model.install(target, {}, &problem) && problem.contains(QStringLiteral("changed")),
          "an install confirmed for A is refused once B is connected");
    const auto refused = [&problem] { return problem.contains(QStringLiteral("changed")); };
    problem.clear();
    check(!model.backup(target, dir.filePath(QStringLiteral("b.bin")), &problem) && refused(),
          "and a backup");
    problem.clear();
    check(!model.restore(target, dir.filePath(QStringLiteral("a.bin")), &problem) && refused(),
          "and a restore");
    problem.clear();
    check(!model.startFirmware(target, &problem) && refused(), "and a start");
    check(log->opens == 0 && log->writes == 0, "nothing was opened or written");

    // The list still shows A, but the port now holds B: the job's MAC check.
    *loaders = {{port, a}};
    model.setDeviceSources([loaders] { return *loaders; },   // looks again, as its poll does
                           [] { return QList<Ctr2HidPort::DeviceInfo>(); });
    const FirmwareInstallModel::Target again = model.target();
    problem.clear();
    check(model.backup(again, dir.filePath(QStringLiteral("a.bin")), &problem),
          "a backup of A starts");
    bool ok = true;
    QString message;
    check(waitFinished(model, &ok, &message), "and finishes");
    check(!ok && message.contains(QStringLiteral("not")) && log->writes == 0,
          "but B, found on the port, is refused by its MAC before anything is written");

    // A itself.
    *onPort = QByteArray::fromHex("AAAAAAAAAAAA");
    check(model.backup(model.target(), dir.filePath(QStringLiteral("a.bin")), &problem),
          "a backup of A, with A on the port");
    check(waitFinished(model, &ok, &message) && ok, "succeeds");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testConfirmedDevice();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("firmware_install_model_test: all checks passed\n");
    return 0;
}
