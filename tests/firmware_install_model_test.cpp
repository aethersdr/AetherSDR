// FirmwareInstallModel acts only on the device the operator confirmed: an
// action captured for one CTR2-Max is refused, with nothing opened or
// written, when another is connected by the time it is confirmed (even on
// the same port name), and a port that turns out to hold another device is
// refused by MAC before anything is written. Injected device lists and an
// in-memory flash; no serial port, no network.

#include "TestSettingsProfile.h"
#include "core/EspFlashTarget.h"
#include "models/Ctr2ProxyModel.h"
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
    model.setJobTargetOpener([onPort, log](const QString&, QString*, bool*) {
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

// With two controllers connected, the window shows the one the relay has
// selected: an update over the link goes to that one.
void testFollowsRelaySelection()
{
    using Info = Ctr2HidPort::DeviceInfo;
    const Info first{QStringLiteral("hid-1"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                     QStringLiteral("111111111111")};
    const Info second{QStringLiteral("hid-2"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                      QStringLiteral("222222222222")};
    Ctr2ProxyModel relay;
    relay.setUsbBackend([=] { return QList<Info>{first, second}; },
                        [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(nullptr); });
    relay.setTransport(Ctr2ProxyModel::Transport::Usb);
    relay.setUsbDevicePath(second.path);
    FirmwareInstallModel model(&relay);
    model.setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                           [=] { return QList<Info>{first, second}; });
    check(model.device() == FirmwareInstallModel::Device::LinkFirmware
              && model.deviceMac() == QStringLiteral("22:22:22:22:22:22"),
          "the relay's selected controller is the one shown");
}

// Only CTR2-Max units are shown: an AetherKnob is not one, and does not
// hide a CTR2-Max behind it. A link action goes to the relay's device, so it
// is refused when the relay holds another one.
void testCtr2MaxOnly()
{
    using Info = Ctr2HidPort::DeviceInfo;
    const Info knob{QStringLiteral("knob"), 0x303A, 0x1001, {}, QStringLiteral("AETHER_KNOB"),
                    QStringLiteral("333333333333")};
    const Info max{QStringLiteral("max"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                   QStringLiteral("444444444444")};
    Ctr2ProxyModel relay;
    relay.setUsbBackend([=] { return QList<Info>{knob, max}; },
                        [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(nullptr); });
    relay.setTransport(Ctr2ProxyModel::Transport::Usb);
    relay.setUsbDevicePath(knob.path);
    FirmwareInstallModel model(&relay);
    model.setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                           [=] { return QList<Info>{knob, max}; });
    check(model.device() == FirmwareInstallModel::Device::LinkFirmware
              && model.deviceMac() == QStringLiteral("44:44:44:44:44:44"),
          "the CTR2-Max is shown, not the AetherKnob before it");
    QString problem;
    check(!model.restart(model.target(), true, &problem)
              && problem.contains(QStringLiteral("another device")),
          "a restart is refused while the relay holds the AetherKnob");

    FirmwareInstallModel knobOnly(nullptr);
    knobOnly.setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                              [=] { return QList<Info>{knob}; });
    check(knobOnly.device() == FirmwareInstallModel::Device::None,
          "an AetherKnob alone is not a CTR2-Max, stock or otherwise");
}

// A device in download mode whose MAC is not known is not acted on: the job
// could not tell it from another on the same port.
void testUnknownMac()
{
    FirmwareInstallModel model(nullptr);
    model.setDeviceSources([] {
        return QList<FirmwareInstallModel::Loader>{{QStringLiteral("ttyACM0"), QString()}};
    }, [] { return QList<Ctr2HidPort::DeviceInfo>(); });
    QTemporaryDir dir;
    QString problem;
    check(!model.backup(model.target(), dir.filePath(QStringLiteral("x.bin")), &problem)
              && problem.contains(QStringLiteral("MAC")),
          "no MAC, no action");
}

} // namespace

// The CTR2 window offers the Linux access rule only when the OS refused the
// port: a typed flag from the opener, never the wording of the message.
void testAccessDenied()
{
    using Loader = FirmwareInstallModel::Loader;
    const QString a = QStringLiteral("AA:AA:AA:AA:AA:AA");
    auto denied = std::make_shared<bool>(true);
    QTemporaryDir dir;
    FirmwareInstallModel model(nullptr);
    model.setJobTargetOpener([denied](const QString& port, QString* problem, bool* accessDenied) {
        // Same words either way: only the flag may tell them apart.
        *problem = QStringLiteral("Permission to open %1 was denied.").arg(port);
        *accessDenied = *denied;
        return std::unique_ptr<EspFlashTarget>();
    });
    model.setDeviceSources([a] { return QList<Loader>{{QStringLiteral("ttyACM0"), a}}; },
                           [] { return QList<Ctr2HidPort::DeviceInfo>(); });
    QString problem;
    bool ok = true;
    QString message;
    check(model.backup(model.target(), dir.filePath(QStringLiteral("a.bin")), &problem),
          "a backup starts");
    check(waitFinished(model, &ok, &message) && !ok && model.lastFailureAccessDenied(),
          "a port the OS refused is reported as access denied");
    *denied = false;
    check(model.backup(model.target(), dir.filePath(QStringLiteral("b.bin")), &problem),
          "another backup starts");
    ok = true;
    check(waitFinished(model, &ok, &message) && !ok && !model.lastFailureAccessDenied(),
          "any other failure is not, whatever its message says");
}

int main(int argc, char** argv)
{
    // The relay model reads its saved setup: never the operator's.
    TestSettingsProfile profile(QStringLiteral("firmware_install_model_test"));
    QCoreApplication app(argc, argv);
    testConfirmedDevice();
    testAccessDenied();
    testFollowsRelaySelection();
    testCtr2MaxOnly();
    testUnknownMac();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("firmware_install_model_test: all checks passed\n");
    return 0;
}
