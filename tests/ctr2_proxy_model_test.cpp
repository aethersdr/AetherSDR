// Ctr2ProxyModel destination handling: the relay targets only the radio its
// owner pushes in, and every unusable case keeps Start disabled with the
// pushed reason. Socket-free: nothing is started or bound.

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2HidPort.h"
#include "models/Ctr2ProxyModel.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>
#include <memory>
#include <vector>

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
    bool closedWithClosed{false};
};

// Injected HID port: records whether the relay told the CTR2 CLOSED on close.
class FakePort : public AetherSDR::Ctr2HidPort {
public:
    explicit FakePort(std::shared_ptr<PortLog> log) : m_log(std::move(log)) { ++m_log->opened; }
    bool isOpen() const override { return m_open; }
    void send(const std::vector<AetherSDR::ctr2hid::Report>&) override {}
    void discardQueued() override {}
    void shutdown(const std::vector<AetherSDR::ctr2hid::Report>& final) override
    {
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
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    model.setUsbDevicePath(ctr2.path);
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
    QCoreApplication::processEvents();
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
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    check(model.configurationProblem() == smartLink || !model.usbAvailable(),
          "USB mode also needs the radio");
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);

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
          "with a radio pushed, Wi-Fi mode only asks for its own settings");
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    const QString usb = model.configurationProblem();
    check(!usb.contains(QStringLiteral("Connect AetherSDR")), "USB mode accepts the pushed radio");

    model.setAetherRadio({}, 0, {}, {});
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);
    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "losing the radio falls back to the default reason");

    testRelayFollowsAetherSdrsRadio();

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_proxy_model_test: all checks passed\n");
    return 0;
}
