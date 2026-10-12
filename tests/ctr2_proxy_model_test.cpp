// Ctr2ProxyModel destination handling and device selection: the relay targets
// only the radio its owner pushes in, every unusable case keeps Start
// disabled with its reason, and each checked USB device gets its own relay.
// Socket-free: USB ports are injected, nothing is bound, and the radio is
// never reached (no CTR2 answers the host's call).

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidPort.h"
#include "models/Ctr2ProxyModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>
#include <vector>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

using AetherSDR::Ctr2ProxyModel;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct PortLog {
    int opened{0};
    int shutdowns{0};
    bool openOnCreate{true};
    bool closedWithClosed{false};
};

// Injected HID port: records whether the relay told the CTR2 CLOSED on close.
class FakePort : public AetherSDR::Ctr2HidPort {
public:
    explicit FakePort(std::shared_ptr<PortLog> log) : m_log(std::move(log)), m_open(m_log->openOnCreate)
    {
        ++m_log->opened;
    }
    bool isOpen() const override { return m_open; }
    void send(const std::vector<AetherSDR::ctr2hid::Report>&) override {}
    void discardQueued() override {}
    void shutdown(const std::vector<AetherSDR::ctr2hid::Report>& final) override
    {
        ++m_log->shutdowns;
        m_log->closedWithClosed = final.size() == 1
            && final[0][3] == static_cast<std::uint8_t>(AetherSDR::ctr2hid::MessageType::Closed);
        m_open = false;
        deleteLater();
    }
    QString description() const override { return QStringLiteral("Fake CTR2"); }

private:
    std::shared_ptr<PortLog> m_log;
    bool m_open{true};
};

