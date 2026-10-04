// A panadapter display control the client applied itself is not reported as
// "nothing was sent to the radio".
//
// WtrFall Gain, Black Level, Clone to all Pans and Reset display defaults set
// their value on the SpectrumWidget and then hand it to RadioModel for the
// radio. RadioModel::sendCmd() drops Flex wire text on a backend with no
// command plane and emits commandDropped, which MainWindow shows once per
// session. This test pins when that send is withheld and when it is not.
//
// WHAT IS PROVED, AND HOW.
//
// 1. By execution, on RadioModel with an injected backend whose capabilities()
//    are read off the real Hl2Backend / IcomCivBackend / FlexBackend:
//      - no command plane, absolute-dB rows (HL2): none of the four raises
//        commandDropped;
//      - no command plane, rows the manual black point cannot reach (Icom):
//        WtrFall Gain raises nothing, Black Level still raises commandDropped;
//      - a command plane (Flex): the wire text, compared whole, per pan.
//    "The client applied it" is checked on the renderer's own law
//    (WaterfallLevelMap::level) with the row unit taken from the same
//    capabilities, not asserted.
//
// 2. By SOURCE TEXT, for one claim no behavioural seam reaches: the four sites
//    in MainWindow_Wiring.cpp set the widget first and then call the two
//    RadioModel setters, and build no `display panafall set` gain or black
//    text of their own. They are MainWindow lambdas and no test target
//    constructs a MainWindow. It shows how the sites are written, not that
//    they run.
//
// Socket-free. No radio, no peer, no application window.

#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "gui/WaterfallLevelMap.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {
// RadioModel and RadioConnection befriend a class of this name for socket-free
// tests; this is this file's definition of it. It hands the model an unopened
// connection as its command plane and captures what is written to it.
class TxOperationIntegrationTestAccess {
public:
    static void useCommandPlane(RadioModel& radio, RadioConnection& connection,
                                QStringList& written)
    {
        connection.m_commandSinkForTest =
            [&written](quint32, const QString& command) { written << command; };
        radio.m_connection = &connection;
    }
    static void releaseCommandPlane(RadioModel& radio) { radio.m_connection = nullptr; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const QString& message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(message));
    failures += !ok;
}

class StubBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// Each family's declaration, read off a real backend instance (never dialled).
RadioCapabilities hl2Capabilities()
{
    hl2::Hl2Backend backend;
    return backend.capabilities();
}
RadioCapabilities icomCapabilities()
{
    icom::IcomCivBackend backend;
    return backend.capabilities();
}
RadioCapabilities flexCapabilities()
{
    FlexBackend backend;
    return backend.capabilities();
}

// A RadioModel on an injected backend with two panes, made the way a backend
// without Flex status makes them: from its first pan geometry.
struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};
    QStringList dropped;
    QString pan0;
    QString pan1;
    QString wf0;
    QString wf1;

    explicit Fixture(const RadioCapabilities& caps)
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        backend->caps = caps;
        radio.setBackendForTest(std::move(owned), caps.family);
        QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                         [this](const QString& cmd) { dropped << cmd; });
        emit backend->panCenterBandwidthChanged(QStringLiteral("first"), 7.100, 0.384);
        emit backend->panCenterBandwidthChanged(QStringLiteral("second"), 14.100, 0.384);
        pan0 = RadioModel::neutralPanIdStringForTest(0);
        pan1 = RadioModel::neutralPanIdStringForTest(1);
        if (const PanadapterModel* pan = radio.panadapter(pan0)) {
            wf0 = pan->waterfallId();
        }
        if (const PanadapterModel* pan = radio.panadapter(pan1)) {
            wf1 = pan->waterfallId();
        }
        radio.setActivePanId(pan0);
    }
    ~Fixture() { TxOperationIntegrationTestAccess::releaseCommandPlane(radio); }

    bool hasTwoPanes() const { return !wf0.isEmpty() && !wf1.isEmpty() && wf0 != wf1; }
};

QString gainText(const QString& wf, int v)
{
    return QStringLiteral("display panafall set %1 color_gain=%2").arg(wf).arg(v);
}
QString blackText(const QString& wf, int v)
{
    return QStringLiteral("display panafall set %1 black_level=%2").arg(wf).arg(v);
}

