// TCI sql_level in the spec form (TCI v2.0): `sql_level:trx,dB;` with dB
// -140..0. The value used to be taken as the slice's 0..100 level, so a spec
// SET of -83 failed the level clamp and every read answered a level a client
// then drew as dB. The backend's published squelch scale converts; without one
// Flex's -160 + level map applies. A positive value is still the older level.
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
#include <optional>
#include <utility>
#include <vector>

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
    explicit StubBackend(std::optional<SquelchLevelScale> scale) : m_scale(std::move(scale)) {}

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities c;
        c.squelchLevelScale = m_scale;
        return c;
    }
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

    std::vector<int> levels;
    ReceiveDispatch requestSliceSquelch(int, const SliceSquelchRequest& r) override
    {
        levels.push_back(r.level);
        return ReceiveDispatch::Dispatched;
    }

private:
    std::optional<SquelchLevelScale> m_scale;
};

struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};
    SliceModel* slice{nullptr};

    explicit Fixture(std::optional<SquelchLevelScale> scale = std::nullopt)
    {
        auto owned = std::make_unique<StubBackend>(std::move(scale));
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("tci-sql-level-test"));
        const QString pan = QStringLiteral("tci-sql-level/pan0");
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

    void setSliceLevel(int level)
    {
        SliceDelta delta;
        delta.squelchLevel = level;
        emit backend->sliceChanged(0, delta);
    }

    int lastLevel() const { return backend->levels.empty() ? -1 : backend->levels.back(); }

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

    f.send(tci, QStringLiteral("sql_level:0,-83"));
    check(f.lastLevel() == 77, "sql_level:0,-83 requests level 77 on the -160 + level map");
    check(tci.pendingNotification() == QStringLiteral("sql_level:0,-83;"),
          "the SET is echoed in dB");

    f.send(tci, QStringLiteral("sql_level:0,-200"));
    check(f.lastLevel() == 20, "a value below -140 dB is held at -140 dB");
    check(tci.pendingNotification() == QStringLiteral("sql_level:0,-140;"),
          "the held value is what is echoed");

    f.send(tci, QStringLiteral("sql_level:0,0"));
    check(f.lastLevel() == 100, "0 dB is the top of the map, not level 0");
    check(tci.pendingNotification() == QStringLiteral("sql_level:0,-60;"),
          "the echo states the threshold the level really gives");
}

void testSpecGetAnswersDecibels()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.setSliceLevel(40);
    check(f.send(tci, QStringLiteral("sql_level:0")) == QStringLiteral("sql_level:0,-120;"),
          "sql_level:0 reads level 40 as -120 dB");
    check(f.backend->levels.empty(), "a read requests nothing");
}

void testPublishedScaleConverts()
{
    SquelchLevelScale scale;
    scale.offsetDb = -130.0;
    scale.dbPerStep = 0.5;
    scale.modes = {QStringLiteral("USB")};
    Fixture f(scale);
    TciProtocol tci(&f.radio);

    f.send(tci, QStringLiteral("sql_level:0,-100"));
    check(f.lastLevel() == 60, "the backend's own scale converts dB to its level");
    check(tci.pendingNotification() == QStringLiteral("sql_level:0,-100;"),
          "and back again for the echo");
}

void testLegacyLevelStillSets()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.send(tci, QStringLiteral("sql_level:0,35"));
    check(f.lastLevel() == 35, "legacy sql_level:0,35 still sets level 35");
    check(tci.pendingNotification() == QStringLiteral("sql_level:0,-125;"),
          "the legacy SET is echoed in dB");
}

void testMalformedIsDropped()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    check(f.send(tci, QStringLiteral("sql_level:0,")).isEmpty() && f.backend->levels.empty(),
          "an empty value is refused, not read as 0 dB");
    check(f.send(tci, QStringLiteral("sql_level:0,loud")).isEmpty() && f.backend->levels.empty(),
          "a value that is not a number is refused");
    check(f.send(tci, QStringLiteral("sql_level:x,-80")).isEmpty() && f.backend->levels.empty(),
          "a receiver that is not a number is refused");
}

// TciServer relays a GUI squelch change as sql_level through the same
// conversion the GET uses, so both name one dB value for one level.
void testRelayConversionMatchesGet()
{
    Fixture f;
    TciProtocol tci(&f.radio);
    f.setSliceLevel(77);
    check(TciProtocol::squelchLevelDb(&f.radio, f.slice, 77) == -83,
          "the relay conversion maps level 77 to -83 dB");
    check(f.send(tci, QStringLiteral("sql_level:0")) == QStringLiteral("sql_level:0,-83;"),
          "and the GET answers the same dB");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tci-sql-level-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    AppSettings::instance().load();

    testSpecSetIsDecibels();
    testSpecGetAnswersDecibels();
    testPublishedScaleConverts();
    testLegacyLevelStillSets();
    testMalformedIsDropped();
    testRelayConversionMatchesGet();

    std::printf("%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
