// Regression test for #4419: qCInfo on QtWarningMsg-declared categories could
// never emit — applyFilterRules() re-enabled .debug only, so enabling a
// category in Support & Diagnostics left its Info lines dead under every
// runtime configuration the app can produce.
//
// Asserts the toggle now governs Info the same way it governs Debug, and pins
// the behaviors the fix must not disturb: default-off stays default-off,
// warnings always pass, the aether.audio.summary always-on Info special case
// survives, and Info on QtDebugMsg-declared categories (visible by
// declaration, untouched by any rule) is not newly suppressed.

#include "core/LogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

// Captured qCInfo payloads, to prove the emit path end-to-end rather than
// only the category flags.
QStringList g_infoMessages;

void captureHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtInfoMsg)
        g_infoMessages << QString::fromLatin1(ctx.category ? ctx.category : "") + ": " + msg;
}

} // namespace

int main(int argc, char** argv)
{
    // Defense-in-depth only: the save path in setEnabled()/setAllEnabled() is
    // intentionally a no-op here — AppSettings refuses to save before a
    // successful load(), which this test never performs (the "refusing to
    // save" warnings in the output are expected). The sandbox guards against
    // that refusal ever changing.
    QTemporaryDir fakeHome(QDir::tempPath() + "/aether-logmanager-filter-test-XXXXXX");
    if (!fakeHome.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    qputenv("HOME", fakeHome.path().toUtf8());
    qputenv("CFFIXED_USER_HOME", fakeHome.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", fakeHome.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);

    LogManager& lm = LogManager::instance();
    lm.setAllEnabled(false);

    // Baseline: category disabled — Debug and Info both gated, Warning passes.
    report("disabled: debug off", !lcCat().isDebugEnabled());
    report("disabled: info off", !lcCat().isInfoEnabled());
    report("disabled: warning still on", lcCat().isWarningEnabled());

    // The #4419 assertion: enabling a QtWarningMsg-declared category enables
    // its Info level alongside Debug.
    lm.setEnabled("aether.cat", true);
    report("enabled: debug on", lcCat().isDebugEnabled());
    report("enabled: info on (#4419)", lcCat().isInfoEnabled());

    // End-to-end: a qCInfo on the enabled category reaches the handler.
    g_infoMessages.clear();
    QtMessageHandler prev = qInstallMessageHandler(captureHandler);
    qCInfo(lcCat) << "info probe";
    qInstallMessageHandler(prev);
    report("enabled: qCInfo emitted",
           g_infoMessages.size() == 1 && g_infoMessages.first().startsWith("aether.cat"));

    // Enabling one category does not light up another.
    report("other category stays dark", !lcDax().isInfoEnabled());

    // Behaviors the fix must not disturb.
    report("audio.summary info always on", lcAudioSummary().isInfoEnabled());
    report("QtDebugMsg-declared info untouched", lcKiwiSdr().isInfoEnabled());

    // #4750: the aether.cat checkbox must NAME the TCI server, which owns 53 of
    // the category's 76 call sites. The old label said only "CAT/rigctld", so a
    // user hunting TCI diagnostics scanned Help → Support, found no mention of
    // TCI, and concluded there was none to enable. The label is the checkbox
    // text and the description is its tooltip, so the label is what has to
    // carry it — a user who has to open a tooltip to discover the category has
    // already given up on it.
    //
    // Asserted on the metadata rather than by eyeballing the dialog because
    // this is precisely the kind of drift that returns: the id is stable and
    // the call sites moved to TCI over time while the human-readable name kept
    // pointing at the original minority user.
    {
        QString catLabel, catDesc;
        bool found = false;
        for (const auto& c : lm.categories()) {
            if (c.id == "aether.cat") { catLabel = c.label; catDesc = c.description; found = true; break; }
        }
        report("aether.cat is registered", found);
        report("aether.cat label names TCI (#4750)", catLabel.contains("TCI", Qt::CaseInsensitive));
        // The rename must not drop the surfaces that were already discoverable.
        report("aether.cat label keeps CAT", catLabel.contains("CAT", Qt::CaseSensitive));
        report("aether.cat label keeps rigctld", catLabel.contains("rigctld", Qt::CaseInsensitive));
        report("aether.cat description mentions TCI", catDesc.contains("TCI", Qt::CaseInsensitive));
    }

    // #2554: aether.render must be REGISTERED, not merely declared. A category
    // that exists only as a Q_LOGGING_CATEGORY is swept up by the blanket
    // "aether.*.debug=false" in applyFilterRules(), and no UI toggle can switch
    // it back on, because the toggle list is built from m_categories. That is
    // not hypothetical: aether.hl2 shipped that way and silently hid the TX IQ
    // FIFO depth until someone noticed. Assert reachability, not just presence.
    {
        bool found = false;
        for (const auto& c : lm.categories()) {
            if (c.id == "aether.render") { found = true; break; }
        }
        report("aether.render is registered", found);

        lm.setEnabled("aether.render", true);
        report("aether.render can be switched on", lcRender().isInfoEnabled());
        lm.setEnabled("aether.render", false);
        report("aether.render can be switched off", !lcRender().isInfoEnabled());
    }

    // Toggling back off closes the gate again.
    lm.setEnabled("aether.cat", false);
    report("re-disabled: info off", !lcCat().isInfoEnabled());
    g_infoMessages.clear();
    prev = qInstallMessageHandler(captureHandler);
    qCInfo(lcCat) << "info probe while disabled";
    qInstallMessageHandler(prev);
    report("re-disabled: qCInfo suppressed", g_infoMessages.isEmpty());

    return g_failed == 0 ? 0 : 1;
}