void testRelayFollowsAetherSdrsRadio()
{
    Ctr2ProxyModel model;
    auto log = std::make_shared<PortLog>();
    AetherSDR::Ctr2HidPort::DeviceInfo ctr2{QStringLiteral("fake-path"), 0x303A, 0x1001,
                                            {}, QStringLiteral("ESP32S3_DEV"), {}};
    model.setUsbBackend([ctr2] { return QList<AetherSDR::Ctr2HidPort::DeviceInfo>{ctr2}; },
                        [log](const AetherSDR::Ctr2HidPort::DeviceInfo&, QString*) {
                            return static_cast<AetherSDR::Ctr2HidPort*>(new FakePort(log));
                        });
    model.setWifiEnabled(false);
    check(model.isUsbDeviceSelected(ctr2.path), "the recognized CTR2 is checked");
    const QHostAddress radioA(QStringLiteral("192.0.2.10"));
    const QHostAddress radioB(QStringLiteral("192.0.2.11"));
    model.setAetherRadio(radioA, 4992, QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(model.configurationProblem().isEmpty(), "USB relay is ready with a radio and a device");
    check(model.start() && model.isRunning() && log->opened == 1, "relay starts on the pushed radio");
    check(model.radioEndpoint() == QStringLiteral("192.0.2.10:4992"), "it targets that radio on 4992");

    model.setAetherRadio(radioA, 4992, QStringLiteral("FLEX-8600 \"Renamed\"  192.0.2.10"), {});
    check(model.isRunning(), "an info update about the same radio keeps the relay running");

    model.setAetherRadio(radioB, 4992, QStringLiteral("FLEX-6600  192.0.2.11"), {});
    check(!model.isRunning(), "switching radios in AetherSDR stops the relay");
    check(log->closedWithClosed, "the CTR2 is told CLOSED when the relay is torn down");
    check(model.lastError().contains(QStringLiteral("switched from")), "the reason is shown");
    check(model.configurationProblem().isEmpty(), "the operator may start again on the new radio");

    log->closedWithClosed = false;
    check(model.start() && model.radioEndpoint() == QStringLiteral("192.0.2.11:4992"),
          "a new Start captures the new radio");
    check(model.lastError().isEmpty(), "starting clears the old stop reason");
    model.setAetherRadio({}, 0, {}, {});
    check(!model.isRunning() && log->closedWithClosed, "disconnecting AetherSDR stops the relay");
    check(model.lastError().contains(QStringLiteral("no longer connected")), "the disconnect reason is shown");

    model.setAetherRadio(radioA, 5000, QStringLiteral("FLEX-8600  192.0.2.10"), {});
    check(model.start() && model.radioEndpoint() == QStringLiteral("192.0.2.10:5000"),
          "the radio's own port is used, not an assumed 4992");
    model.setAetherRadio(radioA, 4992, QStringLiteral("FLEX-8600  192.0.2.10"), {});
    check(!model.isRunning(), "a different port on the same address is a different radio endpoint");
    check(model.start(), "restart after reconnect");
    model.stop();
    check(!model.isRunning() && model.lastError().isEmpty(), "a manual Stop leaves no error behind");

    // A port the relay refuses is shut down, never deleted in place: deleting
    // waits on its I/O thread, which may still be waiting for the last port.
    log->openOnCreate = false;
    const int shutdownsBefore = log->shutdowns;
    check(!model.start() && log->shutdowns == shutdownsBefore + 1,
          "a refused port is handed back with shutdown(), which never blocks");
    QCoreApplication::processEvents();
}

// The packaged udev rule grants exactly the USB identities the app labels as
// a CTR2: a recognised device the rule misses would loop forever on the
// install prompt, and an extra one would open an unrelated ESP32-S3 board.
void testUdevRuleMatchesKnownCtr2s()
{
    QFile file(QStringLiteral(AETHER_CTR2_UDEV_RULES));
    check(file.open(QIODevice::ReadOnly | QIODevice::Text), "the packaged CTR2 udev rule is readable");
    const auto key = [](quint16 vid, quint16 pid, const QString& product) {
        return QStringLiteral("%1:%2:%3")
            .arg(vid, 4, 16, QLatin1Char('0')).arg(pid, 4, 16, QLatin1Char('0')).arg(product);
    };
    static const QRegularExpression attr(QStringLiteral(R"re(ATTRS\{(idVendor|idProduct|product)\}=="([^"]*)")re"));
    QSet<QString> granted;
    for (const QString& line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#'))) {
            continue;
        }
        QString vid, pid, products;
        for (auto it = attr.globalMatch(line); it.hasNext();) {
            const auto m = it.next();
            (m.captured(1) == QLatin1String("idVendor") ? vid
             : m.captured(1) == QLatin1String("idProduct") ? pid : products) = m.captured(2);
        }
        check(!vid.isEmpty() && !pid.isEmpty() && !products.isEmpty(),
              "every rule line pins VID, PID and product string");
        for (const QString& product : products.split(QLatin1Char('|'))) {
            granted.insert(key(vid.toUShort(nullptr, 16), pid.toUShort(nullptr, 16), product));
        }
    }
    QSet<QString> known;
    for (const auto& d : AetherSDR::Ctr2HidPort::knownCtr2Devices()) {
        known.insert(key(d.vendorId, d.productId, QString::fromLatin1(d.product)));
    }
    for (const QString& k : known - granted) {
        std::fprintf(stderr, "  recognised but not granted: %s\n", qPrintable(k));
    }
    for (const QString& k : granted - known) {
        std::fprintf(stderr, "  granted but not recognised: %s\n", qPrintable(k));
    }
    check(granted == known, "the udev rule grants exactly the recognised CTR2 devices");
}

// USB is the default when the build can open a CTR2: Wi-Fi starts unchecked
// and every recognized CTR2 is checked. A rescan keeps the operator's choices.
void testUsbDefaults()
{
    Ctr2ProxyModel fresh;
    check(fresh.wifiEnabled() != fresh.usbAvailable(),
          "Wi-Fi starts checked exactly when USB HID is unavailable");

    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info other{QStringLiteral("other"), 0x1234, 0x5678, {}, QStringLiteral("Keyboard"), {}};
    const Info first{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    const Info second{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    auto devices = std::make_shared<QList<Info>>(QList<Info>{other, first, second});
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return *devices; },
                        [](const Info&, QString*) { return static_cast<AetherSDR::Ctr2HidPort*>(nullptr); });
    check(model.selectedUsbDevices() == QStringList({first.path, second.path}),
          "every recognized CTR2 is checked, in device order");
    check(!model.isUsbDeviceSelected(other.path), "an unrecognized device is never checked");

    check(model.setUsbDeviceSelected(second.path, false), "a device can be unchecked");
    check(model.setUsbDeviceSelected(other.path, true), "an unrecognized device can be checked by hand");
    check(!model.setUsbDeviceSelected(QStringLiteral("absent"), true), "an absent device cannot be checked");
    const Info third{QStringLiteral("ctr2-c"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    devices->append(third);
    model.refreshDevices();
    check(model.selectedUsbDevices() == QStringList({other.path, first.path, third.path}),
          "a rescan keeps the operator's choices and checks the new CTR2");

    devices->removeIf([&](const Info& d) { return d.path == first.path; });
    model.refreshDevices();
    check(model.selectedUsbDevices() == QStringList({other.path, third.path}),
          "a vanished device leaves the selection");

    for (const QString& path : model.selectedUsbDevices()) {
        model.setUsbDeviceSelected(path, false);
    }
    model.setWifiEnabled(false);
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992, QStringLiteral("radio"), {});
    check(model.configurationProblem().contains(QStringLiteral("enable Wi-Fi")),
          "with nothing checked, Start asks for a device or Wi-Fi");
}

// Each checked device gets its own relay: one that cannot be opened keeps its
// error and does not hold back the others, and Stop tears every one down.
void testMultipleUsbDevices()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info a{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    const Info b{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    const Info broken{QStringLiteral("ctr2-broken"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    auto logA = std::make_shared<PortLog>();
    auto logB = std::make_shared<PortLog>();
    Ctr2ProxyModel model;
    model.setUsbBackend([=] { return QList<Info>{a, b, broken}; },
                        [=](const Info& d, QString* error) -> AetherSDR::Ctr2HidPort* {
                            if (d.path == a.path) {
                                return new FakePort(logA);
                            }
                            if (d.path == b.path) {
                                return new FakePort(logB);
                            }
                            *error = QStringLiteral("Permission denied");
                            return nullptr;
                        });
    model.setWifiEnabled(false);
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992, QStringLiteral("radio"), {});
    check(model.selectedUsbDevices().size() == 3, "all three CTR2s are checked");
    check(model.links().isEmpty(), "no links before Start");

    check(model.start() && model.isRunning(), "Start succeeds while any device starts");
    check(logA->opened == 1 && logB->opened == 1, "every openable device is opened once");
    const auto links = model.links();
    check(links.size() == 3, "one link per checked device");
    int waiting = 0;
    int failed = 0;
    for (const auto& link : links) {
        check(link.kind == Ctr2ProxyModel::Link::Kind::Usb, "USB links only");
        waiting += link.state == AetherSDR::TcpByteProxy::State::Listening ? 1 : 0;
        if (link.state == AetherSDR::TcpByteProxy::State::Error) {
            ++failed;
            check(link.error == QStringLiteral("Permission denied"), "the open error stays on its link");
        }
    }
    check(waiting == 2 && failed == 1, "two devices are called, one failed to open");
    check(model.lastError() == QStringLiteral("Permission denied"),
          "the model surfaces the first link error");
    check(!model.setWifiEnabled(true) && !model.setUsbDeviceSelected(a.path, false),
          "configuration is frozen while running");

    model.stop();
    check(!model.isRunning() && model.links().isEmpty(), "Stop clears every link");
    check(logA->shutdowns == 1 && logB->shutdowns == 1 && logA->closedWithClosed
              && logB->closedWithClosed,
          "every device is released and told CLOSED");
    QCoreApplication::processEvents();

    model.setAetherRadio({}, 0, {}, {});
    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "the radio comes before any device problem");
}

// The Start-time udev prompt keys off this: a node the user may not open
// needs the rule; a missing node, an open one, or Wi-Fi mode does not.
void testUsbAccessRuleDetection()
{
#ifdef Q_OS_LINUX
    if (::geteuid() == 0) {
        std::printf("ctr2_proxy_model_test: root ignores permissions, access-rule checks skipped\n");
        return;
    }
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir for the fake device node");
    const QString locked = dir.filePath(QStringLiteral("hidraw-locked"));
    const QString open = dir.filePath(QStringLiteral("hidraw-open"));
    const QString missing = dir.filePath(QStringLiteral("hidraw-missing"));
    for (const QString& path : {locked, open}) {
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "create fake device node");
    }
    QFile::setPermissions(locked, QFileDevice::Permissions());

    QList<AetherSDR::Ctr2HidPort::DeviceInfo> devices;
    for (const QString& path : {locked, open, missing}) {
        devices.append({path, 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}});
    }
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return devices; },
                        [](const AetherSDR::Ctr2HidPort::DeviceInfo&, QString*) {
                            return static_cast<AetherSDR::Ctr2HidPort*>(nullptr);
                        });
    for (const QString& path : {locked, open, missing}) {
        model.setUsbDeviceSelected(path, false);
    }
    check(!model.usbDeviceNeedsAccessRule(), "no device checked needs no rule");
    model.setUsbDeviceSelected(locked, true);
    check(model.usbDeviceNeedsAccessRule(), "a node the user cannot open needs the rule");
    model.setUsbDeviceSelected(open, true);
    check(model.usbDeviceNeedsAccessRule(), "one locked node among several still needs the rule");
    model.setUsbDeviceSelected(locked, false);
    check(!model.usbDeviceNeedsAccessRule(), "an openable node needs no rule");
    model.setUsbDeviceSelected(missing, true);
    check(!model.usbDeviceNeedsAccessRule(), "a vanished node is not a permissions problem");
#endif
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Ctr2ProxyModel model;

    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "with no radio pushed, Start explains that AetherSDR must connect first");
    check(!model.start() && !model.isRunning(), "Start refuses without a radio");

    const QString smartLink = QStringLiteral("AetherSDR is connected through SmartLink");
    model.setAetherRadio({}, 0, {}, smartLink);
    check(model.configurationProblem() == smartLink, "the pushed reason is shown as-is");
    model.setWifiEnabled(true);

    model.setAetherRadio(QHostAddress(QStringLiteral("0.0.0.0")), 4992, QStringLiteral("bogus"), {});
    check(!model.configurationProblem().isEmpty() && !model.start(),
          "an unspecified address is never a destination");
    model.setAetherRadio(QHostAddress(QStringLiteral("fe80::1")), 4992, QStringLiteral("v6"), {});
    check(!model.configurationProblem().isEmpty(), "IPv6 is out of scope and stays unavailable");

    int changes = 0;
    QObject::connect(&model, &Ctr2ProxyModel::configurationChanged, &model, [&] { ++changes; });
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "a new radio is announced once");
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "re-pushing the same radio is not a change");
    check(model.aetherRadioLabel() == QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"),
          "the radio label is exposed for display");
    const QString wifi = model.configurationProblem();
    check(!wifi.contains(QStringLiteral("radio"), Qt::CaseInsensitive),
          "with a radio pushed, Wi-Fi only asks for its own settings");
    check(wifi == QStringLiteral("Select the local address the CTR2 will connect to"),
          "Wi-Fi needs an explicit listen address");

    model.setAetherRadio({}, 0, {}, {});
    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "losing the radio falls back to the default reason");

    testRelayFollowsAetherSdrsRadio();
    testUsbAccessRuleDetection();
    testUsbDefaults();
    testMultipleUsbDevices();
    testUdevRuleMatchesKnownCtr2s();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_proxy_model_test: all checks passed\n");
    return 0;
}
