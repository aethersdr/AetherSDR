// TCI tx_sensors follows this station's own transmission only, and the
// transmitter ownership it reads does not outlive the session it came from.
// Socket-free: the real RadioModel and TransmitModel, the static
// TciServer::txSensorsKeyed() predicate, and an injected interlock status.
// No TciServer is started, no listener, no client, no radio. Nothing keys:
// the "keyed" state is model state set directly, never a command.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "core/backends/InterlockStatusDelta.h"
#include "core/backends/TransmitDelta.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

namespace AetherSDR {

// Friend of both RadioModel and TciServer.
class TciServerReviewTest {
public:
    static bool keyed(RadioModel& model) { return TciServer::txSensorsKeyed(model); }
    static void holder(RadioModel& model, quint32 handle)
    {
        InterlockStatusDelta d;
        d.txClientHandle = handle;
        model.applyInterlockStatus(d);
    }
    static void radioKey(RadioModel& model, bool on) { model.m_radioTransmitting = on; }
    static void disconnect(RadioModel& model)
    {
        model.m_intentionalDisconnect = true;
        model.onDisconnected();
    }
};

} // namespace AetherSDR

using namespace AetherSDR;
using T = TciServerReviewTest;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) { ++g_failures; }
}

static void setTune(RadioModel& model, bool on)
{
    TransmitDelta tune;
    tune.tune = on;
    model.transmitModel().applyChanges(tune);
}

// Tune is a transmission, and it is when an operator reads SWR.
static void followsTune()
{
    RadioModel model;
    check(!T::keyed(model), "idle: no tx_sensors");
    setTune(model, true);
    check(T::keyed(model), "tune alone sends tx_sensors");
    setTune(model, false);
    check(!T::keyed(model), "tune off stops tx_sensors");
}

// On a multiFLEX radio another station's power and SWR are not ours.
static void followsOwnTransmissionOnly()
{
    RadioModel model;
    check(model.txOwnedByUs(), "nobody holds the transmitter at start");
    T::radioKey(model, true);
    check(T::keyed(model), "own radio-keyed transmission sends tx_sensors");

    T::holder(model, 0x40000001u);
    check(!model.txOwnedByUs(), "fixture: another station holds the transmitter");
    check(!T::keyed(model), "another station's transmission sends no tx_sensors");
    setTune(model, true);
    check(!T::keyed(model), "another station's tune sends no tx_sensors");
    setTune(model, false);

    T::holder(model, 0u);
    check(T::keyed(model), "transmitter released: own key sends again");
    T::radioKey(model, false);
    check(!T::keyed(model), "un-keyed: no tx_sensors");
}

// Disconnecting while another station holds the transmitter must not carry
// that ownership into the next session. Only the Flex interlock rewrites it,
// so an HL2 or demo session after it would otherwise never send tx_sensors.
static void ownershipDoesNotOutliveTheSession()
{
    RadioModel model;
    T::holder(model, 0x40000001u);
    T::radioKey(model, true);
    check(!model.txOwnedByUs() && !T::keyed(model),
          "fixture: another station holds and keys the transmitter");

    T::disconnect(model);
    check(model.txOwnedByUs(), "after disconnect: nobody holds the transmitter");
    check(model.txClientHandle() == 0, "after disconnect: no transmitter client handle");
    check(!T::keyed(model), "after disconnect: nothing is keyed");

    // The next session reports no interlock owner, as on an HL2 or the demo.
    setTune(model, true);
    check(T::keyed(model), "next session: own tune sends tx_sensors");
    setTune(model, false);
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-tx-sensors-gate"));
    if (!profile.isValid()) { return 1; }
    // RadioModel owns a DaxIqModel; never let it reach the user's live audio
    // server through pactl (#6726).
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    followsTune();
    followsOwnTransmissionOnly();
    ownershipDoesNotOutliveTheSession();

    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All checks passed\n");
    return 0;
}
