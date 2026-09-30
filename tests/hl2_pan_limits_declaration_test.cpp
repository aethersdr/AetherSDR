// The panadapter limits the Hermes-Lite 2 actually has, as CAPABILITIES.
//
// Four properties of this radio were true and undeclared: a 48 kHz span floor,
// four discrete rates, one span shared by every receiver, and an uncalibrated
// dBFS axis. A fifth, radioOwnsDbmScale, was worse than undeclared — it was
// inheriting the permissive default, which asserts a command plane this backend
// does not have.
//
// WHY THIS TEST EXISTS AT ALL. A capability declaration is the shape that rots
// silently. Nothing calls it, nothing crashes when it drifts, and the only
// symptom is a control somewhere that lies. So every assertion here compares
// the declaration against the SAME constant or predicate production reads —
// hl2::kIqSampleRatesHz for the rates, Hl2DbReference::isCalibrated() for the
// axis — rather than against a re-typed copy of its values. A test that carries
// its own copy of the truth cannot detect the declaration and the code
// diverging, which is exactly the failure being guarded against.
//
// SOCKET-FREE. Hl2Backend::capabilities() takes its receiver ceiling from
// m_connected ? receiverCeiling() : the id count, so the whole descriptor is
// available on a default-constructed backend. Nothing is bound, nothing is
// connected, no event loop is pumped, and no radio is required. (The connected
// half of this seam — that panBandwidthLimitsChanged really emits
// kIqSampleRatesHz[0] as its lower bound — lived in the fake-EP6 fixture that
// is now retired; see the commented block in tests/tests.cmake. What is pinned
// here is the DECLARATION and the constant it is built from, which is the half
// that can rot without anyone noticing.)
//
// NOTHING HERE WAS MEASURED ON A RADIO. In particular this file makes no claim
// about the 24 dB/s dBm ratchet RadioCapabilities.h describes: that is a runtime
// question, and radioOwnsDbmScale is asserted on the SOURCE fact it is actually
// about — whether there is an echo to wait for.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2DbReference.h"
#include "gui/PanSpanControlGate.h"
#include "gui/PanZoomModeGate.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}

// Read a source file of the tree this test was built from.
QByteArray readSource(const char* relative)
{
    QFile f(QStringLiteral(AETHER_SOURCE_DIR "/") + QString::fromLatin1(relative));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

// Visit `src` from `from`, calling `visit(index, depth)` for every character
// outside comments and string/character literals, where depth counts the
// braces opened since `from`. Stops when `visit` returns false.
template <typename Visit>
void scanCode(const QByteArray& src, qsizetype from, Visit visit)
{
    int depth = 0;
    for (qsizetype i = from; i < src.size(); ++i) {
        const char c = src.at(i);
        if (c == '/' && i + 1 < src.size() && src.at(i + 1) == '/') {
            const qsizetype nl = src.indexOf('\n', i);
            i = nl < 0 ? src.size() : nl;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src.at(i + 1) == '*') {
            const qsizetype close = src.indexOf("*/", i + 2);
            i = close < 0 ? src.size() : close + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < src.size() && src.at(i) != c; ++i) {
                if (src.at(i) == '\\') {
                    ++i;
                }
            }
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        }
        if (!visit(i, depth)) {
            return;
        }
    }
}

// The body of the first function whose definition starts with `signature`,
// from its opening brace to the matching closing brace, or empty.
QByteArray functionBody(const QByteArray& src, const QByteArray& signature)
{
    const qsizetype start = src.indexOf(signature);
    if (start < 0) {
        return {};
    }
    const qsizetype open = src.indexOf('{', start + signature.size());
    if (open < 0) {
        return {};
    }
    qsizetype close = -1;
    scanCode(src, open, [&](qsizetype i, int depth) {
        if (depth == 0) {
            close = i;
            return false;
        }
        return true;
    });
    return close < 0 ? QByteArray() : src.mid(open, close - open + 1);
}

// Brace depth, relative to `body`'s own opening brace, of every occurrence of
// `token` in its code (comments and literals skipped, so a brace or a mention
// in a comment cannot count). Depth 1 is the function body itself; anything
// deeper is inside a block or a lambda.
QList<int> depthsOf(const QByteArray& body, const QByteArray& token)
{
    QList<int> depths;
    scanCode(body, 0, [&](qsizetype i, int depth) {
        if (body.mid(i, token.size()) == token) {
            depths.append(depth);
        }
        return true;
    });
    return depths;
}
}  // namespace

