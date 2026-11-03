// Socket-free display notice coverage: local rendering is not a dropped radio
// command. Declared backend support, not the presence of an unrelated command
// plane, selects radio writes. GUI ordering is inspected separately as text.

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
    static void encoder(FlexBackend& backend, RadioModel& radio)
    { backend.setSetupCommandSink(radio.setupCommandSink()); }
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
    std::function<ReceiveDispatch(const QString&, const QString&, const PanDisplayRequest&,
        const RadioControlCompletion&, const RadioControlFence&)> display;
    ReceiveDispatch requestPanDisplay(const QString& pan, const QString& waterfall,
        const PanDisplayRequest& request, const RadioControlCompletion& completion = {},
        const RadioControlFence& current = {}) override
    {
        return display ? display(pan, waterfall, request, completion, current)
            : IRadioBackend::requestPanDisplay(pan, waterfall, request, completion, current);
    }
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
    const QString backendPan0{QStringLiteral("0x40000000")};
    const QString backendPan1{QStringLiteral("0x40000001")};

    explicit Fixture(const RadioCapabilities& caps)
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        backend->caps = caps;
        radio.setBackendForTest(std::move(owned), caps.family);
        QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                         [this](const QString& cmd) { dropped << cmd; });
        emit backend->panCenterBandwidthChanged(backendPan0, 7.100, 0.384);
        emit backend->panCenterBandwidthChanged(backendPan1, 14.100, 0.384);
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

// WtrFall Gain moves the drawn level of some sample of a dB row, with auto
// black (the client's estimate, on the row's own axis) or the default manual
// black level.
bool clientDrawsColourGain(const RadioCapabilities& caps, bool autoBlack)
{
    WaterfallLevelMap::Params params;
    params.autoBlack = autoBlack;
    params.autoBlackThresh = -130.0f;
    params.rowsAreAbsoluteDb = caps.panBinsAbsolute();
    for (float db = kRowLowDb; db <= kRowHighDb; db += 5.0f) {
        params.colorGain = 20;
        const float low = WaterfallLevelMap::level(db, params);
        params.colorGain = 80;
        if (low != WaterfallLevelMap::level(db, params)) {
            return true;
        }
    }
    return false;
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

    check(clientDrawsColourGain(caps, true), "hl2 WtrFall Gain, auto black: the client draws it");
    f.radio.setWaterfallColorGainFor(f.pan0, 60, true);
    check(f.dropped.isEmpty(), "hl2 WtrFall Gain, auto black: no commandDropped");

    check(clientDrawsColourGain(caps, false),
          "hl2 WtrFall Gain, manual black: the client draws it");
    f.radio.setWaterfallColorGainFor(f.pan0, 61, false);
    check(f.dropped.isEmpty(), "hl2 WtrFall Gain, manual black: no commandDropped");

    check(clientDrawsManualBlackLevel(caps), "hl2 Black Level: the client draws it");
    f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
    check(f.dropped.isEmpty(), "hl2 Black Level: no commandDropped");

    f.radio.setWaterfallColorGainFor(f.pan1, 70, false);
    f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
    check(f.dropped.isEmpty(), "hl2 Clone to all Pans: no commandDropped for the target pan");

    f.radio.setWaterfallColorGainFor(f.pan0, 50, true);
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

    check(clientDrawsColourGain(caps, true), "icom WtrFall Gain, auto black: the client draws it");
    f.radio.setWaterfallColorGainFor(f.pan0, 60, true);
    check(f.dropped.isEmpty(), "icom WtrFall Gain, auto black: no commandDropped");

    // With manual black every row is black, so the gain does nothing either.
    check(!clientDrawsColourGain(caps, false),
          "icom WtrFall Gain, manual black: no gain changes a dB row");
    f.radio.setWaterfallColorGainFor(f.pan0, 61, false);
    check(f.dropped == QStringList{QStringLiteral("pan display")},
          "icom WtrFall Gain, manual black: still raises commandDropped");
    f.dropped.clear();

    // The negative: a slider that changes nothing keeps its notice.
    check(!clientDrawsManualBlackLevel(caps),
          "icom Black Level: every slider position draws a dB row black");
    f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
    check(f.dropped == QStringList{QStringLiteral("pan display")},
          "icom Black Level: a slider that does nothing still raises commandDropped");

    f.dropped.clear();
    f.radio.setWaterfallColorGainFor(f.pan1, 70, true);
    f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
    check(f.dropped == QStringList{QStringLiteral("pan display")},
          "icom Clone to all Pans, auto black: only the black level is reported");

    f.dropped.clear();
    f.radio.setWaterfallColorGainFor(f.pan1, 71, false);
    f.radio.setWaterfallBlackLevelFor(f.pan1, 31);
    check(f.dropped == QStringList{QStringLiteral("pan display"), QStringLiteral("pan display")},
          "icom Clone to all Pans, manual black: gain and black are reported");

    f.dropped.clear();
    f.radio.setWaterfallColorGainFor(f.pan0, 50, true);
    f.radio.setWaterfallBlackLevelFor(f.pan0, 15);
    check(f.dropped == QStringList{QStringLiteral("pan display")},
          "icom Reset display defaults: only the black level is reported");

    // The connect-time replay has no operator behind it and stays quiet.
    f.dropped.clear();
    f.radio.setWaterfallColorGain(60);
    f.radio.setWaterfallBlackLevel(40);
    check(f.dropped.isEmpty(), "icom: the connect-time replay raises nothing");
}

