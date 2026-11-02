// `get tracedebug <pan> bins` at the bridge (#5806): the refusals, and the
// meta-call name the bridge looks the method up by.

#include "core/AutomationServer.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QVariantMap>
#include <QWidget>

#include <algorithm>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}

// A stand-in the bridge finds by class name, so its handling of the widget's
// answer is tested without the desktop SpectrumWidget.
namespace TraceBinsFake {
class SpectrumWidget : public QWidget
{
    Q_OBJECT
public:
    QVariantMap answer;
    Q_INVOKABLE QVariantMap traceBinsSnapshot() const { return answer; }
};
}  // namespace TraceBinsFake

namespace {
int failures = 0;

void check(bool ok, const char* description)
{
    std::fprintf(stderr, "%s: %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) {
        ++failures;
    }
}

QByteArray readSource(const char* relativePath)
{
    QFile f(QString::fromLatin1(AETHER_SOURCE_DIR) + QString::fromLatin1(relativePath));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The body of the function whose definition starts with `signature`.
QByteArray functionBody(const QByteArray& source, const QByteArray& signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0) {
        return {};
    }
    const qsizetype end = source.indexOf("\n}\n", start);
    return end < 0 ? QByteArray() : source.mid(start, end - start);
}

// Every assignment to the shown centre or bandwidth that is not followed,
// within three lines, by noteTraceGeometryChange(). Returns the line numbers.
QList<int> unhookedGeometryWrites(const QByteArray& source)
{
    const QList<QByteArray> lines = source.split('\n');
    const QRegularExpression write(
        QStringLiteral("\\bm_(centerMhz|bandwidthMhz)\\s*=[^=]"));
    QList<int> out;
    for (int i = 0; i < lines.size(); ++i) {
        if (!write.match(QString::fromUtf8(lines.at(i))).hasMatch()) {
            continue;
        }
        bool hooked = false;
        for (int j = i; j < std::min<int>(i + 4, lines.size()); ++j) {
            hooked = hooked || lines.at(j).contains("noteTraceGeometryChange();");
        }
        if (!hooked) {
            out.append(i + 1);
        }
    }
    return out;
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    AetherSDR::AutomationServer server(nullptr);
    const auto request = [&server](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };

    QJsonObject r = request(R"({"cmd":"get","model":"tracedebug","property":"bins"})");
    check(!r.value("ok").toBool(), "bins without a pan is refused");
    check(r.value("error").toString().contains(QLatin1String("needs one pan")),
          "and the refusal says a pan is needed");

    r = request(R"({"cmd":"get","model":"tracedebug","selector":"0","property":"bins"})");
    check(!r.value("ok").toBool(), "bins for a pan that does not exist is refused");
    check(r.value("error").toString().contains(QLatin1String("no pan matches '0'")),
          "and the refusal names the selector");

    r = request(R"({"cmd":"get","model":"tracedebug"})");
    check(r.value("ok").toBool() && r.value("pans").isArray(),
          "tracedebug without a property still answers with the pans array");

    TraceBinsFake::SpectrumWidget pan;
    pan.setObjectName(QStringLiteral("trace-bins-fake"));
    pan.answer = {{QStringLiteral("panIndex"), 0},
                  {QStringLiteral("refusal"),
                   QStringLiteral("geometry changed 1 frames ago; wait for 3 settled frames")}};
    r = request(R"({"cmd":"get","model":"tracedebug","selector":"0","property":"bins"})");
    check(!r.value("ok").toBool(), "a widget refusal is an ok:false reply");
    check(r.value("error").toString()
              == QLatin1String("tracedebug bins: geometry changed 1 frames ago; wait for 3 settled frames"),
          "and the reply carries the widget's reason");
    check(!r.contains(QLatin1String("pan")), "and no bins");

    pan.answer = {{QStringLiteral("panIndex"), 0},
                  {QStringLiteral("count"), 2},
                  {QStringLiteral("ageMs"), 40},
                  {QStringLiteral("bins"), QVariantList{-120.5, -119.0}}};
    r = request(R"({"cmd":"get","model":"tracedebug","selector":"0","property":"bins"})");
    const QJsonObject panReply = r.value("pan").toObject();
    check(r.value("ok").toBool() && panReply.value("ageMs").toInt() == 40
              && panReply.value("bins").toArray().size() == 2,
          "an accepted frame passes through with its age");

    // The bridge reaches the widget by method NAME; a misspelling would only
    // ever show as "no pan matches". Pin the name on both sides.
    const QByteArray server_src = readSource("/src/core/AutomationServer.cpp");
    const QByteArray widget_h = readSource("/src/gui/SpectrumWidget.h");
    check(!server_src.isEmpty() && !widget_h.isEmpty(), "the test can read both sources");
    check(server_src.contains("\"traceBinsSnapshot\""), "the bridge invokes traceBinsSnapshot");
    check(widget_h.contains("Q_INVOKABLE QVariantMap traceBinsSnapshot() const;"),
          "SpectrumWidget declares traceBinsSnapshot as Q_INVOKABLE");

    // The widget's wiring. SpectrumWidget.cpp is built only into the desktop
    // executable (and the opt-in spectrum_trace_bins_widget_test), so the
    // default lane can pin its source but not run it.
    const QByteArray widget_cpp = readSource("/src/gui/SpectrumWidget.cpp");
    const QByteArray snapshotBody =
        functionBody(widget_cpp, "QVariantMap SpectrumWidget::traceBinsSnapshot() const");
    check(snapshotBody.contains("m_traceCapture.refusal(")
              && snapshotBody.contains("m_traceCapture.snapshot()")
              && !snapshotBody.contains("m_bins"),
          "traceBinsSnapshot answers from the captured frame, never m_bins");
    check(functionBody(widget_cpp, "void SpectrumWidget::updateSpectrum(")
              .contains("m_traceCapture.ingest(binsDbm,"),
          "updateSpectrum captures the frame as received");
    check(functionBody(widget_cpp, "void SpectrumWidget::clearDisplay()")
              .contains("m_traceCapture.clear();"),
          "clearDisplay clears the captured frame");
    const QByteArray internalBody =
        functionBody(widget_cpp, "void SpectrumWidget::setFrequencyRangeInternal(");
    const qsizetype observed = internalBody.indexOf("m_traceCapture.noteRangeObserved(");
    check(observed >= 0 && observed < internalBody.indexOf("return;"),
          "every range echo is offered to the unconfirmed request before any early return");
    const QList<int> unhooked = unhookedGeometryWrites(widget_cpp);
    for (int line : unhooked) {
        std::fprintf(stderr, "  unhooked geometry write at SpectrumWidget.cpp:%d\n", line);
    }
    check(!widget_cpp.isEmpty() && unhooked.isEmpty(),
          "every write of the shown centre or bandwidth notes a trace geometry change");

    if (failures == 0) {
        std::fprintf(stderr, "automation_tracedebug_bins_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}

#include "automation_tracedebug_bins_test.moc"
