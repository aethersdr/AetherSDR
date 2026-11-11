// HL2 receive antenna (#4954, the RX half). The HL2 has one receive input, ANT.
// The claims under test:
//   - every slice publishes rxAntennaList {ANT} and rxAntenna ANT, so the RX
//     menus have a real port and no longer fall back to invented ANT1/ANT2;
//   - RF1, a transmit-only output, is never in the RX list;
//   - a request for ANT is accepted with nothing to send, anything else is
//     refused, and both republish ANT so the button cannot keep a refused label;
//   - every slice publishes txAntenna ANT, so a slice in a RadioModel puts no
//     ANT1 into knownAntennaTokens(), which all three RX menus merge in;
//   - a tune step on an unchanged ANT emits no rxAntennaChanged/txAntennaChanged,
//     and a differing echo still does.
// Socket-free: a constructed backend; nothing is keyed.

#include "RadioSetupControlFixture.h"
#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "gui/AntennaChoiceGate.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <map>
#include <optional>

namespace AetherSDR::hl2 {

struct Hl2RxAntennaTestAccess {
    static void receivers(Hl2Backend& b, int n)
    {
        b.m_ids.reset(n);
        b.m_rx.assign(static_cast<std::size_t>(n), Hl2Backend::Receiver{});
    }
    static void publishAll(Hl2Backend& b) { b.emitAllSliceState(); }
};

} // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using A = Hl2RxAntennaTestAccess;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

namespace {

struct Heard {
    std::map<int, QStringList> list;
    std::map<int, QString> current;
    std::optional<SliceDelta> last;
};

void listen(Hl2Backend& b, Heard& h)
{
    QObject::connect(&b, &IRadioBackend::sliceChanged, &b, [&h](int id, const SliceDelta& d) {
        if (d.rxAntennaList) {
            h.list[id] = *d.rxAntennaList;
        }
        if (d.rxAntenna) {
            h.current[id] = *d.rxAntenna;
        }
        h.last = d;
    });
}

const QStringList kAntOnly{QStringLiteral("ANT")};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-rx-antenna-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    std::printf("\n  HL2 receive antenna: ANT only (#4954)\n");

    Hl2Backend b;
    A::receivers(b, 2);
    Heard h;
    listen(b, h);
    A::publishAll(b);
    check(h.list.size() == 2 && h.list[0] == kAntOnly && h.list[1] == kAntOnly,
          "every slice publishes rxAntennaList {ANT}");
    check(h.current[0] == QLatin1String("ANT") && h.current[1] == QLatin1String("ANT"),
          "every slice publishes rxAntenna ANT");
    check(!h.list[0].contains(QStringLiteral("RF1")) && !h.list[1].contains(QStringLiteral("RF1")),
          "RF1 is never offered for receive");

    // The menus (VfoWidget, RxApplet, SpectrumOverlayMenu) fall back to ANT1/ANT2
    // and the gate refuses only when the slice's published list is empty.
    SliceModel s(0);
    s.applyChanges(*h.last);
    check(s.rxAntennaList() == kAntOnly && s.rxAntenna() == QLatin1String("ANT"),
          "the slice model holds {ANT} and ANT");
    check(!rxAntennaChoiceRefused(true, !s.rxAntennaList().isEmpty(), false),
          "the RX antenna button opens on a real port instead of refusing");

    // The RX menus merge RadioModel::knownAntennaTokens(), which adds every
    // slice's txAntenna(). Unpublished, that is SliceModel's "ANT1" default: a
    // pick the dispatcher refuses before the backend can republish ANT.
    check(h.last->txAntenna && *h.last->txAntenna == QLatin1String("ANT"),
          "the published slice state carries txAntenna ANT");
    {
        RadioModel radio;
        SliceModel* rs = RadioSetupControlTestAccess::addSlice(radio, 0);
        rs->applyChanges(*h.last);
        const QStringList tokens = radio.knownAntennaTokens();
        check(rs->txAntenna() == QLatin1String("ANT"), "the slice's TX label reads ANT, not ANT1");
        check(!tokens.contains(QStringLiteral("ANT1")) && !tokens.contains(QStringLiteral("ANT2")),
              "knownAntennaTokens() (the RX menu tokens) holds no ANT1 or ANT2");
        // Not "no RF1": it is a TX token there once the TX route publishes it,
        // and rxAntennaMenuTokens() is what keeps it out of the RX menus.
        check(tokens.contains(QStringLiteral("ANT")), "knownAntennaTokens() holds ANT");
    }

