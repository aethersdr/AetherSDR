// TCI rx_volume in the spec form (TCI v2.0): `rx_volume:trx,channel,dB;` with
// dB -60..0, and `rx_volume:trx,channel;` as the read. The slice's audio gain
// used to be set from the CHANNEL argument, so every spec SET muted the slice
// and every read answered in percent. The older `rx_volume:trx,N;` percent
// form is still taken where N cannot be a channel.
//
// Socket-free: an injected stub backend and TciProtocol driven by command
// text. Nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>
#include <utility>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failed;
    }
}

class StubBackend final : public IRadioBackend
{
public:
    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};
    SliceModel* slice{nullptr};

    Fixture()
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("tci-rx-volume-test"));
        const QString pan = QStringLiteral("tci-rx-volume/pan0");
        emit backend->panCenterBandwidthChanged(pan, 14.2, 0.1);
        SliceDelta delta;
        delta.panId = pan;
        delta.frequency = 14.2;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 100;
        delta.filterHigh = 2900;
        delta.inUse = true;
        emit backend->sliceChanged(0, delta);
        slice = radio.slice(0);
        if (!slice) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
    }

    // SETs reach the slice through a queued call.
    QString send(TciProtocol& tci, const QString& cmd)
    {
        const QString reply = tci.handleCommand(cmd);
        QCoreApplication::processEvents();
        return reply;
    }
};

void testSpecSetIsDecibels()
{
    Fixture f;
    TciProtocol tci(&f.radio);

    f.send(tci, QStringLiteral("rx_volume:0,0,-12"));
    check(qRound(f.slice->audioGain()) == 25,
          "rx_volume:0,0,-12 sets the slice to 25 % (-12 dB), not to the channel");
    check(tci.pendingNotification() == QStringLiteral("rx_volume:0,0,-12;"),
          "the SET is echoed in the spec form, in dB");

    f.send(tci, QStringLiteral("rx_volume:0,0,0"));
    check(qRound(f.slice->audioGain()) == 100, "rx_volume:0,0,0 is full volume");

    f.send(tci, QStringLiteral("rx_volume:0,1,-60"));
    check(qRound(f.slice->audioGain()) == 0, "rx_volume:0,1,-60 is silence, on channel 1 too");
    check(tci.pendingNotification() == QStringLiteral("rx_volume:0,1,-60;"),
          "a channel 1 SET is echoed on channel 1");
}

void testSpecGetAnswersDecibels()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.send(tci, QStringLiteral("rx_volume:0,0,-6"));
    const float before = f.slice->audioGain();

    check(f.send(tci, QStringLiteral("rx_volume:0,0")) == QStringLiteral("rx_volume:0,0,-6;"),
          "rx_volume:0,0 reads channel 0 in dB");
    check(f.send(tci, QStringLiteral("rx_volume:0,1")) == QStringLiteral("rx_volume:0,1,-6;"),
          "rx_volume:0,1 reads channel 1, the same slice gain");
    check(f.send(tci, QStringLiteral("rx_volume:0")) == QStringLiteral("rx_volume:0,0,-6;"),
          "rx_volume:0 reads channel 0");
    check(f.slice->audioGain() == before, "a read leaves the gain alone");
}

void testLegacyPercentStillSets()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.send(tci, QStringLiteral("rx_volume:0,50"));
    check(qRound(f.slice->audioGain()) == 50,
          "legacy rx_volume:0,50 still sets 50 %");
    check(tci.pendingNotification() == QStringLiteral("rx_volume:0,0,-6;"),
          "the legacy SET is echoed in the spec form");
}

void testMalformedIsDropped()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.send(tci, QStringLiteral("rx_volume:0,0,-20"));
    const float before = f.slice->audioGain();

    check(f.send(tci, QStringLiteral("rx_volume:0,2,-6")).isEmpty()
              && f.slice->audioGain() == before,
          "channel 2 is refused");
    check(f.send(tci, QStringLiteral("rx_volume:0,0,")).isEmpty()
              && f.slice->audioGain() == before,
          "an empty dB value is refused, not read as 0 dB");
    check(f.send(tci, QStringLiteral("rx_volume:0,-6")).isEmpty()
              && f.slice->audioGain() == before,
          "a negative second argument is neither a channel nor a percent");
    check(f.send(tci, QStringLiteral("rx_volume:7,0,-6")).isEmpty()
              && f.slice->audioGain() == before,
          "an unknown receiver is refused");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tci-rx-volume-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    AppSettings::instance().load();

    testSpecSetIsDecibels();
    testSpecGetAnswersDecibels();
    testLegacyPercentStillSets();
    testMalformedIsDropped();

    std::printf("%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
