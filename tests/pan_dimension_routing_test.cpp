// Pan pixel-dimension routing — the channels out of
// MainWindow::sendPanDimensionsToRadio(), pinned socket-free.
//
// #5920 gave the ANAN panadapter one analyzer point per screen pixel by adding
// a typed seam verb (IRadioBackend::setPanPixelWidth, routed by
// RadioModel::requestLocalPanPixelWidth) beside the Flex `xpixels=` wire text
// that call site had always sent. Its review (@jensenpat, non-blocking) asked
// for exactly this: "a socket-free routing test could pin Flex wire delivery,
// ANAN typed dispatch, and Icom's no-command path at the shared GUI/model
// seam."
//
// The same review also had to CORRECT the PR's own account of the Icom path:
// `shapesDisplayRatesLocally()` covers every non-Flex backend, so Icom ACCEPTS
// the typed call and eats it in the default no-op. It does not decline the seam
// and get stopped by the `hasCommandPlane()` guard, which is what the PR
// discussion said twice. Nothing in the build noticed the wrong story; that is
// the gap this file closes.
//
// WHY THIS FILE HAS TWO HALVES, AND WHY THE COMPOSITION IS A PROOF.
//
// The decision lives in a MainWindow method, and no test target in this repo
// constructs a MainWindow. It cannot move down into RadioModel either: the
// fallback is raw Flex wire text, and tools/check_command_plane.py is a
// PER-FILE, SHRINK-ONLY ratchet with src/models/RadioModel.cpp frozen at its
// baseline count — relocating one `sendCommand()` there fails the ratchet. That
// is what RadioModel::requestLocalPanWeightedAverage's own comment means by
// "moving it in here would add a raw command above the seam". So the call site
// stays where it is and this file pins it from both sides:
//
//   PART A (behaviour; real backends built through the production family
//   switch; no socket, no device, no radio) — what the two predicates in that
//   condition actually answer, per family. This is the half that would have
//   caught the Icom mis-description.
//
//   PART B (the call site, read as TEXT) — that the condition is exactly those
//   two predicates, in that order, with those arguments, guarding exactly that
//   wire spelling.
//
// Neither half alone is worth much: A without B describes predicates nobody
// promises the call site consults, and B without A is a spelling checker. What
// makes the pair a proof is the ORDER claim, which is load-bearing and not
// obvious from either side alone:
//
//   ANAN has NO command plane (Part A measures this). A reviewer tidying the
//   condition into `hasCommandPlane() && !requestLocalPanPixelWidth(...)` —
//   which reads identically and satisfies every stated requirement of #4448 —
//   short-circuits before the seam call, and the ANAN width silently stops
//   reaching the analyzer. The panadapter goes soft again with nothing red.
//   Part A supplies "ANAN has no plane", Part B supplies "the seam is
//   evaluated first", and together they close that.
//
// Part B is a brace matcher and a regular expression, not a compiler, so the
// analyser is a pure function run first against SYNTHETIC sources. Those runs
// are the POSITIVE CONTROLS: they prove it can reject each mutation rather than
// reporting health because it never matches anything. Same spirit, and the same
// stated limitation, as wdsp_nb_hold_invariant_test and meter_surfaces_test.
//
// MUTATION RUNS BEHIND PART A. Part B's controls are in this file and run on
// every invocation; Part A's assertions were earned against real production
// source, each one edited on its own and rebuilt:
//
//   * the call site's two halves swapped -> the seam-first check (this is the
//     bug the two halves exist to catch, and it needs no rebuild at all: the
//     text check reads MainWindow_Wiring.cpp at run time);
//   * requestLocalPanPixelWidth narrowed to `m_family == "anan"`, which is the
//     story the #5920 discussion told twice -> the HL2 and Icom rows;
//   * hasCommandPlane() forced true -> the three no-plane rows;
//   * shapesDisplayRatesLocally() widened to include Flex -> the Flex row;
//   * backendPanIdFor() dropped from the seam call -> the pan-id row;
//   * the isEmpty() guard dropped -> the empty-pan-id row;
//   * the seam accepting the width and forwarding nothing -> the point-count
//     and pan-id rows.
//
// Three checks are CONTROLS rather than claims and are labelled as such below:
// that every family built, that the fixture's two pan ids actually differ, and
// that the source file opened. None of them can fail for an interesting reason;
// they exist so that a vacuous run cannot read as a healthy one.
//
// WHAT IS NOT GUARDED HERE, left bare rather than half-guarded: the local
// decoder rescale and the DSS scale-settle gate that surround the block (their
// own tests own those), whether a given point count LOOKS right on a G2 (a
// measurement, not a predicate), and the values panXpixelsFor() and
// panLocalSpectrumPointsFor() compute from a widget — spectrum_preview_logic_test
// owns the crop arithmetic. This file pins WHICH channel each family uses and
// WHICH of those two functions feeds each channel.