int main(int argc, char** argv)
{
    // The backend touches AppSettings on construction (the owned "Hl2" span
    // object). Redirect it before QCoreApplication so no assertion here can
    // read or write the operator's live configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-pan-limits-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();

    check(caps.family == QLatin1String("hl2"),
          "this is the HL2 descriptor (sanity, not the subject)");

    // ---- the dBm scale is not the radio's, because it has no command plane ----
    //
    // `display pan set … min_dbm=…` is Flex wire text. RadioModel::sendCmd drops
    // it at hasCommandPlane(), which is m_wanConn || m_connection, and
    // m_connection is assigned only inside the dynamic_cast<FlexBackend*> branch
    // of RadioModel::setupBackend. FlexBackend::decodePanRange is the only
    // reader of min_dbm anywhere in the tree.
    //
    // THE HL2'S radioOwnsDbmScale IS NOT ASSERTED HERE. Its DEFAULT is, below,
    // which is a different fact — the one that would change every silent
    // backend's claim at once if it moved. It is wrong for this radio and it
    // is deliberately left undeclared: bench run d101 measured the auto-floor
    // loop SETTLING on this radio (0.307 dB in 74 s quiescent, 0.0000 dB/s over
    // the second half, re-settling within ~30 s after a 12 dB LNA step), and the
    // early return in SpectrumWidget::applyNoiseFloorAutoAdjust keys on the same
    // flag -- so declaring it would remove a loop that works. See the note in
    // Hl2Backend::capabilities(). Asserting it here would pin a decision this
    // change deliberately does not take.

    // ---- the span floor and the four discrete rates ----
    //
    // kIqSampleRatesHz is THE list: capability advertisement, zoom clamp and
    // snap target are one array in Hl2Backend.h. The assertion is that the
    // capability still IS that array, not that it happens to contain 48000.
    {
        QVector<int> expected;
        for (const int rate : hl2::kIqSampleRatesHz)
            expected.append(rate);
        check(caps.sampleRatesHz == expected,
              "sampleRatesHz is exactly kIqSampleRatesHz, the list production snaps to");
        check(!caps.sampleRatesHz.isEmpty()
                  && caps.sampleRatesHz.first() == hl2::kIqSampleRatesHz[0],
              "the narrowest rate is the span FLOOR, and it is the same constant");
        check(std::is_sorted(caps.sampleRatesHz.cbegin(), caps.sampleRatesHz.cend()),
              "ascending, so first() is genuinely the floor and last() the ceiling");
        // The one DELIBERATE literal in this file. Everything else compares
        // production against production; this pins the COUNT, because "four
        // discrete rates" is a claim made OUTSIDE the code — in the capability
        // map, and in what gets said upstream — and a fifth rate appearing would
        // make that claim stale while every other assertion here still passed.
        check(caps.sampleRatesHz.size() == 4,
              "four DISCRETE rates — there is nothing between them to select");
    }

    // Why that floor is a floor and not a default: the span IS the sample rate,
    // so a narrower window would need samples the DDC never delivered. Revert
    // this and a client is entitled to take a 5 kHz zoom request literally
    // instead of snapping it to a rate.
    //
    // The record is PRESENT, which is a claim in its own right: absent means
    // "no backend has been read on the question", and an assertion that only
    // read the fields would pass on a default-constructed nullopt turning into
    // a silent false. Check presence first, then each field.
    check(caps.panSpanModel.has_value(),
          "the HL2 DECLARES a span model — absence would mean nobody had read it");
    check(caps.panSpanModel && caps.panSpanModel->followsSampleRate,
          "the pan span IS the sample rate, so the rate list is the complete span set");

    // One rate field for the whole board — MetisProtocol::ccConfig packs
    // SampleRate into C1[1:0], with the receiver COUNT in a separate field —
    // so a span change is radio-wide. Revert this and a per-pan span control
    // looks legitimate on a radio where narrowing one window silently retunes
    // the other three.
    check(caps.panSpanModel && caps.panSpanModel->radioWide,
          "one DDC rate for the whole radio — span is shared, not per-panadapter");

    // ---- the Y axis is dBFS wearing a dBm label ----
    //
    // THIS ASSERTION PINS TODAY'S ANSWER AND WILL NEED REVISITING. An earlier
    // version of this comment claimed the opposite — that asserting against
    // Hl2DbReference's own predicate rather than a hardcoded false means "the
    // day a per-unit fullScaleDbm is populated the declaration follows it and
    // this assertion keeps holding". It does not, and aethersdr-agent showed
    // why on #5726: production reads the BACKEND's m_dbRef, while the
    // right-hand side here is a default-constructed Hl2DbReference{} whose
    // m_fullScaleDbm is 0.0 by definition. On the day a measurement lands the
    // two sides diverge and this FAILS.
    //
    // Reading the reference off the backend instance would make the original
    // claim true. m_dbRef is private with no accessor, and inventing a test
    // seam so a comment can be accurate is the worse trade — so the comment is
    // corrected instead. Today both sides are false and the check is right.
    check(caps.panAmplitude.has_value(),
          "the HL2 DECLARES an amplitude model — this is a read backend, not a "
          "silent one, and dbmAxisIsCalibrated() must not be answering from the "
          "absent-means-legacy branch");
    check(caps.panAmplitude
              && caps.panAmplitude->calibratedDbm == hl2::Hl2DbReference{}.isCalibrated(),
          "the dBm axis declaration matches Hl2DbReference on a fresh radio");
    check(caps.dbmAxisIsCalibrated() == hl2::Hl2DbReference{}.isCalibrated(),
          "and the accessor reports the declared field, not its absent default");
    // STILL UNCALIBRATED, AND THIS PR BRIEFLY SAID OTHERWISE. A draft of this
    // change flipped the expectation to `caps.dbmAxisIsCalibrated()` on the
    // grounds that the reference is now DERIVED rather than per-unit. The
    // derivation is real and the absolute offset it produces is the point of
    // the PR — but `calibratedDbm` is not "a figure exists", it is
    // RadioCapabilities.h's licence to compare this radio's levels with
    // another station's, and only a measurement earns that. Hl2DbReference::
    // isCalibrated() now reports whether setFullScaleDbm has been called, and
    // nothing in src/ calls it, so the honest declaration is unchanged.
    check(!caps.dbmAxisIsCalibrated(),
          "the axis is still dBFS wearing a dBm label — the reference is "
          "DERIVED, and derived is not measured");

    // WHAT THESE THREE CANNOT SEE, said plainly because this file's own thesis
    // is that a test carrying its own copy of the truth cannot detect the
    // declaration and the code diverging.
    //
    // Hl2DbReference{}.isCalibrated() is false on a default-constructed
    // reference by construction -- the measured flag starts clear and only
    // setFullScaleDbm sets it -- so it is unconditionally false. Comparing
    // the declaration against it therefore compares false with false: replace
    // `amplitude.calibratedDbm = m_dbRef.isCalibrated()` in
    // Hl2Backend::capabilities() with a literal `false` and every assertion
    // above still passes. So "the day a per-unit fullScaleDbm is populated the
    // declaration follows it" is NOT something this test observes — and worse
    // than that, per aethersdr-agent on #5726: when that day comes the two
    // sides of the comparison diverge and this assertion FAILS, because
    // production reads the backend's m_dbRef and this reads a
    // default-constructed one.
    //
    // Nothing better is reachable without a seam to set fullScaleDbm on a
    // pre-connect backend, and inventing one for a test is a worse trade than
    // stating the limit. Raised by aethersdr-agent on #5725.

    // The permissive defaults these fields carry are load-bearing, and a
    // regression that flipped either would make every silent backend change its
    // claim at once. Pin them from a default-constructed descriptor, beside the
    // HL2's overrides, so the facts fail separately.
    //
    // THE TWO ACCESSORS FALL OPPOSITE WAYS ON THE SAME ABSENT RECORD. That is
    // the point of the record and the single thing a conversion could silently
    // regress, so both directions are asserted here rather than inferred.
    check(RadioCapabilities{}.radioOwnsDbmScale,
          "radioOwnsDbmScale still defaults TRUE (the legacy shape)");
    check(!RadioCapabilities{}.panAmplitude.has_value(),
          "a descriptor nobody has written declares NO amplitude model");
    check(RadioCapabilities{}.dbmAxisIsCalibrated(),
          "absent -> CALIBRATED: the legacy claim is kept, matching the bool "
          "default this replaced");
    check(!RadioCapabilities{}.panBinsAbsolute(),
          "absent -> NOT absolute: the opposite fall, matching the bool default "
          "this replaced. The auto-floor gate's OR stays permissive through "
          "radioOwnsDbmScale instead");
    check(!RadioCapabilities{}.panSpanModel.has_value(),
          "and no span model either — a radio without the constraint, and one "
          "nobody has read, are the same descriptor only because neither is "
          "allowed to claim anything");

    // ---- band/segment zoom: the gate, and that one predicate serves both ----
    //
    // Same thesis as the rest of this file -- a control that lies is the
    // symptom of a declaration nobody reads. `band_zoom=`/`segment_zoom=` are
    // Flex wire text; RadioModel::sendCmd drops them at hasCommandPlane() and
    // emits commandDropped(). SpectrumWidget::setBandSegmentZoomAvailable()
    // disabled the two buttons for that reason, and the keyboard/MIDI/
    // FlexControl/RC28/automation paths into MainWindow::togglePanZoomModeForPan
    // and MainWindow::setPanZoomMode went around it.
    //
    // The input is READ OFF THIS RADIO'S OWN DECLARATION rather than retyped
    // as a literal false: the gate's middle rung is
    // RadioCapabilities::panZoomModes.has_value(), so that is what is asked
    // here. Engage the record in Hl2Backend::capabilities() and this section
    // follows it instead of agreeing with a stale copy -- which is the failure
    // mode the header of this file is about. (It used to ask
    // `caps.family == "flex"`, which was the family-string branch #5554's
    // standing notice forbids; the record is the sanctioned shape and does not
    // move the capability-bool ratchet, since that counts direct bool members
    // of RadioCapabilities and an std::optional is not one.)
    check(!caps.panZoomModes.has_value(),
          "this radio declares NO band/segment zoom -- absent, not a false "
          "bool, so 'nobody set it' and 'considered no' are not the same "
          "record");

    const bool hl2DeclaresPanZoomModes = caps.panZoomModes.has_value();

    check(!panZoomModeWritable(/*connected=*/true, hl2DeclaresPanZoomModes,
                               /*panKnown=*/true),
          "REFUSED on this radio even connected with a pan: band/segment zoom "
          "is Flex wire text and this backend declares no band/segment zoom");
    check(panZoomModeRefusal(/*connected=*/true, hl2DeclaresPanZoomModes,
                             /*panKnown=*/true)
              == PanZoomModeRefusal::NotDeclared,
          "and it is refused for the CAPABILITY, not for a missing pan or a "
          "missing connection -- the reason a caller would show the operator, "
          "and the ONLY rung the call sites announce with "
          "showUnsupportedControlNotice()");
    check(!bandSegmentZoomAvailable(/*connected=*/true, hl2DeclaresPanZoomModes),
          "the B/S buttons are disabled on this radio for the same reason");

    // THE CONTAINMENT, which is the half that could drift: the availability
    // the buttons show must be the admissibility the command paths test, with
    // the pan taken as present. Asserted over every input rather than by
    // comment, so a future edit that special-cases one side fails here.
    for (bool connected : {false, true}) {
        for (bool declared : {false, true}) {
            check(bandSegmentZoomAvailable(connected, declared)
                      == panZoomModeWritable(connected, declared,
                                             /*panKnown=*/true),
                  "button availability and write admissibility agree on every "
                  "(connected, declared) pair");
            // A write is never admitted without a pan, whatever the rest says.
            check(!panZoomModeWritable(connected, declared, /*panKnown=*/false),
                  "no resolvable pan is always a refusal");
        }
    }

    // A radio that DOES declare it, for contrast: the gate refuses this one for
    // a property of the radio, not because it refuses everything.
    check(panZoomModeWritable(/*connected=*/true,
                              /*panZoomModesDeclared=*/true,
                              /*panKnown=*/true),
          "a connected radio that declares the record, with a pan, IS admitted "
          "(positive control -- the refusal above is a capability decision, "
          "not a dead predicate)");
    check(panZoomModeRefusal(/*connected=*/false,
                             /*panZoomModesDeclared=*/true,
                             /*panKnown=*/true)
              == PanZoomModeRefusal::NotConnected,
          "and a disconnected one is refused for being disconnected");

    // WHAT THIS SECTION CANNOT SEE, said as plainly as the dBm note above.
    //
    // It pins the PREDICATE and the containment between its two spellings. It
    // does NOT observe that MainWindow::togglePanZoomModeForPan and
    // MainWindow::setPanZoomMode call it: no registered test target links
    // MainWindow*.cpp, so an edit that deletes the call from either one leaves
    // every assertion here green. What stands behind those two call sites is
    // that there is now only one predicate to call -- the button-availability
    // computation in MainWindow::onConnectionStateChanged reads the same
    // function -- so the drift this guards against is the predicate changing
    // under a caller, not a caller quietly dropping it.
    //
    // Closing the remaining half needs a MainWindow seam that does not exist
    // today. Stated rather than left for the next reader to assume otherwise.

    // ---- one span control when there is one span (#5750) ----
    //
    // radioWide was declared above and, until this section, read by nothing in
    // src/gui/: every pane got its own -/+ pair and pressing one re-spanned
    // them all. PanSpanControlGate.h decides which pane keeps the pair. The
    // `radioWide` input is READ OFF THIS RADIO'S DECLARATION, the same way the
    // band/segment section above reads panZoomModes, so reverting the
    // declaration fails here rather than agreeing with a retyped literal.
    {
        const bool hl2RadioWide = caps.panSpanModel && caps.panSpanModel->radioWide;
        const QStringList panes{QStringLiteral("0x40000000"),
                                QStringLiteral("0x40000001"),
                                QStringLiteral("0x40000002")};

        int shown = 0;
        for (const QString& p : panes) {
            shown += spanControlLiveOnPan(hl2RadioWide, panes,
                                           QStringLiteral("0x40000001"), p) ? 1 : 0;
        }
        check(shown == 1,
              "HL2 with three panes: exactly ONE pane's span control is live, "
              "because there is one span");
        check(spanControlLiveOnPan(hl2RadioWide, panes,
                                    QStringLiteral("0x40000001"),
                                    QStringLiteral("0x40000001")),
              "and it is the pane holding the TX slice");
        check(radioWideSpanControlPan(hl2RadioWide, panes, QString())
                  == QStringLiteral("0x40000000"),
              "with no TX slice it falls back to the FIRST pane in fallback "
              "order, a fixed pane rather than whichever was clicked last");
        check(radioWideSpanControlPan(hl2RadioWide, panes,
                                      QStringLiteral("0x40000009"))
                  == QStringLiteral("0x40000000"),
              "a TX slice on a pane the stack does not hold also falls back, "
              "so a stale pan id can never leave ZERO panes with the control");
        check(spanControlLiveOnPan(hl2RadioWide, {QStringLiteral("0x40000000")},
                                    QString(), QStringLiteral("0x40000000")),
              "a single pane on a radio-wide radio keeps its control live");

        // The other families must keep today's per-pane controls. A Flex has
        // genuinely independent spans; a radio with per-pan span declares
        // radioWide false; a backend nobody has read declares no record at
        // all, and absence must never hide a control.
        const RadioCapabilities unread{};
        const bool unreadRadioWide =
            unread.panSpanModel && unread.panSpanModel->radioWide;
        PanSpanModel perPan;
        perPan.followsSampleRate = true;
        perPan.radioWide = false;
        for (const QString& p : panes) {
            check(spanControlLiveOnPan(unreadRadioWide, panes,
                                        QStringLiteral("0x40000001"), p),
                  "no span record (Flex, Icom, Sim): every pane keeps a live control");
            check(spanControlLiveOnPan(perPan.radioWide, panes,
                                        QStringLiteral("0x40000001"), p),
                  "a per-pan span (radioWide false, as ANAN declares): every "
                  "pane keeps a live control");
        }
        check(radioWideSpanControlPan(false, panes, QStringLiteral("0x40000001"))
                  .isEmpty(),
              "per-pan answers 'every pane', not the TX pane");
    }

    // ---- the fallback is a pane in this window, in the order it is shown ----
    //
    // PanadapterStack::panIds() is QMap key order, not screen order, and still
    // holds a pane that floats in its own window or is lent to the workspace
    // canvas. The fallback is built from the DOCKED panes in layout order
    // first (#6042 review), so it never sends the one live control behind the
    // main window while a docked pane exists.
    {
        const bool hl2RadioWide = caps.panSpanModel && caps.panSpanModel->radioWide;
        const QString a = QStringLiteral("0x40000000");
        const QString b = QStringLiteral("0x40000001");
        const QString c = QStringLiteral("0x40000002");
        const QStringList all{a, b, c};  // what panIds() returns: id order

        // c is shown first, b floats: fallback order is c, a, then b.
        const QStringList order = panIdsInSpanFallbackOrder({c, a}, all);
        check(order == QStringList({c, a, b}),
              "fallback order: docked panes in layout order, then the floating one");
        check(radioWideSpanControlPan(hl2RadioWide, order, QString()) == c,
              "no TX slice: the live control goes to the first pane SHOWN, not "
              "the lowest pan id");
        check(radioWideSpanControlPan(hl2RadioWide,
                                      panIdsInSpanFallbackOrder({b, c}, all),
                                      QString()) == b,
              "and it follows a rearranged layout");
        check(radioWideSpanControlPan(hl2RadioWide,
                                      panIdsInSpanFallbackOrder({c}, {a, c}),
                                      QString()) == c,
              "a floating pane with the lowest id does not take the fallback "
              "from a docked one");
        check(radioWideSpanControlPan(hl2RadioWide, order, b) == b,
              "a floating pane holding the TX slice still keeps it live: the "
              "TX pane is the one being worked");
        check(radioWideSpanControlPan(hl2RadioWide,
                                      panIdsInSpanFallbackOrder({}, all),
                                      QString()) == a,
              "every pane floating or on the canvas: still exactly one live "
              "control, never zero");
        check(panIdsInSpanFallbackOrder({QStringLiteral("0x40000009"), c}, all)
                  == QStringList({c, a, b}),
              "a docked id the stack no longer holds is dropped, never named");
    }

    // ---- the call sites and the widget, read from the source ----
    //
    // No registered target links MainWindow*.cpp, PanadapterStack.cpp or
    // SpectrumWidget.cpp, so these are SOURCE contracts, the same kind
    // meter_applet_capability_test and rf_gain_presentation_test carry. They
    // pin the shape a reviewer found broken; they do not run it.
    {
        const QByteArray wiring = readSource("src/gui/MainWindow_Wiring.cpp");
        const QByteArray onSliceAdded =
            functionBody(wiring, "void MainWindow::onSliceAdded(SliceModel* s)");
        check(!onSliceAdded.isEmpty(), "found MainWindow::onSliceAdded");
        const QList<int> syncDepths =
            depthsOf(onSliceAdded, "syncPanSpanControlPlacement()");
        check(syncDepths.contains(1),
              "onSliceAdded re-derives the span control in its OWN body, not "
              "only inside a lambda: RadioModel applies a slice's fields before "
              "sliceAdded, so a slice that arrives already TX never fires "
              "txSliceChanged (#6042 review)");

        const QByteArray session = readSource("src/gui/MainWindow_Session.cpp");
        const QByteArray sync = functionBody(
            session, "void MainWindow::syncPanSpanControlPlacement()");
        check(sync.contains("dockedPanIdsInLayoutOrder()")
                  && sync.contains("panIdsInSpanFallbackOrder("),
              "the sync orders the fallback by the docked layout, not by "
              "panIds() alone");
        const qsizetype occ = session.indexOf(
            "connect(&m_radioModel, &RadioModel::slotOccupancyChanged,\n"
            "            this, [this](int) { syncPanSpanControlPlacement(); });");
        check(occ >= 0,
              "a reclaimed slice (no sliceAdded) re-derives the span control too");

        const QByteArray header = readSource("src/gui/SpectrumWidget.h");
        const QByteArray setter = functionBody(
            header, "    void setSpanControlPlacement(bool live, bool radioWide)");
        check(!setter.isEmpty(), "found SpectrumWidget::setSpanControlPlacement");
        check(!setter.contains("setVisible") && !setter.contains("hide()"),
              "the span pair is never hidden on a capability (AGENTS.md: dim "
              "it, never hide it)");
        check(setter.count("setEnabled(live)") == 2
                  && setter.count("setAccessibleDescription(desc)") == 2,
              "both buttons are dimmed, and both carry the reason where a "
              "screen reader reads it");

        const QByteArray widget = readSource("src/gui/SpectrumWidget.cpp");
        check(widget.contains("m_zoomOutBtn->installEventFilter(this);")
                  && widget.contains("m_zoomInBtn->installEventFilter(this);")
                  && widget.contains("|| widget == m_zoomOutBtn || widget == m_zoomInBtn)"),
              "the dimmed pair still shows its tooltip: Qt skips tooltips on a "
              "disabled widget, and eventFilter() answers them for these two");
    }

    // WHAT THIS CANNOT SEE: the widgets running. That the dimmed pair renders
    // dimmed, announces its description, and that each MainWindow event really
    // fires in the app are not observed here -- the contracts above read the
    // source; they do not execute it.

    std::printf("%s: %d failure(s)\n", argv[0], failures);
    return failures == 0 ? 0 : 1;
}