// ── The renderer's law, asked with the row unit the capabilities declare ─────

// A waterfall row computed on this host is dB, negative; the span below covers
// every floor and signal such a row carries.
constexpr float kRowLowDb = -160.0f;
constexpr float kRowHighDb = -20.0f;

// WtrFall Gain moves the drawn level of a sample above the black point.
bool clientDrawsColourGain(const RadioCapabilities& caps)
{
    WaterfallLevelMap::Params params;
    params.autoBlack = true;
    params.autoBlackThresh = -130.0f;
    params.rowsAreAbsoluteDb = caps.panBinsAbsolute();
    params.colorGain = 20;
    const float low = WaterfallLevelMap::level(-110.0f, params);
    params.colorGain = 80;
    const float high = WaterfallLevelMap::level(-110.0f, params);
    return low != high;
}

// Manual Black Level: some slider position lights a dB row and some position
// blacks it. False when every position draws the whole row black.
bool clientDrawsManualBlackLevel(const RadioCapabilities& caps)
{
    WaterfallLevelMap::Params params;
    params.autoBlack = false;
    params.rowsAreAbsoluteDb = caps.panBinsAbsolute();
    float lowest = 1.0f;
    float highest = 0.0f;
    for (int slider = 0; slider <= 100; ++slider) {
        params.blackLevel = slider;
        for (float db = kRowLowDb; db <= kRowHighDb; db += 5.0f) {
            const float level = WaterfallLevelMap::level(db, params);
            lowest = std::min(lowest, level);
            highest = std::max(highest, level);
        }
    }
    return lowest == 0.0f && highest > 0.0f;
}

// ── 1a. No command plane, absolute-dB rows: the Hermes-Lite 2's shape ────────

void absoluteRowsWithoutCommandPlane()
{
    const RadioCapabilities caps = hl2Capabilities();
    Fixture f(caps);
    check(!f.radio.hasCommandPlane(), "hl2: premise, no command plane");
    check(caps.panBinsAbsolute(), "hl2: premise, the backend declares absolute-dB rows");
    check(f.hasTwoPanes(), "hl2: premise, two panes with distinct waterfall ids");

    // The alarm is alive in this fixture: the same text, sent raw, is reported.
    f.radio.sendCommand(gainText(f.wf0, 60));
    check(f.dropped == QStringList{gainText(f.wf0, 60)},
          "hl2: control, raw gain text is still reported as dropped");
    f.dropped.clear();

    check(clientDrawsColourGain(caps), "hl2 WtrFall Gain: the client draws it");
    f.radio.setWaterfallColorGainFor(f.pan0, 60);
    check(f.dropped.isEmpty(), "hl2 WtrFall Gain: no commandDropped");

    check(clientDrawsManualBlackLevel(caps), "hl2 Black Level: the client draws it");
    f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
    check(f.dropped.isEmpty(), "hl2 Black Level: no commandDropped");

    f.radio.setWaterfallColorGainFor(f.pan1, 70);
    f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
    check(f.dropped.isEmpty(), "hl2 Clone to all Pans: no commandDropped for the target pan");

    f.radio.setWaterfallColorGainFor(f.pan0, 50);
    f.radio.setWaterfallBlackLevelFor(f.pan0, 15);
    check(f.dropped.isEmpty(), "hl2 Reset display defaults: no commandDropped");
}

// ── 1b. No command plane, rows the manual black point cannot reach: Icom ─────

