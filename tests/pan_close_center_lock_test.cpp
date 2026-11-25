// An operator's pan close (title-bar X, Close Active Panadapter) forgets the
// pan's Center Lock and its saved intent only once RadioModel::removePanadapter()
// accepts the close (maintainer ruling on #7359, applied to Center Lock in
// #7584). A refused close leaves both as they were. Pure helper; no radio.

#include "gui/PanCloseCenterLock.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

const QString kRadio = QStringLiteral("1234-5678/Station");
const QString kOther = QStringLiteral("9999-0000/Station");

PanCloseCenterLock::SavedIntents store()
{
    PanCloseCenterLock::SavedIntents saved;
    saved[kRadio].insert(0, QStringLiteral("A"));
    saved[kRadio].insert(1, QStringLiteral("B"));
    saved[kOther].insert(1, QStringLiteral("C"));
    return saved;
}

struct Run {
    bool accepted{false};
    bool liveForgotten{false};
    bool savedChanged{false};
    PanCloseCenterLock::SavedIntents saved;
};

Run close(const PanCloseCenterLock::Slot& slot, bool modelAccepts)
{
    Run run;
    run.saved = store();
    run.accepted = PanCloseCenterLock::closeThenForget(
        slot, run.saved, [modelAccepts] { return modelAccepts; },
        [&run] { run.liveForgotten = true; }, run.savedChanged);
    return run;
}

} // namespace

int main()
{
    const PanCloseCenterLock::Slot pan1{kRadio, 1, false};

    {
        // Multi-client Flex, X on a pan another client owns: removePanadapter()
        // returns false at ownedByClient. Any refusal looks the same here.
        const Run run = close(pan1, false);
        check(!run.accepted, "refused close: the helper reports the refusal");
        check(!run.liveForgotten, "refused close: the live Center Lock stays");
        check(!run.savedChanged && run.saved == store(),
              "refused close: the saved intent stays, nothing to persist");
    }
    {
        const Run run = close(pan1, true);
        PanCloseCenterLock::SavedIntents expected = store();
        expected[kRadio].remove(1);
        check(run.accepted && run.liveForgotten, "accepted close: the live Center Lock is forgotten");
        check(run.savedChanged && run.saved == expected,
              "accepted close: only this slot's saved intent is forgotten, and it is persisted");
    }
    {
        const Run run = close({kRadio, 1, true}, true);
        check(run.accepted && run.liveForgotten && !run.savedChanged && run.saved == store(),
              "session-only (TCI) lock: an accepted close forgets it and leaves the saved intent");
    }
    {
        // A native close destroys the widget, so the slot is read before the
        // close; an unknown slot (no widget, no radio key) forgets nothing saved.
        const Run noIndex = close({kRadio, -1, false}, true);
        const Run noRadio = close({QString(), 1, false}, true);
        check(noIndex.liveForgotten && !noIndex.savedChanged && noIndex.saved == store()
                  && noRadio.liveForgotten && !noRadio.savedChanged && noRadio.saved == store(),
              "accepted close with no slot index or radio key: live lock forgotten, saved store untouched");
    }
    {
        PanCloseCenterLock::SavedIntents saved;
        saved[kRadio].insert(2, QStringLiteral("A"));
        const bool changed = PanCloseCenterLock::forgetSaved(saved, {kRadio, 2, false});
        check(changed && !saved.contains(kRadio), "forgetting a radio's last slot removes the radio's entry");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
