// The Phone/CW mic level survives a launch.
//
// TransmitModel used to construct m_micLevel at 50 and nothing restored it, so
// the one control an HL2 operator has over where their audio lands relative to
// the ALC's threshold reset to unity every launch — and setKeying()'s "raise
// mic gain" diagnostic pointed at a control that forgot the answer overnight.
// The restore is in the constructor deliberately: RadioModel pushes
// m_transmitModel.micLevel() to a freshly built backend, and TransmitModel is a
// member, so a value restored there is already in place by the time that push
// happens — no new wiring and no second source of truth.
//
// Own process, and own settings home: AppSettings is a process-wide singleton,
// so a test that wrote the real store would move the developer's own mic gain.
// Reconstructing TransmitModel in-process is the relaunch under test — the
// constructor is the only thing that reads the key.
//
// Runs against a GLOBAL key ("PhoneMicLevel"). Scoping it per radio is flagged
// open in the PR series and would need the value restored at connect rather
// than at construction; if that lands, this file is where the new scope is
// pinned.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringLiteral>

#include <cstdio>

using namespace AetherSDR;

namespace {

const QString kMicLevelKey = QStringLiteral("PhoneMicLevel");

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok)
        ++g_failures;
}

// A launch: build the model the way RadioModel owns it, and ask what the
// constructor restored.
int micLevelAfterRelaunch()
{
    TransmitModel tx;
    return tx.micLevel();
}

} // namespace

int main(int argc, char** argv)
{
    // BEFORE QCoreApplication and before the first AppSettings touch — the
    // profile redirects the settings home, and Qt caches those paths.
    TestSettingsProfile profile(QStringLiteral("transmit-model-mic-persistence-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ── A first launch, nothing stored ──────────────────────────────────────
    // 50 and only 50. The slider's mapping pins 50 to unity gain
    // (hl2::micSliderToLinear), so an operator who has never touched the
    // control must still get byte-identical transmit behaviour after this
    // change. Any other default here is a silent level change on every
    // existing install.
    check(!AppSettings::instance().contains(kMicLevelKey),
          "the test starts with no stored mic level");
    check(micLevelAfterRelaunch() == 50,
          "no stored key restores 50, the unity position");

    // ── Set 70, relaunch, still 70 ──────────────────────────────────────────
    // setMicLevel is the operator's slider. It writes inside the existing
    // changed-test, so the store sees a row only when the value actually
    // moved; the commit is AppSettings::save(), which MainWindow::closeEvent
    // already runs over every dirty row at quit.
    {
        TransmitModel tx;
        tx.setMicLevel(70);
        check(tx.micLevel() == 70, "the model adopts the operator's 70");
    }
    check(AppSettings::instance().value(kMicLevelKey).toInt() == 70,
          "moving the slider records the level under PhoneMicLevel");
    check(micLevelAfterRelaunch() == 70,
          "the operator's 70 survives a relaunch");

    // ── A hand-edited or corrupt store cannot command a nonsense level ──────
    // The slider is 0..100 and 0 MUTES, so an unclamped restore is not merely
    // untidy: a stored 250 would ask the modulator for +80 dB it does not
    // have, and a stored -10 would silently take the operator off the air.
    AppSettings::instance().setValue(kMicLevelKey, QStringLiteral("250"));
    check(micLevelAfterRelaunch() == 100, "an over-range stored level clamps to 100");

    AppSettings::instance().setValue(kMicLevelKey, QStringLiteral("-10"));
    check(micLevelAfterRelaunch() == 0, "an under-range stored level clamps to 0");

    // Unparseable is NOT zero. QVariant::toInt() answers 0 for "banana", which
    // is in range and therefore survives the clamp — and 0 is the mute. A store
    // this client cannot read must leave the operator at unity, not silently
    // off the air, so the restore falls back to the default rather than
    // trusting the conversion.
    AppSettings::instance().setValue(kMicLevelKey, QStringLiteral("banana"));
    check(micLevelAfterRelaunch() == 50,
          "a garbage stored level falls back to 50 rather than muting");

    AppSettings::instance().setValue(kMicLevelKey, QString());
    check(micLevelAfterRelaunch() == 50, "an empty stored level falls back to 50");

    if (g_failures == 0)
        std::printf("transmit_model_mic_persistence_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