void otherRowsWithoutCommandPlane()
{
    const RadioCapabilities caps = icomCapabilities();
    Fixture f(caps);
    check(!f.radio.hasCommandPlane(), "icom: premise, no command plane");
    check(!caps.panBinsAbsolute(), "icom: premise, the backend declares no absolute-dB rows");
    check(f.hasTwoPanes(), "icom: premise, two panes with distinct waterfall ids");

    check(clientDrawsColourGain(caps), "icom WtrFall Gain: the client draws it");
    f.radio.setWaterfallColorGainFor(f.pan0, 60);
    check(f.dropped.isEmpty(), "icom WtrFall Gain: no commandDropped");

    // The negative: a slider that changes nothing keeps its notice.
    check(!clientDrawsManualBlackLevel(caps),
          "icom Black Level: every slider position draws a dB row black");
    f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
    check(f.dropped == QStringList{blackText(f.wf0, 40)},
          "icom Black Level: a slider that does nothing still raises commandDropped");

    f.dropped.clear();
    f.radio.setWaterfallColorGainFor(f.pan1, 70);
    f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
    check(f.dropped == QStringList{blackText(f.wf1, 30)},
          "icom Clone to all Pans: only the black level is reported");

    f.dropped.clear();
    f.radio.setWaterfallColorGainFor(f.pan0, 50);
    f.radio.setWaterfallBlackLevelFor(f.pan0, 15);
    check(f.dropped == QStringList{blackText(f.wf0, 15)},
          "icom Reset display defaults: only the black level is reported");

    // The connect-time replay has no operator behind it and stays quiet.
    f.dropped.clear();
    f.radio.setWaterfallColorGain(60);
    f.radio.setWaterfallBlackLevel(40);
    check(f.dropped.isEmpty(), "icom: the connect-time replay raises nothing");
}

// ── 1c. A command plane: the wire text, whole ───────────────────────────────

