// Ctr2ProxyModel destination handling: the relay targets only the radio its
// owner pushes in, and every unusable case keeps Start disabled with the
// pushed reason. Socket-free: nothing is started or bound.

#include "models/Ctr2ProxyModel.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Ctr2ProxyModel model;

    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "with no radio pushed, Start explains that AetherSDR must connect first");
    check(!model.start() && !model.isRunning(), "Start refuses without a radio");

    const QString smartLink = QStringLiteral("AetherSDR is connected through SmartLink");
    model.setAetherRadio({}, {}, smartLink);
    check(model.configurationProblem() == smartLink, "the pushed reason is shown as-is");
    model.setTransport(Ctr2ProxyModel::Transport::Usb);
    check(model.configurationProblem() == smartLink || !Ctr2ProxyModel::usbSupported(),
          "USB mode also needs the radio");
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);

    model.setAetherRadio(QHostAddress(QStringLiteral("0.0.0.0")), QStringLiteral("bogus"), {});
    check(!model.configurationProblem().isEmpty() && !model.start(),
          "an unspecified address is never a destination");
    model.setAetherRadio(QHostAddress(QStringLiteral("fe80::1")), QStringLiteral("v6"), {});
    check(!model.configurationProblem().isEmpty(), "IPv6 is out of scope and stays unavailable");

    int changes = 0;
    QObject::connect(&model, &Ctr2ProxyModel::configurationChanged, &model, [&] { ++changes; });
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")),
                         QStringLiteral("FLEX-8600 \"Shack\"  192.0.2.10"), {});
    check(changes == 1, "a new radio is announced once");
    model.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")),
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

    model.setAetherRadio({}, {}, {});
    model.setTransport(Ctr2ProxyModel::Transport::Wifi);
    check(model.configurationProblem() == QStringLiteral("Connect AetherSDR to a radio first"),
          "losing the radio falls back to the default reason");

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2_proxy_model_test: all checks passed\n");
    return 0;
}