    h = Heard{};
    check(b.requestSliceRxAntenna(1, QStringLiteral("ANT")) == ReceiveDispatch::LocalOnly
              && h.current[1] == QLatin1String("ANT"),
          "ANT is accepted with nothing to send and republished");
    bool refused = true;
    for (const char* token : {"ANT1", "ANT2", "RF1", "RX_A", ""}) {
        h = Heard{};
        refused = refused
            && b.requestSliceRxAntenna(0, QString::fromLatin1(token)) == ReceiveDispatch::Unsupported
            && h.current[0] == QLatin1String("ANT") && h.list[0] == kAntOnly;
    }
    check(refused, "ANT1, ANT2, RF1 and junk are refused and ANT is republished");
    check(b.requestSliceRxAntenna(7, QStringLiteral("ANT")) == ReceiveDispatch::Unsupported,
          "a slice that does not exist is refused");

    // Tuning does not republish unchanged HL2 antennas. A full refresh and an
    // explicit request still reconcile the labels, without shared-model guards.
    {
        SliceModel tuned(0);
        QObject::connect(&b, &IRadioBackend::sliceChanged, &tuned,
                         [&tuned](int id, const SliceDelta& d) {
            if (id == 0) {
                tuned.applyChanges(d);
            }
        });
        A::publishAll(b);
        int rxEmits = 0;
        int txEmits = 0;
        QObject::connect(&tuned, &SliceModel::rxAntennaChanged, [&rxEmits] { ++rxEmits; });
        QObject::connect(&tuned, &SliceModel::txAntennaChanged, [&txEmits] { ++txEmits; });
        for (int step = 1; step <= 10; ++step) {
            b.setSliceFrequency(0, 7'100'000.0 + 100.0 * step);
        }
        std::printf("  (10 tune steps: rxAntennaChanged=%d txAntennaChanged=%d)\n", rxEmits,
                    txEmits);
        check(rxEmits == 0 && txEmits == 0 && tuned.rxAntenna() == QLatin1String("ANT"),
              "tune steps on an unchanged ANT emit no rxAntennaChanged or txAntennaChanged");

        // A differing echo still reaches the listeners: a refused pick moved the
        // label, and the republished ANT moves it back.
        QObject::connect(&tuned, &SliceModel::receiveRxAntennaRequested, &b,
                         [&b](const QString& ant) { b.requestSliceRxAntenna(0, ant); });
        QStringList heardRx;
        QObject::connect(&tuned, &SliceModel::rxAntennaChanged,
                         [&heardRx](const QString& ant) { heardRx << ant; });
        tuned.setRxAntenna(QStringLiteral("ANT2"));
        check(heardRx == QStringList({QStringLiteral("ANT2"), QStringLiteral("ANT")})
                  && tuned.rxAntenna() == QLatin1String("ANT"),
              "a refused pick shows, then the differing ANT echo snaps the label back");
        txEmits = 0;
        SliceDelta moved;
        moved.txAntenna = QStringLiteral("RF1");
        tuned.applyChanges(moved);
        A::publishAll(b);
        check(txEmits == 2 && tuned.txAntenna() == QLatin1String("ANT"),
              "a differing txAntenna echo emits, each way");
    }

    // Preserve the status-notification contract other backends already use.
    {
        SliceModel shared(0);
        int rxEmits = 0;
        int txEmits = 0;
        QObject::connect(&shared, &SliceModel::rxAntennaChanged, &shared,
                         [&rxEmits] { ++rxEmits; });
        QObject::connect(&shared, &SliceModel::txAntennaChanged, &shared,
                         [&txEmits] { ++txEmits; });
        SliceDelta echo;
        echo.rxAntenna = QStringLiteral("ANT1");
        echo.txAntenna = QStringLiteral("ANT1");
        shared.applyChanges(echo);
        shared.applyChanges(echo);
        check(rxEmits == 2 && txEmits == 2,
              "shared antenna echoes retain their existing notification behavior");
    }

    std::printf("\n  %s (%d failure%s)\n\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
