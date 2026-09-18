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

    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
