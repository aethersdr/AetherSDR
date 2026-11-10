// The WSPR receiver's slice is another client's, never AetherSDR's.
//
// The radio reports every headless slice AetherWSPR makes as owned by the GUI
// client connected then, and hands it to the next GUI client to connect when
// none was. RadioModel shows that slice as a foreign slot once it knows which
// it is (setExternalSliceClient), holds its own slices on connect until then,
// and recognizes slices the receiver makes later by their status's source
// handle. WsprSliceGuard asks the receiver. Socket-free: the receiver's
// answer is injected.

#include "core/WsprAgent.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/WsprSliceGuard.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", what);
    if (!ok) { ++g_failures; }
}

constexpr quint32 kOurs = 0x613C5DA8;
constexpr quint32 kWspr = 0x2A5F27D7;
const QString kLabel = QStringLiteral("WSPR receiver");

QString hex(quint32 h) { return QStringLiteral("0x%1").arg(h, 8, 16, QLatin1Char('0')).toUpper(); }

// A slice's full status, as the radio sends it on "client gui" or creation.
QMap<QString, QString> sliceStatus(int id, quint32 owner, const QString& mhz)
{
    return {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("RF_frequency"), mhz},
            {QStringLiteral("client_handle"), hex(owner)},
            {QStringLiteral("index_letter"), QString(QChar('A' + id))},
            {QStringLiteral("mode"), QStringLiteral("DIGU")}};
}

bool foreignToWspr(RadioModel& m, int id)
{
    return !m.slice(id) && m.isSlotForeign(id) && m.foreignSliceOwnerStation(id) == kLabel;
}

// Connecting: the radio's dump hands us the receiver's slice D with our own
// A. Held until the receiver answers; then A is ours and D another client's,
// and stays so as the receiver retunes it, until it is removed.
void connectHoldsUntilTheReceiverAnswers()
{
    RadioModel m;
    m.rebuildBackendForTest(QStringLiteral("flex"));
    m.setClientHandleForTest(kOurs);
    m.setSliceAdoptionHoldOnConnect(2000);
    m.beginSliceAdoptionHoldForTest();
    check(m.sliceAdoptionHeld(), "connect: our slices are held");
    m.handleSliceStatusForTest(0, sliceStatus(0, kOurs, QStringLiteral("14.074000")));
    m.handleSliceStatusForTest(3, sliceStatus(3, kOurs, QStringLiteral("7.038600")));
    m.handleSliceStatusForTest(0, {{QStringLiteral("RF_frequency"), QStringLiteral("14.075000")}});
    check(m.slices().isEmpty(), "nothing is adopted while held");

    m.setExternalSliceClient(kWspr, 3, kLabel);
    m.releaseSliceAdoption();
    check(!m.sliceAdoptionHeld(), "the receiver's answer ends the hold");
    check(m.slice(0) && m.slices().size() == 1, "our slice A is adopted");
    check(m.slice(0) && qAbs(m.slice(0)->frequency() - 14.075) < 1e-6,
          "with the update that arrived while held");
    check(foreignToWspr(m, 3), "the receiver's slice D is a foreign slot, in use by the WSPR receiver");

    m.handleSliceStatusForTest(3, {{QStringLiteral("RF_frequency"), QStringLiteral("3.568600")}});
    m.handleSliceStatusForTest(3, {{QStringLiteral("in_use"), QStringLiteral("1")},
                                   {QStringLiteral("RF_frequency"), QStringLiteral("3.568600")}});
    check(foreignToWspr(m, 3), "the receiver retuning it leaves it foreign");

    m.handleSliceStatusForTest(3, {}, true);
    check(!m.isSlotForeign(3) && m.externalSlice() == -1, "removed, the slot is free again");
}

// While connected, a slice the receiver makes is recognized by its status's
// source handle, before anything adopts it; ours are adopted as before.
void slicesTheReceiverMakesLater()
{
    RadioModel m;
    m.rebuildBackendForTest(QStringLiteral("flex"));
    m.setClientHandleForTest(kOurs);
    m.setExternalSliceClient(kWspr, -1, kLabel);
    m.handleStatusFromForTest(kOurs, QStringLiteral("slice 0"), sliceStatus(0, kOurs, QStringLiteral("14.074000")));
    m.handleStatusFromForTest(kWspr, QStringLiteral("slice 2"), sliceStatus(2, kOurs, QStringLiteral("7.038600")));
    check(m.slice(0) != nullptr, "a slice we made is ours");
    check(foreignToWspr(m, 2) && m.externalSlice() == 2,
          "a slice the receiver made is the receiver's, though the radio says ours");
}