#include "TestSettingsProfile.h"

#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const QString& what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    g_failures += ok ? 0 : 1;
}

int occurrences(const QString& haystack, const QString& needle)
{
    int found = 0;
    for (int at = haystack.indexOf(needle); at >= 0;
         at = haystack.indexOf(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

// ─── Part A ──────────────────────────────────────────────────────────────────

// Records the typed seam verb and nothing else. Installed under a non-Flex
// family, which is all `shapesDisplayRatesLocally()` reads, so it stands in for
// "a backend that computes its own spectrum" without pulling a real one's
// worker threads into a value assertion.
class WidthRecordingBackend final : public IRadioBackend {
public:
    QStringList panIds;
    QList<int> points;

    void setPanPixelWidth(const QString& panId, int pixels) override
    {
        panIds << panId;
        points << pixels;
    }

    RadioCapabilities capabilities() const override { return m_caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64,
                         const QVariant&) override {}

private:
    RadioCapabilities m_caps;
};

struct Routing {
    bool built = false;
    bool seamAccepted = false;   // requestLocalPanPixelWidth() took the width
    bool commandPlane = false;   // hasCommandPlane(): the text has somewhere to go
};

// Builds the REAL backend for `family` through the production family switch
// (rebuildBackendForTest calls the same rebuildBackendForFamily() that
// connectToRadio calls, minus the dial), then reads the two predicates the call
// site consults. No socket is opened and no radio is addressed.
Routing routingFor(const QString& family)
{
    Routing out;
    RadioModel radio;
    out.built = radio.rebuildBackendForTest(family);
    if (!out.built) {
        return out;
    }
    out.seamAccepted =
        radio.requestLocalPanPixelWidth(RadioModel::neutralPanIdStringForTest(0), 1400);
    out.commandPlane = radio.hasCommandPlane();
    QCoreApplication::processEvents();
    return out;
}

// ─── Part B ──────────────────────────────────────────────────────────────────

// One scanner that brace-matches a function body while knowing about string
// literals, char literals and both comment forms, so a `{` or `}` inside any of
// them cannot end the body early and the block's own explanatory comment is not
// read as code. Whitespace is then collapsed, which makes the checks below
// indifferent to how the condition is wrapped across lines. (It collapses runs
// inside string literals too; the one literal that matters here has single
// spaces, so that is inert.)
QString functionBody(const QByteArray& source, const char* signature)
{
    const int sig = source.indexOf(signature);
    if (sig < 0) {
        return {};
    }
    int i = source.indexOf('{', sig);
    if (i < 0) {
        return {};
    }

    enum State { Code, LineComment, BlockComment, StringLit, CharLit };
    State state = Code;
    int depth = 0;
    QString text;
    for (; i < source.size(); ++i) {
        const char c = source.at(i);
        const char next = (i + 1 < source.size()) ? source.at(i + 1) : '\0';
        switch (state) {
        case Code:
            if (c == '/' && next == '/') { state = LineComment; continue; }
            if (c == '/' && next == '*') { state = BlockComment; ++i; continue; }
            if (c == '"') { state = StringLit; }
            else if (c == '\'') { state = CharLit; }
            else if (c == '{') { ++depth; }
            else if (c == '}') { --depth; }
            break;
        case LineComment:
            if (c == '\n') { state = Code; text.append(QLatin1Char(' ')); }
            continue;
        case BlockComment:
            if (c == '*' && next == '/') { state = Code; ++i; text.append(QLatin1Char(' ')); }
            continue;
        case StringLit:
        case CharLit:
            if (c == '\\') {
                text.append(QLatin1Char(c));
                text.append(QLatin1Char(next));
                ++i;
                continue;
            }
            if ((state == StringLit && c == '"') || (state == CharLit && c == '\'')) {
                state = Code;
            }
            break;
        }
        text.append(QLatin1Char(c));
        if (state == Code && depth == 0 && c == '}') {
            break;
        }
    }
    return depth == 0 ? text.simplified() : QString();
}

QRegularExpression pattern(const char* source)
{
    return QRegularExpression(QString::fromLatin1(source));
}

struct CallSite {
    bool parsed = false;
    bool seamFirst = false;
    bool wireSpelling = false;
    bool xpixFromDeviceWidth = false;
    bool ypixFromDeviceHeight = false;
    int seamCalls = 0;
    int planeChecks = 0;
    int wireSends = 0;

    bool healthy() const
    {
        return parsed && seamFirst && wireSpelling && xpixFromDeviceWidth
            && ypixFromDeviceHeight && seamCalls == 1 && planeChecks == 1
            && wireSends == 1;
    }
};

CallSite analyse(const QByteArray& source)
{
    // THE condition, as one expression: the seam call is negated and comes
    // FIRST, it is handed the crop-widened local point count rather than the
    // raw device width, the two halves are joined by && (not ||), and the wire
    // send is the guarded block's first statement.
    static const QRegularExpression kSeamFirstGuard = pattern(
        R"(if ?\( ?! ?m_radioModel\.requestLocalPanPixelWidth\( ?panId, ?)"
        R"(panLocalSpectrumPointsFor\( ?sw ?\) ?\) ?)"
        R"(&& ?m_radioModel\.hasCommandPlane\( ?\) ?\) ?\{ ?)"
        R"(m_radioModel\.sendCommand\()");
    // The Flex contract, spelling and argument ORDER included: transposing xpix
    // and ypix here compiles, ships, and tells the radio a rotated frame.
    static const QRegularExpression kWireText = pattern(
        R"(QString\( ?"display pan set %1 xpixels=%2 ypixels=%3" ?\) ?)"
        R"(\.arg\( ?panId ?\) ?\.arg\( ?xpix ?\) ?\.arg\( ?ypix ?\))");
    static const QRegularExpression kXpix =
        pattern(R"(const int xpix ?= ?panXpixelsFor\( ?sw ?\) ?;)");
    static const QRegularExpression kYpix =
        pattern(R"(const int ypix ?= ?panYpixelsFor\( ?sw ?\) ?;)");

    CallSite out;
    const QString body =
        functionBody(source, "void MainWindow::sendPanDimensionsToRadio(");
    if (body.isEmpty()) {
        return out;
    }
    out.parsed = true;
    out.seamFirst = body.contains(kSeamFirstGuard);
    out.wireSpelling = body.contains(kWireText);
    out.xpixFromDeviceWidth = body.contains(kXpix);
    out.ypixFromDeviceHeight = body.contains(kYpix);
    out.seamCalls = occurrences(body, QStringLiteral("requestLocalPanPixelWidth("));
    out.planeChecks = occurrences(body, QStringLiteral("hasCommandPlane("));
    out.wireSends = occurrences(body, QStringLiteral("xpixels="));
    return out;
}

// The real shape, reduced to what the analyser reads, and carrying the two
// traps the scanner has to survive: an unbalanced brace inside a comment and
// another inside a string literal. Every control below is a one-line edit of
// THIS, so a control that fails for an unintended reason fails the "faithful
// copy" check first.
constexpr const char* kRealShape = R"CPP(
void MainWindow::sendPanDimensionsToRadio(const QString& panId,
                                          SpectrumWidget* sw,
                                          bool updateLocalDecoderImmediately)
{
    // Sending xpixels/ypixels during a profile load is what this defers. }
    if (panId.isEmpty() || !sw || !panPixelDimensionsReady(sw)) {
        return;
    }
    const char* braceInAStringLiteral = "} {";
    const int xpix = panXpixelsFor(sw);
    const int ypix = panYpixelsFor(sw);
    if (!m_radioModel.requestLocalPanPixelWidth(panId, panLocalSpectrumPointsFor(sw))
        && m_radioModel.hasCommandPlane()) {
        m_radioModel.sendCommand(
            QString("display pan set %1 xpixels=%2 ypixels=%3")
                .arg(panId).arg(xpix).arg(ypix));
    }
    if (pan->fftYPixels() > 0 && pan->fftYPixels() != ypix) {
        sw->beginFftPixelScaleSettle();
    }
}
)CPP";

// Applies one control edit. An edit that does not apply returns empty, which
// the caller reports as a failure rather than passing on a source it never
// mutated.
QByteArray mutated(const char* from, const char* to)
{
    QByteArray out(kRealShape);
    if (!out.contains(QByteArrayView(from))) {
        return {};
    }
    return out.replace(QByteArrayView(from), QByteArrayView(to));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("pan-dimension-routing-test"));
    QCoreApplication app(argc, argv);

    std::printf("── Part A: which channel each family uses\n");

    const Routing flex = routingFor(QStringLiteral("flex"));
    const Routing anan = routingFor(QStringLiteral("anan"));
    const Routing hl2 = routingFor(QStringLiteral("hl2"));
    const Routing icom = routingFor(QStringLiteral("icom"));
    const Routing sim = routingFor(QStringLiteral("sim"));

    check(flex.built && anan.built && hl2.built && icom.built && sim.built,
          "control: every family in this table builds through the production "
          "switch");

    check(!flex.seamAccepted && flex.commandPlane,
          "Flex declines the typed width and has a command plane: the "
          "xpixels=/ypixels= text is its only channel");
    check(anan.seamAccepted,
          "ANAN takes the width through the typed seam");
    check(!anan.commandPlane,
          "ANAN has NO command plane, so the seam call must be evaluated FIRST "
          "or its width is short-circuited away");
    check(hl2.seamAccepted && !hl2.commandPlane,
          "HL2 takes the same typed call and ignores the value, with no wire "
          "fallback behind it");
    check(icom.seamAccepted,
          "Icom ACCEPTS the typed call and eats it in the default no-op: it is "
          "not the command-plane guard that stops its wire text (#5920 review "
          "correction)");
    check(!icom.commandPlane,
          "Icom has no command plane either, so the #4448 unsupported-command "
          "warning cannot return through the fallback");
    check(sim.seamAccepted && sim.commandPlane,
          "the demo is the one family with BOTH channels open, and the seam "
          "coming first is what keeps its synthetic wire out of this write");

    {
        RadioModel radio;
        auto owned = std::make_unique<WidthRecordingBackend>();
        auto* recorder = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));

        // Allocate a real backend->neutral pan mapping, so the id the backend
        // receives is NOT the model key and the translation is observable. A
        // width addressed with the model's "0xe1000000" is a width the backend
        // cannot resolve to a receiver.
        const QString backendPanId = QStringLiteral("test-2");
        const int panIndex = radio.panIndexForBackendIdForTest(backendPanId);
        const QString modelPanId = RadioModel::neutralPanIdStringForTest(panIndex);
        check(modelPanId != backendPanId,
              "control: the model key and the backend pan id are different "
              "strings, so the next check is not comparing a value to itself");

        const bool accepted = radio.requestLocalPanPixelWidth(modelPanId, 2083);
        check(accepted && recorder->points.size() == 1
                  && recorder->points.value(0) == 2083,
              "the typed call carries the point count to the backend unchanged");
        check(recorder->panIds.value(0) == backendPanId,
              "and addresses it with the BACKEND's pan id, not the model key");
        check(!radio.requestLocalPanPixelWidth(QString(), 2083)
                  && recorder->points.size() == 1,
              "an empty pan id is refused instead of reaching the backend");
    }

    std::printf("── Part B: the call site, read as text\n");

    const CallSite reference = analyse(QByteArray(kRealShape));
    check(reference.healthy(),
          "control: the analyser accepts a faithful copy of the real shape");
    check(reference.wireSends == 1 && reference.parsed,
          "control: an unbalanced brace in a comment and in a string literal "
          "neither ends the body early nor adds a phantom send");

    struct Control {
        const char* what;
        const char* from;
        const char* to;
    };
    static const Control kControls[] = {
        {"the command-plane guard dropped",
         "\n        && m_radioModel.hasCommandPlane()", ""},
        {"the two halves swapped, which short-circuits ANAN's width away",
         "if (!m_radioModel.requestLocalPanPixelWidth(panId, panLocalSpectrumPointsFor(sw))\n"
         "        && m_radioModel.hasCommandPlane()) {",
         "if (m_radioModel.hasCommandPlane()\n"
         "        && !m_radioModel.requestLocalPanPixelWidth(panId, panLocalSpectrumPointsFor(sw))) {"},
        {"the seam call no longer negated",
         "if (!m_radioModel.requestLocalPanPixelWidth",
         "if (m_radioModel.requestLocalPanPixelWidth"},
        {"&& weakened to ||",
         "\n        && m_radioModel.hasCommandPlane()",
         "\n        || m_radioModel.hasCommandPlane()"},
        {"the seam handed the raw device width instead of the local point count",
         "requestLocalPanPixelWidth(panId, panLocalSpectrumPointsFor(sw))",
         "requestLocalPanPixelWidth(panId, xpix)"},
        {"xpix and ypix transposed on the wire",
         ".arg(panId).arg(xpix).arg(ypix)", ".arg(panId).arg(ypix).arg(xpix)"},
        {"the wire fields renamed",
         "xpixels=%2 ypixels=%3", "x_pixels=%2 y_pixels=%3"},
        {"the wire fed the crop-widened count instead of the device width",
         "const int xpix = panXpixelsFor(sw);",
         "const int xpix = panLocalSpectrumPointsFor(sw);"},
        {"a second, unguarded send added",
         "    if (pan->fftYPixels() > 0",
         "    m_radioModel.sendCommand(QString(\"display pan set %1 xpixels=%2\")"
         ".arg(panId).arg(xpix));\n    if (pan->fftYPixels() > 0"},
    };
    for (const Control& control : kControls) {
        const QByteArray source = mutated(control.from, control.to);
        const CallSite shape = analyse(source);
        check(!source.isEmpty() && shape.parsed && !shape.healthy(),
              QStringLiteral("control: rejected — %1")
                  .arg(QString::fromLatin1(control.what)));
    }
    check(!analyse(QByteArray("void MainWindow::somethingElse() { }\n")).parsed,
          "control: a source without the function reads as unparsed, not as "
          "healthy");

    QFile file(QString::fromLatin1(AETHER_SOURCE_DIR)
               + QStringLiteral("/src/gui/MainWindow_Wiring.cpp"));
    const bool opened = file.open(QIODevice::ReadOnly);
    check(opened, "control: MainWindow_Wiring.cpp is readable");
    if (!opened) {
        std::printf("FAILURES\n");
        return 1;
    }
    const CallSite real = analyse(file.readAll());

    check(real.parsed, "sendPanDimensionsToRadio() is still recognisable");
    check(real.seamCalls == 1,
          "the typed width is requested exactly once in that function");
    check(real.planeChecks == 1, "and the command plane is consulted once");
    check(real.wireSends == 1, "and there is exactly one xpixels= send");
    check(real.seamFirst,
          "the seam is tried FIRST, negated, && the command-plane guard, with "
          "the send as the guarded block's first statement");
    check(real.wireSpelling,
          "the fallback keeps the Flex spelling and argument order");
    check(real.xpixFromDeviceWidth && real.ypixFromDeviceHeight,
          "the wire keeps the raw device pixel counts while the seam gets the "
          "crop-widened local point count");

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
