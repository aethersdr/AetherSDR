// Without a ready direct connection or FlexRadio relay, TGXL mode commands
// emit TunerModel::relayedCommandRefused. MainWindow turns that into a
// transport-specific notice. MainWindow cannot be constructed in a
// test, so this pins the connection in the wiring source: the one claim no
// behavioural seam reaches. TunerModel's emission is covered behaviourally by
// tuner_model_test.

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdio>

namespace {
int failures = 0;

void report(const char* what, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++failures;
}

QByteArray readSource(const char* relative)
{
    QFile f(QString::fromUtf8(AETHER_SOURCE_DIR) + QLatin1Char('/') + QLatin1String(relative));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

int main()
{
    const QByteArray wiring = readSource("src/gui/MainWindow_Wiring.cpp");
    report("the test can read MainWindow_Wiring.cpp", !wiring.isEmpty());
    const int at = wiring.indexOf("&TunerModel::relayedCommandRefused");
    report("MainWindow connects TunerModel::relayedCommandRefused", at >= 0);
    const int end = at >= 0 ? wiring.indexOf("});", at) : -1;
    const QByteArray handler = end > at ? wiring.mid(at, end - at) : QByteArray();
    report("the refusal uses the shared status-notice latch",
           handler.contains("showUnsupportedControlNotice(")
               && handler.contains("no ready direct connection")
               && handler.contains("FlexRadio relay"));
    return failures == 0 ? 0 : 1;
}
