#include "TestSettingsProfile.h"
#include "gui/ClientChainApplet.h"
#include "gui/ClientChainWidget.h"
#include "gui/ClientRxChainWidget.h"

#include <QApplication>
#include <QLabel>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("client-chain-audio-path-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    ClientChainApplet chain;
    chain.show();
    app.processEvents();
    ClientChainWidget* tx = chain.findChild<ClientChainWidget*>();
    ClientRxChainWidget* rx = chain.findChild<ClientRxChainWidget*>();
    QLabel* notice = chain.findChild<QLabel*>(QStringLiteral("chainTxPcAudioNotice"));
    if (!tx || !rx || !notice) {
        std::fprintf(stderr, "Missing production chain widgets\n");
        return 1;
    }
    int failures = 0;
    const auto check = [&failures](const char* name, bool passed) {
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) {
            ++failures;
        }
    };
    check("initial TX visible, RX hidden", tx->isVisible() && !rx->isVisible());
    chain.setTxAudioPathNotice(QStringLiteral("Use PC mic"), true);
    app.processEvents();
    check("warning hides TX, exposes guidance", !tx->isVisible() && notice->isVisible());
    chain.setActiveTab(ClientChainApplet::ChainMode::Rx);
    app.processEvents();
    check("RX remains visible, guidance hidden", rx->isVisible() && !notice->isVisible());
    chain.setTxAudioPathNotice(QStringLiteral("Use PC mic"), true);
    app.processEvents();
    check("repeated blocked update preserves RX", rx->isVisible() && !notice->isVisible());
    chain.setActiveTab(ClientChainApplet::ChainMode::Tx);
    app.processEvents();
    check("TX stays blocked after tab roundtrip", !tx->isVisible() && notice->isVisible());
    chain.setTxAudioPathNotice({}, false);
    app.processEvents();
    check("route recovery restores TX", tx->isVisible() && !notice->isVisible());
    chain.setTxAudioPathNotice(QStringLiteral("Offline guidance"), false);
    app.processEvents();
    check("informational notice does not block TX", tx->isVisible() && notice->isVisible());
    return failures == 0 ? 0 : 1;
}
