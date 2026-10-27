// Ctr2ProxyModel destination handling: the relay targets only the radio its
// owner pushes in, and every unusable case keeps Start disabled with the
// pushed reason. Socket-free: nothing is bound, and a relay that reaches
// its radio connection gets an injected socket.

#include "TestSettingsProfile.h"
#include "core/Ctr2HidFraming.h"
#include "core/AppSettings.h"
#include "core/Ctr2RelaySettings.h"
#include "core/Ctr2HidPort.h"
#include "core/Ctr2UsbRelay.h"
#include "models/Ctr2ProxyModel.h"
#include "gui/Ctr2AccessRule.h"
#include "gui/Ctr2ActionRetry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QSemaphore>
#include <QTimer>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>
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

// A radio socket that never connects: a test whose CTR2 answers READY keeps
// the relay Connecting without reaching the network.
class InertRadioSocket final : public QTcpSocket {
public:
    using QTcpSocket::connectToHost;
    void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                       NetworkLayerProtocol = AnyIPProtocol) override
    {
    }
};

void injectInertRadio(Ctr2ProxyModel& model)
{
    auto* relay = model.findChild<AetherSDR::Ctr2UsbRelay*>();
    check(relay != nullptr, "the model owns a USB relay");
    if (relay)
        relay->setRadioSocketFactory([] { return new InertRadioSocket; });
}

// A relay port whose device offers link extensions it did not take.
class OfferPort : public FakePort {
public:
    using FakePort::FakePort;
    void offer(quint32 offered) { setExtensions(0, std::nullopt, offered); }
};