void storedDisplayReplayIsSilent()
{
    for (const RadioCapabilities& caps : {icomCapabilities(), hl2Capabilities()}) {
        Fixture f(caps);
        f.radio.setPanWnb(true);
        check(f.dropped.isEmpty(), caps.family + ": stored WNB replay is silent");
        f.dropped.clear();
        f.radio.setPanWnbLevel(73);
        check(f.dropped.isEmpty(), caps.family + ": stored WNB level replay is silent");
        f.dropped.clear();

        PanadapterModel* pan = f.radio.activePanadapter();
        check(f.radio.requestPanDisplay(pan, {PanDisplayField::LevelRange,
                  PanLevelRange{-130.0, -30.0}}, {}, false) == ReceiveDispatch::Unsupported,
              caps.family + ": silent level-range replay still refuses unsupported control");
        check(f.dropped.isEmpty(), caps.family + ": stored level-range replay is silent");

        for (const PanDisplayRequest& request : {
                 PanDisplayRequest{PanDisplayField::Wnb, true},
                 PanDisplayRequest{PanDisplayField::WnbLevel, 73}}) {
            check(f.radio.requestPanDisplay(pan, request, {}, false) == ReceiveDispatch::Unsupported,
                  caps.family + ": band-stack replay still refuses unsupported WNB");
        }
        check(f.dropped.isEmpty(), caps.family + ": band-stack WNB replay is silent");
        f.radio.requestPanDisplay(pan, {PanDisplayField::Wnb, true});
        f.radio.requestPanDisplay(pan, {PanDisplayField::WnbLevel, 73});
        f.radio.requestPanDisplay(pan, {PanDisplayField::LevelRange,
            PanLevelRange{-130.0, -30.0}});
        check(f.dropped.size() == 3, caps.family + ": explicit operator requests still announce");
    }

    // Geometry-only display support, as declared by the demo backend. Do not
    // construct SimBackend here: its transport threads initialize sockets.
    RadioCapabilities geometryOnly = icomCapabilities();
    geometryOnly.panDisplayControl = PanDisplayControl{
        {PanDisplayField::Geometry}, {PanGeometryComponent::Center}};
    Fixture f(geometryOnly);
    check(f.radio.requestPanDisplay(f.radio.activePanadapter(),
              {PanDisplayField::RfGain, 12}, {}, false) == ReceiveDispatch::Unsupported,
          "geometry-only: RF-gain replay still refuses unsupported control");
    check(f.dropped.isEmpty(), "geometry-only: stored RF-gain replay is silent");
    f.radio.setPanRfGain(12);
    check(f.dropped.size() == 1, "geometry-only: explicit RF-gain setter still announces");
}

// ── 1c. A command plane: the wire text, whole ───────────────────────────────