// A late answer, after the slice was adopted: it stops being ours.
void lateAnswerReleasesAnAdoptedSlice()
{
    RadioModel m;
    m.rebuildBackendForTest(QStringLiteral("flex"));
    m.setClientHandleForTest(kOurs);
    m.handleSliceStatusForTest(0, sliceStatus(0, kOurs, QStringLiteral("14.074000")));
    m.handleSliceStatusForTest(3, sliceStatus(3, kOurs, QStringLiteral("7.038600")));
    check(m.slice(3) != nullptr, "without an answer the receiver's slice looks like ours");
    m.setExternalSliceClient(kWspr, 3, kLabel);
    check(foreignToWspr(m, 3) && m.slice(0), "named later, it becomes the receiver's; A stays ours");
}

// No answer: the hold ends by itself and everything is adopted as before.
void holdTimesOut()
{
    RadioModel m;
    m.rebuildBackendForTest(QStringLiteral("flex"));
    m.setClientHandleForTest(kOurs);
    m.setSliceAdoptionHoldOnConnect(50);
    m.beginSliceAdoptionHoldForTest();
    m.handleSliceStatusForTest(3, sliceStatus(3, kOurs, QStringLiteral("7.038600")));
    check(m.slices().isEmpty(), "held");
    check(QTest::qWaitFor([&] { return !m.sliceAdoptionHeld(); }, 2000), "the hold times out");
    check(m.slice(3) != nullptr, "and the held slice is adopted");
}

// WsprSliceGuard asks the receiver when the hold starts, names its slice,
// and ends the hold. One from before 0.1.3 names nothing; no answer, nothing.
void guardAsksTheReceiver()
{
    {
        RadioModel m;
        m.setClientHandleForTest(kOurs);
        m.setRadioAddressForTest(QHostAddress(QStringLiteral("192.168.50.100")));
        WsprSliceGuard guard(&m);
        QHostAddress asked;
        guard.setStatusSource([&](const QHostAddress& radio, std::function<void(std::optional<WsprStatus>)> done) {
            asked = radio;
            WsprStatus st;
            st.clientHandle = kWspr;
            st.sliceIndex = 3;
            done(st);
        });
        m.beginSliceAdoptionHoldForTest();
        check(asked == QHostAddress(QStringLiteral("192.168.50.100")), "the guard asks the receiver at the radio");
        check(!m.sliceAdoptionHeld() && m.externalSlice() == 3, "names its slice and ends the hold");
        // The dump arriving after the answer.
        m.handleSliceStatusForTest(3, sliceStatus(3, kOurs, QStringLiteral("7.038600")));
        m.handleSliceStatusForTest(0, sliceStatus(0, kOurs, QStringLiteral("14.074000")));
        check(foreignToWspr(m, 3) && m.slice(0), "the dump then gives us A, and D stays the receiver's");
    }
    {
        RadioModel m;
        m.setClientHandleForTest(kOurs);
        m.setRadioAddressForTest(QHostAddress(QStringLiteral("192.168.50.100")));
        WsprSliceGuard guard(&m);
        guard.setStatusSource([&](const QHostAddress&, std::function<void(std::optional<WsprStatus>)> done) {
            done(WsprStatus{});   // 0.1.2 and earlier: no slice_index, no client_handle
        });
        m.beginSliceAdoptionHoldForTest();
        check(!m.sliceAdoptionHeld() && m.externalSlice() == -1, "an older receiver names nothing");
        guard.setStatusSource([&](const QHostAddress&, std::function<void(std::optional<WsprStatus>)> done) {
            done(std::nullopt);
        });
        m.beginSliceAdoptionHoldForTest();
        check(!m.sliceAdoptionHeld() && m.externalSlice() == -1, "no receiver: the hold just ends");
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    connectHoldsUntilTheReceiverAnswers();
    slicesTheReceiverMakesLater();
    lateAnswerReleasesAnAdoptedSlice();
    holdTimesOut();
    guardAsksTheReceiver();
    return g_failures ? 1 : 0;
}