// Injected firmware-probe port: the test decides when negotiation finishes.
class ProbePort : public AetherSDR::Ctr2HidPort {
public:
    explicit ProbePort(QString path, std::shared_ptr<PortLog> log)
        : m_path(std::move(path)), m_log(std::move(log))
    {
        ++m_log->opened;
    }
    bool isOpen() const override { return true; }
    void send(const std::vector<AetherSDR::ctr2hid::Report>&) override {}
    void discardQueued() override {}
    void shutdown(const std::vector<AetherSDR::ctr2hid::Report>&) override
    {
        ++m_log->shutdowns;
        deleteLater();
    }
    QString description() const override { return m_path; }
    void negotiate(int major, quint32 offered = 0)
    {
        AetherSDR::ctr2hid::deviceinfo::Info info;
        info.major = major;
        setExtensions(0, info, offered);
    }
    QString path() const { return m_path; }

private:
    QString m_path;
    std::shared_ptr<PortLog> m_log;
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
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setUsbDevicePath(ctr2.path);
    const QHostAddress radioA(QStringLiteral("192.0.2.10"));
    const QHostAddress radioB(QStringLiteral("192.0.2.11"));
    model.setAetherRadio(radioA, 4992, QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(model.configurationProblem().isEmpty(), "USB relay is ready with a radio and a device");
    check(model.start() && model.isRunning() && log->opened == 1, "relay starts on the pushed radio");
    check(model.radioEndpoint() == QStringLiteral("192.0.2.10:4992"), "it targets that radio on 4992");
    check(model.peerEndpoint() == ctr2.shortName() && !model.peerEndpoint().contains(QLatin1Char('(')),
          "the status names the device, without the IDs the list shows");

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

// The packaged udev rule lists exactly the product patterns the app labels as
// a CTR2: a recognised device the rule misses would loop forever on the
// install prompt, and an extra one would open an unrelated ESP32-S3 board.
// This compares the patterns, not the devices they match: udev's "Aether*"
// also matches names the app refuses, which testSelfNamedDevices() pins.
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
    QStringList ttyLines;
    for (const QString& line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#'))) {
            continue;
        }
        // The download-mode serial port is not a CTR2 HID device: checked below.
        if (line.contains(QLatin1String("SUBSYSTEM==\"tty\""))) {
            ttyLines << line;
            continue;
        }
        check(line.contains(QLatin1String("SUBSYSTEM==\"hidraw\"")),
              "every other rule line is for hidraw");
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
    check(granted == known, "the udev rule lists exactly the recognized CTR2 product patterns");

    // One tty line: the ESP32-S3 ROM's USB serial port, which is what the
    // CTR2-Max is in download mode, opened to the seat's user only.
    check(ttyLines.size() == 1, "the rule has one tty line, for download mode");
    const QString tty = ttyLines.value(0);
    check(tty.contains(QLatin1String("ATTRS{idVendor}==\"303a\""))
              && tty.contains(QLatin1String("ATTRS{idProduct}==\"1001\""))
              && tty.contains(QLatin1String("ATTRS{product}==\"USB JTAG/serial debug unit\""))
              && tty.contains(QLatin1String("TAG+=\"uaccess\""))
              && !tty.contains(QLatin1String("MODE")) && !tty.contains(QLatin1String("GROUP")),
          "the tty line pins the ROM's USB serial identity and grants only the seat's user");

    // Every name the app recognizes must be one the rule's glob grants, or a
    // self-named device would loop on the install prompt.
    const QStringList probes{QStringLiteral("AetherX"), QStringLiteral("AetherDial"), QStringLiteral("Aether ~"),
                             QStringLiteral("Aether") + QString(26, QLatin1Char('x'))};
    for (const QString& probe : probes) {
        const AetherSDR::Ctr2HidPort::DeviceInfo info{QStringLiteral("p"), 0x303A, 0x1001, {}, probe, {}};
        bool covered = false;
        for (const QString& g : granted) {
            const QString prefix = key(0x303A, 0x1001, QString());
            const QRegularExpression glob(QRegularExpression::wildcardToRegularExpression(
                g.mid(prefix.size()), QRegularExpression::NonPathWildcardConversion));
            covered = covered || (g.startsWith(prefix) && glob.match(probe).hasMatch());
        }
        check(!info.ctr2Model().isEmpty(), "a self-named probe is recognized");
        check(covered, "a recognized self-named product is granted by the rule's glob");
    }
}

// udev uses the first copy of a rule file in precedence order; a copy in
// /etc masks the packaged one. "Already installed" must mean the copy in
// effect, or an operator with an older rule in /etc is never offered the new one.
void testUdevRuleInEffect()
{
    QTemporaryDir root;
    const QString etc = root.filePath(QStringLiteral("etc"));
    const QString lib = root.filePath(QStringLiteral("lib"));
    QDir().mkpath(etc);
    QDir().mkpath(lib);
    const QStringList dirs{etc, lib};
    const QString name = QStringLiteral("70-aethersdr-ctr2.rules");
    const QString current = QStringLiteral("current rule\n");
    const auto put = [&name](const QString& dir, const QString& text) {
        QFile f(dir + QLatin1Char('/') + name);
        f.open(QIODevice::WriteOnly);
        f.write(text.toUtf8());
    };
    check(!AetherSDR::udevRuleInEffect(dirs, name, current), "no copy: not installed");
    put(lib, current);
    check(AetherSDR::udevRuleInEffect(dirs, name, current), "only the packaged copy: installed");
    put(etc, QStringLiteral("older rule\n"));
    check(!AetherSDR::udevRuleInEffect(dirs, name, current),
          "an older copy in /etc masks the current packaged one: not installed");
    put(etc, current);
    check(AetherSDR::udevRuleInEffect(dirs, name, current), "the current copy in /etc: installed");
    check(AetherSDR::udevRuleDirs().first() == QStringLiteral("/etc/udev/rules.d"),
          "/etc is udev's highest precedence");
    // An administrator's mask: never offered, so never written through.
    QFile::remove(etc + QLatin1Char('/') + name);
    check(QFile::link(QStringLiteral("/dev/null"), etc + QLatin1Char('/') + name)
              && AetherSDR::udevRuleInEffect(dirs, name, current),
          "a /dev/null mask in /etc counts as settled");
}

// A retry armed for one CTR2 window action runs only while that action is
// still the operator's latest: another action or Cancel drops it.
void testActionRetryTickets()
{
    AetherSDR::Ctr2ActionRetry actions;
    int runs = 0;
    const AetherSDR::Ctr2ActionRetry::Job first = [&runs](bool, QString*) { ++runs; return false; };
    actions.started(first, QStringLiteral("first done"));
    const auto ticket = actions.ticket();
    check(actions.current(ticket) && !actions.retried(), "the failed action's ticket is current");
    check(ticket.doneText == QStringLiteral("first done") && ticket.job, "and carries that action");
    actions.started([](bool, QString*) { return true; }, QStringLiteral("second done"));
    check(!actions.current(ticket), "a newer action makes the old ticket stale");

    actions.started(first, QStringLiteral("first done"));
    const auto again = actions.ticket();
    actions.invalidate();
    check(!actions.current(again), "Cancel makes it stale");

    const auto pressed = actions.mark();
    check(actions.current(pressed), "a press waiting on the access question is current");
    actions.started(first, QStringLiteral("first done"), true);
    check(!actions.current(pressed) && actions.retried(),
          "until an action starts; a retry is marked as the one retry");

    // A press waiting on a read of the device belongs to that device.
    const auto forA = actions.mark(QStringLiteral("/sim/A|AA"));
    check(actions.current(forA, QStringLiteral("/sim/A|AA")), "a press for A is current while A is selected");
    check(!actions.current(forA, QStringLiteral("/sim/B|BB")),
          "and not once another unit is selected");
}

// Firmware that names itself: a 303A:1001 product string starting "Aether" is
// shown as the model if it is a short printable name; otherwise it is unknown.
void testSelfNamedDevices()
{
    const auto named = [](const QString& product, quint16 vid = 0x303A) {
        return AetherSDR::Ctr2HidPort::DeviceInfo{QStringLiteral("p"), vid, 0x1001, QStringLiteral("AetherSDR"),
                                                  product, QStringLiteral("0123ABCD")};
    };
    check(named(QStringLiteral("AetherDial")).ctr2Model() == QStringLiteral("AetherDial"),
          "a self-named device shows its own name");
    check(named(QStringLiteral("AetherDial")).label()
              == QStringLiteral("AetherDial (303a:1001 #0123ABCD)"),
          "a self-named device's label names it once");
    check(named(QStringLiteral("AetherDial")).shortName() == QStringLiteral("AetherDial"),
          "and its short name is just the name");
    check(named(QStringLiteral("AETHER_KNOB")).ctr2Model() == QStringLiteral("AetherKnob"),
          "the fixed AetherKnob entry is not shadowed by the prefix");
    const QString longest = QStringLiteral("Aether") + QString(26, QLatin1Char('x'));
    check(named(longest).ctr2Model() == longest, "a 32-character name is accepted");
    check(named(longest + QLatin1Char('x')).ctr2Model().isEmpty(), "a 33-character name is refused");
    check(named(QStringLiteral("Aether")).ctr2Model().isEmpty(), "the prefix alone is not a name");
    check(named(QStringLiteral("AetherX")).ctr2Model() == QStringLiteral("AetherX"),
          "one character past the prefix is a name");
    check(named(QStringLiteral("Aether\tTab")).ctr2Model().isEmpty()
              && named(QStringLiteral("Aether\u00e9")).ctr2Model().isEmpty()
              && named(QStringLiteral("Aether\x7f")).ctr2Model().isEmpty(),
          "a name with control or non-ASCII characters is refused");
    check(named(QStringLiteral("Aether ~")).ctr2Model() == QStringLiteral("Aether ~"),
          "space and tilde bound the accepted range");
    check(named(QStringLiteral("aetherdial")).ctr2Model().isEmpty()
              && named(QStringLiteral("AetherDial"), 0x1234).ctr2Model().isEmpty(),
          "the prefix is case-sensitive and needs the Espressif IDs");
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

// The relay starts by itself when AetherSDR connects to a radio, retries
// until the device is plugged in, stays stopped after the operator's Stop
// until the next connection, and remembers its device.
void testAutoStart(const QString& product)
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    AetherSDR::Ctr2RelaySettings{}.save();
    const Info stock{QStringLiteral("stock"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     QStringLiteral("000000000001")};
    const Info aether{QStringLiteral("aether"), 0x303A, 0x1001, {}, product,
                      QStringLiteral("B81F3FC45888")};
    auto devices = std::make_shared<QList<Info>>();
    auto log = std::make_shared<PortLog>();
    const auto backend = [&](Ctr2ProxyModel& m) {
        m.setUsbBackend([devices] { return *devices; },
                        [log](const Info&, QString*) {
                            return static_cast<AetherSDR::Ctr2HidPort*>(new FakePort(log));
                        });
        m.setTransport(Ctr2ProxyModel::Transport::Usb);
    };
    const QHostAddress radio(QStringLiteral("192.0.2.10"));
    const int retry = Ctr2ProxyModel::kAutoStartRetryMs + 2000;
    {
        Ctr2ProxyModel model;
        backend(model);
        model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
        waitUntil([] { return false; }, 50);
        check(!model.isRunning(), "with auto-start off, connecting starts nothing");

        model.setAutoStart(true);
        waitUntil([] { return false; }, 50);
        check(!model.isRunning(), "no device: nothing starts");
        devices->append(aether);
        check(waitUntil([&] { return model.isRunning(); }, retry),
              "the relay starts once the device is plugged in");

        model.stopByOperator();
        check(!waitUntil([&] { return model.isRunning(); }, retry),
              "after the operator's Stop it stays stopped");
        model.setAetherRadio({}, 0, {}, {});
        model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
        check(waitUntil([&] { return model.isRunning(); }, 500),
              "the next connection starts it again");
        model.stop();
    }
    check(AetherSDR::Ctr2RelaySettings::load().autoStart
              && AetherSDR::Ctr2RelaySettings::load().usbSerial == aether.serial,
          "auto-start and the device are saved");
    {
        *devices = {stock, aether};
        Ctr2ProxyModel model;
        backend(model);
        check(model.usbDevicePath() == aether.path, "the saved device is selected again");
        model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
        check(waitUntil([&] { return model.isRunning(); }, 500),
              "and the relay starts on connecting");
        model.stop();
    }
    AetherSDR::Ctr2RelaySettings{}.save();
}

// Lynovation's firmware relays over USB from v1.02.00 on, so a CTR2-Max on
// it starts by itself like AetherCTR2; one that never answers the link (an
// earlier version) gets the hint, which an answer or a stop clears. Other
// stock CTR2 identities still have no USB mode.
void testStockFirmwareOverUsb()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info stock{QStringLiteral("stock"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     QStringLiteral("000000000001")};
    const Info dial{QStringLiteral("dial"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"),
                    QStringLiteral("000000000002")};
    check(stock.usbLink(), "a CTR2-Max on Lynovation's firmware relays over USB");
    check(!dial.usbLink(), "a stock M5Dial CTR2 has no USB mode");

    AetherSDR::Ctr2RelaySettings settings;
    settings.autoStart = true;
    settings.save();
    auto devices = std::make_shared<QList<Info>>(QList<Info>{stock});
    auto log = std::make_shared<PortLog>();
    QPointer<FakePort> port;
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return *devices; },
                        [log, &port](const Info&, QString*) {
                            port = new FakePort(log);
                            return static_cast<AetherSDR::Ctr2HidPort*>(port.data());
                        });
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    injectInertRadio(model);
    model.setNoAnswerHintMs(300);
    const QHostAddress radio(QStringLiteral("192.0.2.10"));
    model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
    check(waitUntil([&] { return model.isRunning(); }, 1000),
          "a CTR2-Max on Lynovation's firmware is started automatically");
    check(model.noAnswerHint().isEmpty(), "no hint while the CTR2 may still answer");
    check(waitUntil([&] { return !model.noAnswerHint().isEmpty(); }, 2000),
          "a CTR2 that never answers gets the hint");
    check(model.noAnswerHint().contains(QStringLiteral("v1.02.00"))
              && model.noAnswerHint().contains(QStringLiteral("Wi-Fi")),
          "the hint names the firmware needed and the Wi-Fi alternative");
    model.stop();
    check(model.noAnswerHint().isEmpty(), "stopping clears the hint");

    model.start();
    check(waitUntil([&] { return model.isRunning() && port; }, 1000), "started again");
    const auto ready = QByteArray::fromHex("ff00000200010000");   // READY, counter 0
    emit port->reportsReceived(ready);
    check(!waitUntil([&] { return !model.noAnswerHint().isEmpty(); }, 800),
          "a CTR2 that answered gets no hint");
    model.stop();

    model.setAetherRadio({}, 0, {}, {});
    *devices = {dial};
    check(waitUntil([&] { return model.usbDevicePath() == dial.path; },
                    Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "the M5Dial is selected");
    model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
    check(!waitUntil([&] { return model.isRunning(); }, Ctr2ProxyModel::kAutoStartRetryMs + 2000),
          "a CTR2 identity with no USB mode is never started automatically");
    AetherSDR::Ctr2RelaySettings{}.save();
}

// A relay that fails before the CTR2 ever answered (firmware with no USB
// mode, whose first write fails on Windows and macOS) is not reopened every
// kAutoStartRetryMs on the GUI thread: it waits for the device list to
// change. One that fails after an answer is retried as before.
void testNoRetryWithoutAnswer()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info stock{QStringLiteral("stock"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     QStringLiteral("000000000001")};
    AetherSDR::Ctr2RelaySettings settings;
    settings.autoStart = true;
    settings.save();
    auto devices = std::make_shared<QList<Info>>(QList<Info>{stock});
    auto log = std::make_shared<PortLog>();
    QPointer<FakePort> port;
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return *devices; },
                        [log, &port](const Info&, QString*) {
                            port = new FakePort(log);
                            return static_cast<AetherSDR::Ctr2HidPort*>(port.data());
                        });
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    injectInertRadio(model);
    const QHostAddress radio(QStringLiteral("192.0.2.10"));
    const int retry = Ctr2ProxyModel::kAutoStartRetryMs + 2000;
    model.setAetherRadio(radio, 4992, QStringLiteral("FLEX"), {});
    check(waitUntil([&] { return model.isRunning() && port; }, 1000), "auto-start opens the CTR2");
    const int opened = log->opened;
    emit port->failed(QStringLiteral("HID write failed"));
    check(!model.isRunning(), "a failed write stops the relay");
    check(model.lastError().contains(QStringLiteral("never answered"))
              && model.lastError().contains(QStringLiteral("HID write failed")),
          "the error says the CTR2 never answered, after the port's own error");
    check(!waitUntil([&] { return model.isRunning(); }, retry) && log->opened == opened,
          "a CTR2 that never answered is not reopened automatically");
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    check(waitUntil([&] { return model.isRunning() && log->opened > opened; }, retry),
          "changing the mode tries it again");
    emit port->failed(QStringLiteral("HID write failed"));
    check(!model.isRunning(), "and the second unanswered run stops too");
    const int beforeToggle = log->opened;
    model.setAutoStart(false);
    model.setAutoStart(true);
    check(waitUntil([&] { return model.isRunning() && log->opened > beforeToggle; }, 1000),
          "ticking Start automatically again tries it again");
    emit port->failed(QStringLiteral("HID write failed"));