void commandPlaneGetsTheWireText()
{
    FlexBackend encoder;
    RadioConnection connection;   // unopened: no socket, no thread
    QStringList written;
    {
        Fixture f(flexCapabilities());
        TxOperationIntegrationTestAccess::encoder(encoder, f.radio);
        f.backend->display = [&encoder](const QString& pan, const QString& waterfall,
            const PanDisplayRequest& request, const RadioControlCompletion& completion, const RadioControlFence& current) {
            return encoder.requestPanDisplay(pan, waterfall, request, completion, current);
        };
        check(f.hasTwoPanes(), "flex: premise, two panes with distinct waterfall ids");
        TxOperationIntegrationTestAccess::useCommandPlane(f.radio, connection, written);
        check(f.radio.hasCommandPlane(), "flex: premise, a command plane");

        f.radio.setWaterfallColorGainFor(f.pan0, 60, false);
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
        f.radio.setWaterfallColorGainFor(f.pan1, 70, true);
        f.radio.setWaterfallBlackLevelFor(f.pan1, 30);
        check(written == QStringList{gainText(f.wf1, 70), blackText(f.wf1, 30)},
              QStringLiteral("flex Clone to all Pans: wire text for the target pan (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        written.clear();
        f.radio.setWaterfallColorGainFor(f.pan0, 50, true);
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
        f.radio.setPanWnb(true);
        f.radio.setPanWnbLevel(73);
        f.radio.requestPanDisplay(f.radio.activePanadapter(),
            {PanDisplayField::RfGain, 12}, {}, false);
        check(written == QStringList{
                  QStringLiteral("display pan set %1 wnb=1").arg(f.backendPan1),
                  QStringLiteral("display pan set %1 wnb_level=73").arg(f.backendPan1),
                  QStringLiteral("display pan set %1 rfgain=12").arg(f.backendPan1)},
              QStringLiteral("flex: silent replay still writes WNB and RF gain to the active second pan (got: %1)")
                  .arg(written.join(QStringLiteral(" | "))));

        written.clear();
        f.radio.setWaterfallColorGainFor(QStringLiteral("0xdeadbeef"), 60, false);
        f.radio.setWaterfallBlackLevelFor(QString(), 40);
        check(written.isEmpty(), "flex: an unknown or empty pan id writes nothing");
        check(f.dropped.isEmpty(), "flex: nothing is dropped");
    }
}

// An unrelated command plane cannot manufacture a missing backend verb.
void commandPlaneOutranksTheRowUnit()
{
    RadioConnection connection;
    QStringList written;
    {
        Fixture f(hl2Capabilities());
        TxOperationIntegrationTestAccess::useCommandPlane(f.radio, connection, written);
        f.radio.setWaterfallBlackLevelFor(f.pan0, 40);
        check(written.isEmpty() && f.dropped.isEmpty(),
              "absolute-dB rows remain local when the declared backend has no waterfall verb");
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
              {QStringLiteral(
                  "m_radioModel.setWaterfallColorGainFor(applet->panId(), v, sw->wfAutoBlack());")}),
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
                   "m_radioModel.setWaterfallColorGainFor(targetPanId, src->wfColorGain(),"),
               QStringLiteral("dst->wfAutoBlack());"),
               QStringLiteral(
                   "m_radioModel.setWaterfallBlackLevelFor(targetPanId, src->wfBlackLevel());")}),
          "source Clone to all Pans: the target widget is set, then gain and black "
          "go to the model setters");

    check(widgetThenModel(
              region(code, QStringLiteral("sw->setWfColorGain(50);"),
                     QStringLiteral("auto& s = AppSettings::instance();")),
              {QStringLiteral("sw->setWfColorGain(50);"), QStringLiteral("sw->setWfBlackLevel(15);"),
               QStringLiteral("sw->setWfAutoBlack(true);")},
              {QStringLiteral(
                   "m_radioModel.setWaterfallColorGainFor(applet->panId(), 50, sw->wfAutoBlack());"),
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
    storedDisplayReplayIsSilent();
    commandPlaneGetsTheWireText();
    commandPlaneOutranksTheRowUnit();
    guiSitesCallTheModelSetters();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
