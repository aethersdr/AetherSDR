// Socket-free: the VPN source-path picker keeps the address the operator chose
// while the radio address is edited through other families. Reads this host's
// interface list; skips (77) without an adapter carrying two IPv6 addresses.
#include "TestSettingsProfile.h"
#include "core/NetworkPathResolver.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QMetaObject>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}
bool isUla(const QHostAddress& address)
{
    return address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7);
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("connection-panel-source-pick"));
    if (!profile.isValid())
        return 1;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // A GUA and a ULA on one adapter, as an IPv6 LAN gives.
    const auto v6 = NetworkPathResolver::enumerateCandidates(QAbstractSocket::IPv6Protocol);
    NetworkPathCandidate gua, ula;
    for (const auto& c : v6) {
        for (const auto& other : v6) {
            if (c.interfaceId == other.interfaceId && !isUla(c.address) && isUla(other.address)) {
                gua = c;
                ula = other;
            }
        }
    }
    if (!gua.isValid()) {
        std::fprintf(stderr, "SKIP: no adapter with both an IPv6 GUA and a ULA\n");
        return 77;
    }

    ConnectionPanel panel;
    auto* ip = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIp"));
    auto* source = panel.findChild<QComboBox*>(QStringLiteral("connectionManualSourcePath"));
    if (!ip || !source) {
        std::fprintf(stderr, "FAIL: connection panel controls not found\n");
        return 1;
    }

    // A ULA radio; the operator deliberately picks the adapter's GUA.
    const QString ulaRadio = QStringLiteral("fd58:eee:46e8::10");
    ip->setText(ulaRadio);
    const int guaRow = source->findText(gua.label());
    check(guaRow > 0, "the adapter's GUA is offered for an IPv6 radio");
    source->setCurrentIndex(guaRow);
    QMetaObject::invokeMethod(source, "activated", Qt::DirectConnection, Q_ARG(int, guaRow));

    // Retype the address: empty and partial text read as IPv4 on the way.
    ip->setText(QString());
    ip->setText(QStringLiteral("fd58"));
    check(source->currentText() != gua.label(), "an IPv4 list is shown while the text is not IPv6");
    ip->setText(ulaRadio);
    check(source->currentText() == gua.label(),
          "the picked GUA comes back, not the ULA the prefix rule alone would choose");

    // Choosing Auto is a pick too and must not be undone by later typing.
    source->setCurrentIndex(0);
    QMetaObject::invokeMethod(source, "activated", Qt::DirectConnection, Q_ARG(int, 0));
    ip->setText(QString());
    ip->setText(ulaRadio);
    check(source->currentIndex() == 0, "a picked Auto stays Auto after retyping");

    std::printf("%s connection_panel_source_pick_test\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