    Info replugged = stock;
    replugged.serial = QStringLiteral("000000000002");
    *devices = {replugged};
    check(waitUntil([&] { return model.isRunning(); }, Ctr2ProxyModel::kDeviceRescanMs + retry),
          "a change in the device list tries it again");

    check(waitUntil([&] { return bool(port); }, 1000), "with a new port");
    emit port->reportsReceived(QByteArray::fromHex("ff00000200010000"));   // READY
    waitUntil([] { return false; }, 100);
    const int answeredOpens = log->opened;
    emit port->failed(QStringLiteral("HID read failed"));
    check(waitUntil([&] { return model.isRunning() && log->opened > answeredOpens; }, retry),
          "a CTR2 that had answered is retried as before");

    // The hold is a slower retry, not a stop: a fault on the way up heals.
    model.setNoAnswerRetryMs(500);
    check(waitUntil([&] { return bool(port); }, 1000), "the retry has a port");
    const int slowOpens = log->opened;
    emit port->failed(QStringLiteral("HID write failed"));
    check(!model.isRunning(), "the retry failed without an answer");
    check(waitUntil([&] { return model.isRunning() && log->opened > slowOpens; }, retry),
          "once the hold expires it is tried again");
    model.stop();
    AetherSDR::Ctr2RelaySettings{}.save();
}

