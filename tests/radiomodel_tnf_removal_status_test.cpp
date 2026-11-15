// SmartSDR answers `tnf remove <id>` with a status line — "tnf <id> removed"
// (bare token) or "tnf <id> removed=1" (kv form). The router used to match only
// "tnf <id>" and hand every match to TnfModel::applyTnfStatus(), which upserts
// via QMap::operator[]. So the removal status RE-CREATED the entry a beat after
// the operator's click, and the notch reappeared on the panadapter with the
// optimistic removal already done.
//
// This pins that the router routes both removal forms to removeTnf() and still
// upserts a genuine "tnf <id> <fields>" line.

#include "models/RadioModel.h"
#include "models/TnfModel.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    RadioModel model;   // default family is Flex — the command plane this bug lives on

    const QMap<QString, QString> tnf1 {
        {"freq", "14.100000"}, {"width", "100"}, {"depth", "1"}, {"permanent", "0"},
    };

    // A normal TNF status creates the entry.
    model.handleStatusForTest(QStringLiteral("tnf 1"), tnf1);
    check(model.tnfModel().tnf(1) != nullptr, "a 'tnf 1 <fields>' status creates the entry");

    // kv form: "tnf 1 removed=1" — object is still "tnf 1", removed lands in kvs.
    model.handleStatusForTest(QStringLiteral("tnf 1"), {{"removed", "1"}});
    check(model.tnfModel().tnf(1) == nullptr,
          "'tnf 1 removed=1' removes the entry rather than re-creating it");
    check(model.tnfModel().tnfs().isEmpty(), "no stray entries left after kv-form removal");

    // Bare form: the whole string lands in `object` as "tnf 1 removed".
    model.handleStatusForTest(QStringLiteral("tnf 1"), tnf1);
    check(model.tnfModel().tnf(1) != nullptr, "re-created for the bare-form check");
    model.handleStatusForTest(QStringLiteral("tnf 1 removed"), {});
    check(model.tnfModel().tnf(1) == nullptr,
          "'tnf 1 removed' (bare token) removes the entry");

    // A removal for an id that was already dropped optimistically is a no-op,
    // not a crash or a resurrected zero-value entry.
    model.handleStatusForTest(QStringLiteral("tnf 7 removed"), {});
    check(model.tnfModel().tnf(7) == nullptr, "removal of an unknown id is a harmless no-op");

    // A genuine status still upserts — the removal branch must not have eaten
    // the normal path.
    model.handleStatusForTest(QStringLiteral("tnf 2"), tnf1);
    check(model.tnfModel().tnf(2) != nullptr, "a normal 'tnf 2 <fields>' status still creates an entry");

    TnfModel confirmed;
    confirmed.setControlPolicy(ReceiveControlPolicy::Confirmed);
    NotchDelta initial;
    initial.centerHz = 14'200'000; initial.widthHz = 400;
    confirmed.applyNotchDelta(9, initial);
    int changes = 0;
    QObject::connect(&confirmed, &TnfModel::notchChangeRequested, &confirmed,
        [&](int, const NotchDelta&) { ++changes; });
    confirmed.setTnfFreq(9, 14.201);
    confirmed.setTnfWidth(9, 800);
    confirmed.requestGlobalTnfEnabled(false);
    check(changes == 2 && confirmed.tnf(9)->freqMhz == 14.2
              && confirmed.tnf(9)->widthHz == 400 && confirmed.globalEnabled(),
          "confirmed notch edits remain intents until graph adoption");
    // The backend may normalize a width and report it synchronously. Its
    // accepted observation must survive the remainder of the setter.
    const auto normalize = QObject::connect(&confirmed, &TnfModel::notchChangeRequested,
        &confirmed, [&](int id, const NotchDelta& request) {
            if (request.widthHz) {
                NotchDelta accepted; accepted.widthHz = 400;
                confirmed.applyNotchDelta(id, accepted);
            }
        });
    confirmed.setTnfWidth(9, 100);
    check(confirmed.tnf(9)->widthHz == 400,
          "confirmed normalized width echo is not overwritten by requested width");
    QObject::disconnect(normalize);
    NotchDelta accepted; accepted.centerHz = 14'201'000; accepted.widthHz = 800;
    confirmed.applyNotchDelta(9, accepted);
    confirmed.applyGlobalEnabled(false);
    check(confirmed.tnf(9)->freqMhz == 14.201 && confirmed.tnf(9)->widthHz == 800
              && !confirmed.globalEnabled(), "accepted notch and global bypass advance presentation");
    confirmed.requestRemoveTnf(9);
    check(confirmed.tnf(9) != nullptr, "pending removal keeps the accepted notch visible");
    confirmed.removeTnf(9);
    check(confirmed.tnf(9) == nullptr, "adopted removal deletes the notch");
    confirmed.setControlPolicy(ReceiveControlPolicy::Optimistic);
    confirmed.applyNotchDelta(9, initial);
    confirmed.setTnfFreq(9, 14.202);
    confirmed.setTnfWidth(9, 1200);
    confirmed.requestGlobalTnfEnabled(true);
    check(confirmed.tnf(9)->freqMhz == 14.202 && confirmed.tnf(9)->widthHz == 1200
              && confirmed.globalEnabled(), "legacy sessions retain optimistic notch edits");
    confirmed.requestRemoveTnf(9);
    check(!confirmed.tnf(9), "legacy sessions retain immediate removal");

    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
