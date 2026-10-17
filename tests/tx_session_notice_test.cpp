// The notice a refused key shows while transmit admission is closed.
//
// Admission closes in resetTxOperations() on every teardown and reopens only in
// onConnected(). An auto-reconnect after a dropped link therefore runs with it
// closed while the TX slice survives, so a key reaches that refusal. The notice
// must name the phase the operator is in, not "disconnects" during a reconnect.
//
// No socket is bound or listened on. connectToRadio() reaches RadioConnection,
// so the address is TEST-NET-1 (RFC 5737), as in
// radio_capacity_declaration_test.cpp; the session is then a socket-free backend.

#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QStringList>

#include <cstdio>
#include <memory>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

class Backend : public IRadioBackend {
public:
    bool connected{true};
    RadioCapabilities caps;
    bool handled{false};
    ReceiveDispatch result{ReceiveDispatch::Unsupported};
    std::vector<TransmitControlRequest> requests;
    std::vector<TunePowerContext> contexts;
    QList<QPair<bool, int>> tunes;
    QList<int> cwPitches;
    QList<int> lineoutGains;
    QList<bool> lineoutMutes;
    int keyCalls{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { ++keyCalls; }
    void setTune(bool on, int power, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { tunes.append({on, power}); }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setCwPitch(int hz) override { cwPitches.append(hz); }
    void setLineoutGain(int percent) override { lineoutGains.append(percent); }
    void setLineoutMute(bool mute) override { lineoutMutes.append(mute); }
    ReceiveDispatch requestTransmitControl(const TransmitControlRequest& request, TunePowerContext context) override
    {
        requests.push_back(request);
        contexts.push_back(context);
        return handled ? result : IRadioBackend::requestTransmitControl(request, context);
    }
};

struct Session {
    RadioModel radio;
    Backend* backend{nullptr};
    QStringList keys;
    QStringList messages;
    Session()
    {
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        radio.connectToRadio(info);   // records the radio the timer reconnects to
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        backend->caps.canTransmit = true;
        radio.setBackendForTest(std::move(owned), QStringLiteral("tx-session-notice-test"));
        SliceDelta delta;
        delta.inUse = true; delta.frequency = 14.2; delta.mode = QStringLiteral("USB");
        delta.filterLow = 100; delta.filterHigh = 2800;
        delta.panId = QStringLiteral("receiver"); delta.txSlice = true;
        emit backend->sliceChanged(0, delta);
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
                         [this](const QString& message, const QString& key, const QString&) {
                             keys.append(key);
                             messages.append(message);
                         });
    }
    void key()
    {
        keys.clear();
        messages.clear();
        radio.setTransmit(true);
        std::printf("notices: %s\n", qPrintable(keys.join(QStringLiteral(", "))));
    }
};

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tx-session-notice"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    {
        Session s;
        QMetaObject::invokeMethod(&s.radio, "onDisconnected", Qt::DirectConnection);
        s.radio.triggerAutoReconnectForTest();
        check(s.radio.txSlice() != nullptr, "the TX slice survives into the reconnect");
        s.key();
        check(s.backend->keyCalls == 0, "a key during the reconnect does not reach the radio");
        check(s.keys.contains(QStringLiteral("tx-session-connecting"))
                  && s.messages.contains(QStringLiteral(
                      "Transmit is unavailable until the radio has connected.")),
              "it is refused with the connecting notice");
        check(!s.keys.contains(QStringLiteral("tx-session-closing")),
              "not with the disconnecting notice");
    }
    {
        // The operator disconnects: no attempt is in flight, admission is closed.
        Session s;
        s.radio.disconnectFromRadio();
        s.key();
        check(s.keys.contains(QStringLiteral("tx-session-closing"))
                  && !s.keys.contains(QStringLiteral("tx-session-connecting")),
              "with no attempt in flight the disconnecting notice stands");
    }

    if (g_failures == 0)
        std::printf("tx_session_notice_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
