// TCI agc_gain in the spec form (TCI 2.1 §AGC_GAIN): `agc_gain:trx,dB;` with
// dB -20..120, while the slice stores the AGC-T knob as 0..100. One linear map
// converts for every backend; a KiwiSDR receive replacement, whose value is
// already its AGC threshold in dB, passes through unchanged.
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
        radio.setBackendForTest(std::move(owned), QStringLiteral("tci-agc-gain-test"));
        const QString pan = QStringLiteral("tci-agc-gain/pan0");
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

    void setKnob(int level)
    {
        SliceDelta delta;
        delta.agcThreshold = level;
        emit backend->sliceChanged(0, delta);
    }

    // SETs reach the slice through a queued call.
    QString send(TciProtocol& tci, const QString& cmd)
    {
        const QString reply = tci.handleCommand(cmd);
        QCoreApplication::processEvents();
        return reply;
    }
};

void testMapEnds()
{
    check(TciProtocol::agcGainDbForLevel(0) == -20, "knob 0 is -20 dB, the bottom of the spec range");
    check(TciProtocol::agcGainDbForLevel(100) == 120, "knob 100 is 120 dB, the top of the spec range");
    check(TciProtocol::agcGainDbForLevel(65) == 71, "the default knob 65 is 71 dB");
    check(TciProtocol::agcLevelForGainDb(-20) == 0, "-20 dB is knob 0");
    check(TciProtocol::agcLevelForGainDb(120) == 100, "120 dB is knob 100");
    check(TciProtocol::agcLevelForGainDb(-60) == 0, "below -20 dB is held at knob 0");
    check(TciProtocol::agcLevelForGainDb(200) == 100, "above 120 dB is held at knob 100");
    bool roundTrips = true;
    for (int level = 0; level <= 100; ++level) {
        if (TciProtocol::agcLevelForGainDb(TciProtocol::agcGainDbForLevel(level)) != level) {
            roundTrips = false;
        }
    }
    check(roundTrips, "every knob step's reported dB, sent back, sets the same step");
}

void testGetAnswersDecibels()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.setKnob(72);
    check(f.send(tci, QStringLiteral("agc_gain:0")) == QStringLiteral("agc_gain:0,81;"),
          "agc_gain:0 reads knob 72 as 81 dB");
    check(tci.generateInitBurst().contains(QStringLiteral("agc_gain:0,81;")),
          "the init burst carries the same dB");
}

void testSetIsDecibels()
{
    Fixture f;
    TciProtocol tci(&f.radio);

    f.send(tci, QStringLiteral("agc_gain:0,87"));
    check(f.slice->receiveAgcThreshold() == 76, "agc_gain:0,87 sets knob 76");
    check(tci.pendingNotification() == QStringLiteral("agc_gain:0,86;"),
          "the echo states the gain knob 76 really gives");

    f.send(tci, QStringLiteral("agc_gain:0,-20"));
    check(f.slice->receiveAgcThreshold() == 0, "-20 dB sets knob 0");

    f.send(tci, QStringLiteral("agc_gain:0,500"));
    check(f.slice->receiveAgcThreshold() == 100, "a value above 120 dB is held at 120 dB");
    check(tci.pendingNotification() == QStringLiteral("agc_gain:0,120;"),
          "the held value is what is echoed");

    f.send(tci, QStringLiteral("agc_gain:0,loud"));
    check(f.slice->receiveAgcThreshold() == 100, "a non-number is ignored");
}

void testKiwiReplacementPassesThrough()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    if (!f.slice->externalReceiveReplacementActive()) {
        check(false, "the KiwiSDR receive replacement could not be enabled");
        return;
    }
    f.send(tci, QStringLiteral("agc_gain:0,-90"));
    check(f.slice->receiveAgcThreshold() == -90,
          "a KiwiSDR replacement takes the client's dB as its threshold");
    check(f.send(tci, QStringLiteral("agc_gain:0")) == QStringLiteral("agc_gain:0,-90;"),
          "and reads it back unchanged");
}

void testHugeAndNonFiniteValuesStayBounded()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.setKnob(72);
    f.send(tci, QStringLiteral("agc_gain:0,inf"));
    f.send(tci, QStringLiteral("agc_gain:0,nan"));
    check(f.slice->agcThreshold() == 72, "inf and nan are ignored");
    f.send(tci, QStringLiteral("agc_gain:0,1e300"));
    check(f.slice->agcThreshold() == 100, "1e300 dB is held at 120 dB, knob 100");

    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    f.send(tci, QStringLiteral("agc_gain:0,-90"));
    f.send(tci, QStringLiteral("agc_gain:0,2147483648"));
    check(f.slice->receiveAgcThreshold() == 0,
          "a KiwiSDR value past int range is held at the top, 0 dB, not wrapped");
    check(tci.pendingNotification() == QStringLiteral("agc_gain:0,0;"),
          "and the echo states the bounded value");
    f.send(tci, QStringLiteral("agc_gain:0,-4294967296"));
    check(f.slice->receiveAgcThreshold() == -160,
          "a KiwiSDR value below int range is held at the bottom, -160 dB");
    check(tci.pendingNotification() == QStringLiteral("agc_gain:0,-160;"),
          "and the echo states the bounded value");
}

void testPathChangeBeforeDeliveryDropsTheSet()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    f.slice->setAgcThreshold(-90);
    f.slice->setExternalReceiveAudioReplacementMute(false, false);
    f.setKnob(72);

    // Queued for the knob, delivered after a KiwiSDR replacement starts.
    tci.handleCommand(QStringLiteral("agc_gain:0,-10"));
    f.slice->setExternalReceiveAudioReplacementMute(true, false);
    QCoreApplication::processEvents();
    check(f.slice->receiveAgcThreshold() == -90,
          "a knob SET is not delivered to the KiwiSDR threshold as dB");
    check(f.slice->agcThreshold() == 72, "nor applied to the knob it was meant for");

    // Queued for the KiwiSDR, delivered after the replacement stops.
    tci.handleCommand(QStringLiteral("agc_gain:0,-10"));
    f.slice->setExternalReceiveAudioReplacementMute(false, false);
    QCoreApplication::processEvents();
    check(f.slice->agcThreshold() == 72, "a KiwiSDR SET is not delivered to the knob");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tci-agc-gain-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    AppSettings::instance().load();

    testMapEnds();
    testGetAnswersDecibels();
    testSetIsDecibels();
    testKiwiReplacementPassesThrough();
    testHugeAndNonFiniteValuesStayBounded();
    testPathChangeBeforeDeliveryDropsTheSet();

    std::printf("%s: %d failure(s)\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
