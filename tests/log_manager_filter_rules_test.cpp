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