void commandPlaneGetsTheWireText()
{
    RadioConnection connection;   // unopened: no socket, no thread
    QStringList written;
    {
        Fixture f(flexCapabilities());
        check(f.hasTwoPanes(), "flex: premise, two panes with distinct waterfall ids");
        TxOperationIntegrationTestAccess::useCommandPlane(f.radio, connection, written);
        check(f.radio.hasCommandPlane(), "flex: premise, a command plane");

        f.radio.setWaterfallColorGainFor(f.pan0, 60);
        check(written == QStringList{gainText(f.wf0, 60)},
              QStringLiteral("flex WtrFall Gain: wire text (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        written.clear();
        f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
        check(written == QStringList{blackText(f.wf0, 40)},
              QStringLiteral("flex Black Level: wire text (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        // The target pan, not the active one, and gain before black.
        written.clear();
        f.radio.setWaterfallColorGainFor(f.pan1, 70);
        f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
        check(written == QStringList{gainText(f.wf1, 70), blackText(f.wf1, 30)},
              QStringLiteral("flex Clone to all Pans: wire text for the target pan (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        written.clear();
        f.radio.setWaterfallColorGainFor(f.pan0, 50);
        f.radio.setWaterfallBlackLevelFor(f.pan0, 15);
        check(written == QStringList{gainText(f.wf0, 50), blackText(f.wf0, 15)},
              QStringLiteral("flex Reset display defaults: wire text (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        // The connect-time replay addresses the active pan, as it always has.
        written.clear();
        f.radio.setActivePanId(f.pan1);
        f.radio.setWaterfallColorGain(33);
        f.radio.setWaterfallBlackLevel(44);
        check(written == QStringList{gainText(f.wf1, 33), blackText(f.wf1, 44)},
              QStringLiteral("flex connect-time replay: wire text for the active pan (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        written.clear();
        f.radio.setWaterfallColorGainFor(QStringLiteral("0xdeadbeef"), 60);
        f.radio.setWaterfallBlackLevelFor(QString(), 40);
        check(written.isEmpty(), "flex: an unknown or empty pan id writes nothing");
        check(f.dropped.isEmpty(), "flex: nothing is dropped");
    }
}

// The command plane decides, not the row unit: with both, the text is sent.
void commandPlaneOutranksTheRowUnit()
{
    RadioConnection connection;
    QStringList written;
    {
        Fixture f(hl2Capabilities());
        TxOperationIntegrationTestAccess::useCommandPlane(f.radio, connection, written);
        f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
        check(written == QStringList{blackText(f.wf0, 40)} && f.dropped.isEmpty(),
              "absolute-dB rows with a command plane: the black level is still sent");
    }
}

// ── 2. The four GUI sites, by source text ────────────────────────────────────

QString sourceWithoutLineComments(const QString& relative)
{
    QFile file(QStringLiteral(AETHER_SOURCE_DIR) + QLatin1Char('/') + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QStringList kept;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const qsizetype at = line.indexOf(QStringLiteral("//"));
        kept << (at < 0 ? line : line.left(at));
    }
    return kept.join(QLatin1Char('\n'));
}

// The code between two anchors; the first must occur exactly once.
QString region(const QString& code, const QString& from, const QString& to)
{
    const qsizetype start = code.indexOf(from);
    const qsizetype end = start < 0 ? -1 : code.indexOf(to, start + from.size());
    if (start < 0 || end <= start || code.count(from) != 1) {
        return {};
    }
    return code.mid(start, end - start);
}

// In `body`, each widget call comes before each model call, and all are there.
bool widgetThenModel(const QString& body, const QStringList& widgetCalls,
                     const QStringList& modelCalls)
{
    qsizetype lastWidget = -1;
    for (const QString& call : widgetCalls) {
        const qsizetype at = body.indexOf(call);
        if (at < 0) {
            return false;
        }
        lastWidget = std::max(lastWidget, at);
    }
    qsizetype previous = lastWidget;
    for (const QString& call : modelCalls) {
        const qsizetype at = body.indexOf(call);
        if (at <= previous) {
            return false;
        }
        previous = at;
    }
    return true;
}

void guiSitesCallTheModelSetters()
{
    const QString code = sourceWithoutLineComments(QStringLiteral("src/gui/MainWindow_Wiring.cpp"));
    check(!code.isEmpty(), "source: MainWindow_Wiring.cpp is readable");

    check(widgetThenModel(
              region(code, QStringLiteral("&SpectrumOverlayMenu::wfColorGainChanged"),
                     QStringLiteral("&SpectrumOverlayMenu::wfBlackLevelChanged")),
              {QStringLiteral("sw->setWfColorGain(v);")},
              {QStringLiteral("m_radioModel.setWaterfallColorGainFor(applet->panId(), v);")}),
          "source WtrFall Gain: the widget is set, then the model setter is called");

    check(widgetThenModel(
              region(code, QStringLiteral("&SpectrumOverlayMenu::wfBlackLevelChanged"),
                     QStringLiteral("&SpectrumOverlayMenu::wfAutoBlackChanged")),
              {QStringLiteral("sw->setWfBlackLevel(v);")},
              {QStringLiteral("m_radioModel.setWaterfallBlackLevelFor(applet->panId(), v);")}),
          "source Black Level: the widget is set, then the model setter is called");

    check(widgetThenModel(
              region(code, QStringLiteral("dst->setWfColorGain(src->wfColorGain());"),
                     QStringLiteral("dst->setWfLineDuration(")),
              {QStringLiteral("dst->setWfColorGain(src->wfColorGain());"),
               QStringLiteral("dst->setWfBlackLevel(src->wfBlackLevel());")},
              {QStringLiteral(
                   "m_radioModel.setWaterfallColorGainFor(targetPanId, src->wfColorGain());"),
               QStringLiteral(
                   "m_radioModel.setWaterfallBlackLevelFor(targetPanId, src->wfBlackLevel());")}),
          "source Clone to all Pans: the target widget is set, then gain and black "
          "go to the model setters");

    check(widgetThenModel(
              region(code, QStringLiteral("sw->setWfColorGain(50);"),
                     QStringLiteral("auto& s = AppSettings::instance();")),
              {QStringLiteral("sw->setWfColorGain(50);"), QStringLiteral("sw->setWfBlackLevel(15);")},
              {QStringLiteral("m_radioModel.setWaterfallColorGainFor(applet->panId(), 50);"),
               QStringLiteral("m_radioModel.setWaterfallBlackLevelFor(applet->panId(), 15);")}),
          "source Reset display defaults: the widget is set, then gain and black "
          "go to the model setters");

    check(!code.contains(QStringLiteral("color_gain="))
              && !code.contains(QStringLiteral("black_level=")),
          "source: MainWindow_Wiring.cpp builds no gain or black wire text of its own");
}
} // namespace

int main(int argc, char** argv)
{
    // Hl2Backend and FlexBackend read AppSettings while they are constructed.
    TestSettingsProfile profile(QStringLiteral("pan-display-drop-notice"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    absoluteRowsWithoutCommandPlane();
    otherRowsWithoutCommandPlane();
    commandPlaneGetsTheWireText();
    commandPlaneOutranksTheRowUnit();
    guiSitesCallTheModelSetters();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
