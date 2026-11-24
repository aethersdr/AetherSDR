// Socket-free: the VPN source-path picker keeps the address the operator chose
// while the radio address is edited through other families. Interface snapshots
// make the same selection rules run on hosts with and without IPv6 adapters.
#include "TestSettingsProfile.h"
#include "core/NetworkPathResolver.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QMetaObject>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
struct ConnectionPanelSmartLinkTestAccess {
    static void setSourcePaths(ConnectionPanel& panel, const QList<NetworkPathCandidate>& paths)
    {
        panel.m_manualSourceCandidates = [paths](QAbstractSocket::NetworkLayerProtocol family) {
            QList<NetworkPathCandidate> matching;
            for (const auto& path : paths) {
                if (path.address.protocol() == family) { matching.append(path); }
            }
            return matching;
        };
        panel.refreshManualSourceOptions();
    }
};
}

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
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
    const NetworkPathCandidate gua{"test-vpn", "VPN", QHostAddress("2001:db8:1::20")};
    const NetworkPathCandidate ula{"test-vpn", "VPN", QHostAddress("fd58:eee:46e8::20")};

    ConnectionPanel panel;
    ConnectionPanelSmartLinkTestAccess::setSourcePaths(panel, {gua, ula});
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
