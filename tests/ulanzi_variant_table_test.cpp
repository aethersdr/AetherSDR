// Ulanzi unsupported-OEM-variant recognition and advisory state transitions.
//
// Two invariants, both from the review of #3485's fix:
//
//   1. A variant is recognised only on an exact VID/PID pair.  0xFFF1 is
//      unassigned placeholder VID space that cheap BLE-HID firmware reuses,
//      so a vendor-only match would confidently mislabel unrelated hardware
//      as a D100H — worse than the "Disconnected" the advisory replaces.
//
//   2. The advisory publishes on transitions in BOTH directions.  The hotplug
//      timer re-scans every few seconds: emitting unconditionally spams the
//      dialog, but suppressing the empty case leaves it claiming an unplugged
//      dial is still present, and deduping on the name alone means
//      present -> absent -> same variant present never re-notifies.
//
// Break them on purpose to see the test earn its keep: in
// UlanziVariantTable.h, drop the `pid == 0 ||` guard's companion `pid ==
// v.pid` check so the match is vendor-only and "unknown PID in the shared
// 0xFFF1 space is not claimed" fails; make shouldPublish() return
// `!current.isEmpty() && lastPublished != current` and both the withdrawal
// and the re-notify cases fail.

#include "core/UlanziVariantTable.h"

#include <QString>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

// Mirrors UlanziDialWindowsManager's publish bookkeeping: the value last
// emitted, updated only when a transition is published.
struct Publisher {
    QString lastPublished;
    int     emissions = 0;
    QString lastEmitted;

    void observe(const QString& current)
    {
        if (!UlanziVariant::shouldPublish(lastPublished, current))
            return;
        lastPublished = current;
        lastEmitted   = current;
        ++emissions;
    }
};

const QString kD100H = QStringLiteral("Ulanzi D100H (KEHWIN \"Dial_Lite\", BLE)");
const QString kD200  = QStringLiteral("Ulanzi D200 (Zkswe \"ulanzi\", USB)");

} // namespace

int main()
{
    bool ok = true;

    // ---- 1. Recognition is exact-pair, not vendor-wide. ----
    ok &= expect(UlanziVariant::lookup(0xFFF1, 0x0082) == kD100H,
                 "KEHWIN D100H is recognised by its exact VID/PID");
    ok &= expect(UlanziVariant::lookup(0x2207, 0x0019) == kD200,
                 "Zkswe D200 is recognised by its exact VID/PID");

    // The whole point of pinning the PID: 0xFFF1 is shared placeholder space.
    ok &= expect(UlanziVariant::lookup(0xFFF1, 0x1234).isEmpty(),
                 "unknown PID in the shared 0xFFF1 space is not claimed");
    ok &= expect(UlanziVariant::lookup(0x2207, 0x0001).isEmpty(),
                 "unknown PID under the Zkswe VID is not claimed");
    ok &= expect(UlanziVariant::lookup(0x1234, 0x0082).isEmpty(),
                 "matching PID under an unrelated VID is not claimed");

    // ---- 2. Transition publishing. ----

    // Steady state must not spam: the hotplug timer rescans every few seconds.
    {
        Publisher p;
        p.observe(kD100H);
        const int afterFirst = p.emissions;
        for (int i = 0; i < 5; ++i) p.observe(kD100H);
        ok &= expect(afterFirst == 1 && p.emissions == 1,
                     "a variant present across repeated rescans emits once");
    }

    // Disappearance must be published, or the dialog lies about the hardware.
    {
        Publisher p;
        p.observe(kD100H);
        p.observe(QString());
        ok &= expect(p.emissions == 2 && p.lastEmitted.isEmpty(),
                     "unplugging the variant publishes an empty withdrawal");

        for (int i = 0; i < 3; ++i) p.observe(QString());
        ok &= expect(p.emissions == 2,
                     "staying absent does not re-emit the withdrawal");
    }

    // The case the name-only dedupe got wrong.
    {
        Publisher p;
        p.observe(kD100H);
        p.observe(QString());
        p.observe(kD100H);
        ok &= expect(p.emissions == 3 && p.lastEmitted == kD100H,
                     "present -> absent -> same variant present re-notifies");
    }

    // Swapping one unsupported dial for another is a transition too.
    {
        Publisher p;
        p.observe(kD100H);
        p.observe(kD200);
        ok &= expect(p.emissions == 2 && p.lastEmitted == kD200,
                     "swapping to a different variant publishes the new name");
    }

    // A supported dial opening clears the advisory (rescan() empties the name).
    {
        Publisher p;
        p.observe(kD100H);
        p.observe(QString());
        ok &= expect(p.lastEmitted.isEmpty(),
                     "a supported dial taking over withdraws the advisory");
    }

    std::cout << (ok ? "ALL PASS" : "FAILURES") << '\n';
    return ok ? 0 : 1;
}