// A controller that switches firmware changes its USB identity; while the
// relay is stopped the list follows without a Rescan.
void testDevicesRescanByThemselves()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info aether{QStringLiteral("aether"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                      QStringLiteral("B81F3FC45888")};
    const Info stock{QStringLiteral("stock"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"),
                     QStringLiteral("B81F3FC45888")};
    auto devices = std::make_shared<QList<Info>>(QList<Info>{stock});
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return *devices; },
                        [](const Info&, QString*) { return static_cast<AetherSDR::Ctr2HidPort*>(nullptr); });
    check(model.usbDevicePath() == stock.path, "the stock firmware's device is listed");
    *devices = {aether};
    check(waitUntil([&] { return model.usbDevicePath() == aether.path; },
                    Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "after a firmware switch the new device is found without a Rescan");

    // On Linux the switched firmware comes back at the same hidraw path.
    Info samePath = stock;
    samePath.path = aether.path;
    samePath.serial.clear();
    *devices = {samePath};
    check(waitUntil([&] {
              const auto list = model.availableUsbDevices();
              return list.size() == 1 && list[0].product == samePath.product;
          }, Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "a firmware switch at the same device path is noticed too");
}

// The periodic rescan enumerates a thread-safe source off the GUI thread:
// the event loop keeps turning during a slow scan, the result still reaches
// the list, and a scan overtaken by an explicit Rescan is dropped.
void testRescanRunsOffGuiThread()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info a{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"), {}};
    const Info b{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"), {}};
    struct Source {
        QMutex mutex;
        QList<Info> devices;
        int slowMs{0};
        int workerCalls{0};
        QThread* gui{nullptr};
    };
    auto source = std::make_shared<Source>();
    source->devices = {a};
    source->gui = QThread::currentThread();
    Ctr2ProxyModel model;
    model.setUsbBackend(
        [source] {
            int slowMs = 0;
            QList<Info> devices;
            {
                const QMutexLocker lock(&source->mutex);
                if (QThread::currentThread() != source->gui) {
                    ++source->workerCalls;
                    slowMs = source->slowMs;
                }
                devices = source->devices;
            }
            QThread::msleep(slowMs);
            return devices;
        },
        [](const Info&, QString*) { return static_cast<AetherSDR::Ctr2HidPort*>(nullptr); },
        {}, true);
    check(model.usbDevicePath() == a.path, "the first scan lists the device");

    {
        const QMutexLocker lock(&source->mutex);
        source->slowMs = 1500;
        source->devices = {b};
    }
    const auto workerCalls = [&] {
        const QMutexLocker lock(&source->mutex);
        return source->workerCalls;
    };
    check(waitUntil([&] { return workerCalls() > 0; }, Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "the periodic rescan calls the source off the GUI thread");
    // The scan now sleeps 1.5 s on its worker; the event loop must not.
    int ticks = 0;
    QTimer tick;
    tick.setInterval(20);
    QObject::connect(&tick, &QTimer::timeout, [&] { ++ticks; });
    tick.start();
    waitUntil([] { return false; }, 600);
    tick.stop();
    check(ticks >= 10, "the event loop keeps running while a slow scan is out");
    check(model.usbDevicePath() == a.path, "the list changes only when the scan completes");
    check(waitUntil([&] { return model.usbDevicePath() == b.path; }, 3000),
          "the background scan's result reaches the device list");

    // A synchronous Rescan while a background scan is out wins: the older
    // result, which still lists b, is dropped when it arrives.
    const int before = workerCalls();
    check(waitUntil([&] { return workerCalls() > before; }, Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "another background scan starts");
    {
        const QMutexLocker lock(&source->mutex);
        source->slowMs = 0;
        source->devices = {a};
    }
    model.refreshDevices();
    check(model.usbDevicePath() == a.path, "an explicit Rescan applies at once");
    waitUntil([] { return false; }, 1800);
    check(model.usbDevicePath() == a.path, "a background scan overtaken by a Rescan is dropped");
}

// Without a USB source (a build without hidapi) the periodic rescan still
// follows the network interfaces, off the GUI thread, so a listen address
// that appears after startup is offered without pressing Rescan.
void testInterfacesRescanWithoutUsbSource()
{
    struct Source {
        QMutex mutex;
        QStringList addresses{QStringLiteral("192.0.2.1")};
        int guiCalls{0};
        int workerCalls{0};
        QThread* gui{nullptr};
    };
    auto source = std::make_shared<Source>();
    source->gui = QThread::currentThread();
    Ctr2ProxyModel model;
    model.setUsbBackend({}, {});
    model.setListenAddressSource([source] {
        const QMutexLocker lock(&source->mutex);
        ++(QThread::currentThread() == source->gui ? source->guiCalls : source->workerCalls);
        return source->addresses;
    });
    check(model.availableListenAddresses() == QStringList{QStringLiteral("192.0.2.1")},
          "the injected interfaces are listed");
    {
        const QMutexLocker lock(&source->mutex);
        source->addresses.append(QStringLiteral("192.0.2.2"));
        source->guiCalls = 0;
    }
    check(waitUntil([&] { return model.availableListenAddresses().size() == 2; },
                    Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "with no USB source, a new interface is still found without a Rescan");
    const QMutexLocker lock(&source->mutex);
    check(source->workerCalls > 0 && source->guiCalls == 0,
          "and the interfaces are enumerated off the GUI thread");
}

// A scan that was out while the relay started belongs to the time before
// that session: when it lands after the relay has stopped again, it is
// dropped rather than replacing the list.
void testScanFromBeforeAStartIsDropped()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info a{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                 QStringLiteral("A00000000001")};
    const Info b{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                 QStringLiteral("B00000000002")};
    struct Source {
        QMutex mutex;
        QList<Info> devices;
        bool hold{false};
        int entered{0};
        int returned{0};
        QThread* gui{nullptr};
        QSemaphore release;
    };
    auto source = std::make_shared<Source>();
    source->devices = {a, b};
    source->gui = QThread::currentThread();
    auto log = std::make_shared<PortLog>();
    Ctr2ProxyModel model;
    model.setListenAddressSource([] { return QStringList{QStringLiteral("192.0.2.1")}; });
    model.setUsbBackend(
        [source] {
            bool hold = false;
            QList<Info> devices;
            {
                const QMutexLocker lock(&source->mutex);
                if (QThread::currentThread() != source->gui) {
                    ++source->entered;
                    hold = source->hold;
                }
                devices = source->devices;
            }
            if (hold) {
                source->release.acquire();
            }
            const QMutexLocker lock(&source->mutex);
            if (QThread::currentThread() != source->gui) {
                ++source->returned;
            }
            return devices;
        },
        [log](const Info&, QString*) {
            return static_cast<AetherSDR::Ctr2HidPort*>(new FakePort(log));
        },
        {}, true);
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992, QStringLiteral("FLEX"), {});
    model.setUsbDevicePath(a.path);
    {
        const QMutexLocker lock(&source->mutex);
        source->hold = true;
        source->devices = {b};   // what the held scan reports: a has gone
    }
    const auto counts = [&] {
        const QMutexLocker lock(&source->mutex);
        return std::pair{source->entered, source->returned};
    };
    check(waitUntil([&] { return counts().first > 0; }, Ctr2ProxyModel::kDeviceRescanMs + 2000),
          "a background scan is held");
    {
        const QMutexLocker lock(&source->mutex);
        source->devices = {a, b};
    }
    check(model.start() && model.isRunning(), "the relay starts on a while that scan is out");
    model.stop();
    source->release.release();
    check(waitUntil([&] { return counts().second > 0; }, 2000), "the held scan returns");
    waitUntil([] { return false; }, 100);
    check(model.usbDevicePath() == a.path,
          "a scan from before the relay started is dropped after it stops");
    {
        const QMutexLocker lock(&source->mutex);
        source->hold = false;
    }
}

// Selecting another CTR2 while the first one's version probe is still
// pending probes the new one once the first finishes; a model destroyed
// mid-probe closes the probe's port.
void testProbeFollowsSelection()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info a{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    const Info b{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    auto log = std::make_shared<PortLog>();
    auto probes = std::make_shared<std::vector<QPointer<ProbePort>>>();
    const auto settle = [] {
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
    };
    {
        Ctr2ProxyModel model;
        model.setTransport(Ctr2ProxyModel::Transport::Usb);
        model.setUsbBackend(
            [a, b] { return QList<Info>{a, b}; },
            [](const Info&, QString*) { return static_cast<AetherSDR::Ctr2HidPort*>(nullptr); },
            [log, probes](const Info& d, quint32, QString*) -> AetherSDR::Ctr2HidPort* {
                auto* port = new ProbePort(d.path, log);
                probes->push_back(port);
                return port;
            });
        settle();
        check(probes->size() == 1 && probes->back() && probes->back()->path() == a.path,
              "the preselected CTR2 is probed");

        model.setUsbDevicePath(b.path);
        settle();
        check(probes->size() == 1, "one probe at a time");
        (*probes)[0]->negotiate(1);
        settle();
        check(probes->size() == 2 && (*probes)[1] && (*probes)[1]->path() == b.path,
              "the newly selected CTR2 is probed once the first probe finishes");
        if (probes->size() == 2 && (*probes)[1]) {
            (*probes)[1]->negotiate(2);
            settle();
        }
        check(model.usbFirmwareVersion() == QStringLiteral("2.0.0"),
              "the selected CTR2's own version is reported");
        check(probes->size() == 2, "and nothing is probed again");

        // Back to the first: probed again, and the model goes before it answers.
        model.setUsbDevicePath(a.path);
        settle();
        check(probes->size() == 3, "selecting another CTR2 probes it");
    }
    settle();
    check(log->shutdowns == log->opened, "every probe port is closed, including one mid-probe");
}

// Whether the selected CTR2 takes the link's FIRMWARE commands (restart
// into its other firmware or download mode): read with its version while
// idle, and from the relay's own port while the relay holds it, as the idle
// probe never opens it then.
void testFirmwareCommandsOffered()
{
    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const quint32 firmware =
        AetherSDR::ctr2hid::capabilityBit(AetherSDR::ctr2hid::MessageType::Firmware);
    const Info stock{QStringLiteral("stock"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    auto log = std::make_shared<PortLog>();
    auto probes = std::make_shared<std::vector<QPointer<ProbePort>>>();
    QPointer<OfferPort> relayPort;
    const auto settle = [] {
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
    };
    Ctr2ProxyModel model;
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setUsbBackend(
        [stock] { return QList<Info>{stock}; },
        [log, &relayPort](const Info&, QString*) -> AetherSDR::Ctr2HidPort* {
            auto* port = new OfferPort(log);
            relayPort = port;
            return port;
        },
        [log, probes](const Info& d, quint32, QString*) -> AetherSDR::Ctr2HidPort* {
            auto* port = new ProbePort(d.path, log);
            probes->push_back(port);
            return port;
        });
    settle();
    check(!probes->empty() && probes->back(), "the stock unit is probed");
    if (!probes->empty() && probes->back())
        probes->back()->negotiate(1, firmware);
    settle();
    check(model.usbFirmwareCommands(), "an offer of FIRMWARE is read with the version");

    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600  192.0.2.10"), {});
    check(model.start() && relayPort, "the relay starts on the unit");
    check(!model.usbFirmwareCommands(), "while it runs, its own port is asked, not the probe");
    if (relayPort)
        relayPort->offer(firmware);
    check(model.usbFirmwareCommands(), "and its port's offer counts");
    model.stop();
    settle();
}

// USB is the default mode when the build can open a CTR2, and the first
// recognized CTR2 is preselected; an explicit choice survives a rescan.
void testUsbDefaults()
{
    Ctr2ProxyModel fresh;
    check((fresh.transport() == Ctr2ProxyModel::Transport::Usb) == fresh.usbAvailable(),
          "USB is the default mode exactly when USB HID is available");

    using Info = AetherSDR::Ctr2HidPort::DeviceInfo;
    const Info other{QStringLiteral("other"), 0x1234, 0x5678, {}, QStringLiteral("Keyboard"), {}};
    const Info first{QStringLiteral("ctr2-a"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}};
    const Info second{QStringLiteral("ctr2-b"), 0x303A, 0x1001, {}, QStringLiteral("M5STACK_DIAL"), {}};
    auto devices = std::make_shared<QList<Info>>(QList<Info>{other, first, second});
    Ctr2ProxyModel model;
    model.setUsbBackend([devices] { return *devices; },
                        [](const Info&, QString*) { return static_cast<AetherSDR::Ctr2HidPort*>(nullptr); });
    check(model.usbDevicePath() == first.path, "the first recognized CTR2 is preselected");

    model.setUsbDevicePath(second.path);
    devices->append(Info{QStringLiteral("ctr2-c"), 0x303A, 0x1001, {}, QStringLiteral("ESP32S3_DEV"), {}});
    model.refreshDevices();
    check(model.usbDevicePath() == second.path, "a rescan keeps the operator's choice");

    devices->removeIf([&](const Info& d) { return d.path == second.path; });
    model.refreshDevices();
    check(model.usbDevicePath() == first.path, "a vanished choice falls back to the first CTR2");

    *devices = {other};
    model.refreshDevices();
    check(model.usbDevicePath().isEmpty(), "an unrecognized device is never preselected");
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
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setUsbDevicePath(QString());
    check(!model.usbDeviceNeedsAccessRule(), "no device selected needs no rule");
    model.setUsbDevicePath(locked);
    check(model.usbDeviceNeedsAccessRule(), "a node the user cannot open needs the rule");
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);
    check(!model.usbDeviceNeedsAccessRule(), "Wi-Fi mode never asks for the USB rule");
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setUsbDevicePath(open);
    check(!model.usbDeviceNeedsAccessRule(), "an openable node needs no rule");
    model.setUsbDevicePath(missing);
    check(!model.usbDeviceNeedsAccessRule(), "a vanished node is not a permissions problem");
#endif
}

} // namespace

int main(int argc, char** argv)
{
    // Before the app: starting the relay saves its setup.
    TestSettingsProfile profile(QStringLiteral("ctr2_proxy_model_test"));
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    Ctr2ProxyModel model;

    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "with no radio pushed, Start explains that AetherSDR must connect first");
    check(!model.start() && !model.isRunning(), "Start refuses without a radio");

    const QString smartLink = QStringLiteral("AetherSDR is connected through SmartLink");
    model.setAetherRadio({}, 0, {}, smartLink);
    check(model.configurationProblem() == smartLink, "the pushed reason is shown as-is");
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    check(model.configurationProblem() == smartLink || !model.usbAvailable(),
          "USB mode also needs the radio");
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);

    model.setAetherRadio(QHostAddress(QStringLiteral("0.0.0.0")), 4992, QStringLiteral("bogus"), {});
    check(!model.configurationProblem().isEmpty() && !model.start(),
          "an unspecified address is never a destination");
    model.setAetherRadio(QHostAddress(QStringLiteral("ff02::1")), 4992, QStringLiteral("v6"), {});
    check(model.configurationProblem().contains(QStringLiteral("radio"), Qt::CaseInsensitive),
          "an IPv6 multicast address is never a destination");
    model.setAetherRadio(QHostAddress(QStringLiteral("fd58:eee:46e8::10")), 4992,
                         QStringLiteral("FLEX-8600  fd58:eee:46e8::10"), {});
    check(!model.configurationProblem().contains(QStringLiteral("radio"), Qt::CaseInsensitive),
          "a radio AetherSDR reached over IPv6 is a usable destination");
    model.setAetherRadio({}, 0, {}, {});

    int changes = 0;
    QObject::connect(&model, &Ctr2ProxyModel::configurationChanged, &model, [&] { ++changes; });
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "a new radio is announced once");
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "re-pushing the same radio is not a change");
    model.setAetherRadio(QHostAddress(QStringLiteral("::ffff:192.0.2.10")), 4992,
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "the IPv4-mapped form of the same radio is not a change");
    check(model.aetherRadioLabel() == QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"),
          "the radio label is exposed for display");
    const QString wifi = model.configurationProblem();
    check(!wifi.contains(QStringLiteral("radio"), Qt::CaseInsensitive),
          "with a radio pushed, Wi-Fi mode only asks for its own settings");
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    const QString usb = model.configurationProblem();
    check(!usb.contains(QStringLiteral("Connect AetherSDR")), "USB mode accepts the pushed radio");

    model.setAetherRadio({}, 0, {}, {});
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);
    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "losing the radio falls back to the default reason");

    AetherSDR::Ctr2RelaySettings{}.save();
    testRelayFollowsAetherSdrsRadio();
    AetherSDR::Ctr2RelaySettings{}.save();
    testUsbAccessRuleDetection();
    testUsbDefaults();
    testProbeFollowsSelection();
    testUdevRuleMatchesKnownCtr2s();
    testUdevRuleInEffect();
    testActionRetryTickets();
    testSelfNamedDevices();
    testAutoStart(QStringLiteral("AetherCTR2"));   // names itself
    testAutoStart(QStringLiteral("AETHER_KNOB"));  // the fixed AetherKnob identity
    testStockFirmwareOverUsb();
    testNoRetryWithoutAnswer();
    testDevicesRescanByThemselves();
    testRescanRunsOffGuiThread();
    testInterfacesRescanWithoutUsbSource();
    testScanFromBeforeAStartIsDropped();
    testFirmwareCommandsOffered();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_proxy_model_test: all checks passed\n");
    return 0;
}
