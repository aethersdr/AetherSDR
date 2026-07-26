// Collapse semantics for owner-managed panadapter status cards (#4387).
//
// The KiwiSDR connection card is re-asserted from ~10 event-driven sync sites
// (slice retune, waterfall availability, queue-position ticks). That is exactly
// why it cannot be *dismissible*: a close would be undone by the next
// re-assertion, and in a quiet state that never re-syncs it would instead hide
// the only indicator that the pan is stale while TX stays inhibited.
//
// Collapsing has to thread that needle: the card must get out of the way, stay
// out of the way across owner re-assertion, and never disappear entirely while
// its condition holds.
#include "gui/PanadapterMessageOverlay.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QEventLoop>
#include <QVariantMap>

#include <cstdio>

using AetherSDR::PanadapterMessageOverlay;
using AetherSDR::PanadapterOverlayMessage;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

PanadapterOverlayMessage kiwiCard(const QString& detail = QStringLiteral("Disconnected"))
{
    PanadapterOverlayMessage m;
    m.id = QStringLiteral("kiwi.connection");
    m.title = QStringLiteral("Not connected to KiwiSDR");
    m.detail = detail;
    m.timeoutMs = 0;
    m.dismissible = false;
    m.collapsible = true;
    return m;
}

QVariantMap findMessage(const PanadapterMessageOverlay& overlay, const QString& id)
{
    for (const QVariant& entry : overlay.messageSnapshot()) {
        const QVariantMap m = entry.toMap();
        if (m.value(QStringLiteral("id")).toString() == id) {
            return m;
        }
    }
    return {};
}

// Card geometry in the snapshot is the animated rect, which eases toward the
// laid-out target at 16 ms per step. Spin the loop until it stops moving so
// size assertions measure the settled card rather than a frame mid-transition.
QVariantMap settledMessage(const PanadapterMessageOverlay& overlay,
                           const QString& id)
{
    QElapsedTimer overall;
    overall.start();
    QVariantMap previous;
    int stableChunks = 0;
    // Sample in 50 ms chunks (≈3 animation ticks each) rather than per
    // processEvents() call. Polling faster than the 16 ms timer sees the same
    // geometry repeatedly before the animation has even started and would
    // report the pre-transition size as "settled".
    while (overall.elapsed() < 4000) {
        QElapsedTimer chunk;
        chunk.start();
        while (chunk.elapsed() < 50) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        const QVariantMap current = findMessage(overlay, id);
        const bool unchanged = !previous.isEmpty()
            && current.value(QStringLiteral("geometry"))
                   == previous.value(QStringLiteral("geometry"));
        stableChunks = unchanged ? stableChunks + 1 : 0;
        previous = current;
        if (stableChunks >= 4 && overall.elapsed() >= 300) {
            return current;
        }
    }
    return previous;
}

// The card is never dismissible, so the owner stays the only thing that can
// remove it. This is the invariant the #3999 review established and #4387 must
// not break.
void testOwnerManagedCardIsNotDismissible()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(kiwiCard());

    const QVariantMap m = findMessage(overlay, QStringLiteral("kiwi.connection"));
    check(!m.isEmpty(), "the card is present");
    check(!m.value(QStringLiteral("dismissible")).toBool(),
          "the KiwiSDR status card is not user-dismissible");
    check(m.value(QStringLiteral("collapsible")).toBool(),
          "the KiwiSDR status card is collapsible instead");
    check(!m.value(QStringLiteral("collapsed")).toBool(),
          "the card starts expanded");
}

// The core of #4387: collapsing must actually shrink the card, and must not be
// undone by the owner re-asserting it.
void testCollapseSurvivesOwnerReassertion()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(kiwiCard());

    check(overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true),
          "collapsing a collapsible card is accepted");
    check(overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "the card reports collapsed");

    // Identical re-assertion — the common case from the sync sites.
    overlay.upsertMessage(kiwiCard());
    check(overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "an identical owner re-assertion does not re-expand the card");

    // Content-changing re-assertion — a queue-position tick rewrites `detail`
    // several times a minute, which is what made a dismissal resurrect.
    overlay.upsertMessage(kiwiCard(QStringLiteral("Waiting - queue position 3")));
    check(overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "a detail change does not re-expand the card");
    overlay.upsertMessage(kiwiCard(QStringLiteral("Waiting - queue position 2")));
    check(overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "repeated detail churn does not re-expand the card");

    const QVariantMap m = findMessage(overlay, QStringLiteral("kiwi.connection"));
    check(m.value(QStringLiteral("collapsed")).toBool(),
          "the snapshot reports the card as collapsed");
}

// Collapsed still means visible. The failure mode this guards is an operator
// looking at a local Flex trace, believing it is the remote Kiwi, with TX
// refused and no on-screen reason.
void testCollapsedCardRemainsPresent()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(kiwiCard());
    overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true);

    check(overlay.hasMessages(),
          "a collapsed card is still a live message");
    const QVariantMap m = findMessage(overlay, QStringLiteral("kiwi.connection"));
    check(!m.isEmpty(), "a collapsed card still appears in the snapshot");
    check(!m.value(QStringLiteral("title")).toString().isEmpty(),
          "a collapsed card still carries its title as the visible indicator");
    check(!overlay.accessibleDescription().contains(
              QStringLiteral("No panadapter messages")),
          "a collapsed card is still announced to assistive tech");
}

// #4387 is "the box permanently obscures the panadapter", so collapsing has to
// measurably shrink it — including being narrower, since flipping `dismissible`
// on alone would have made the card marginally *wider*.
void testCollapsedCardIsSmaller()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(
        kiwiCard(QStringLiteral("Disconnected - the remote receiver refused the "
                                "session, and this is a deliberately long detail "
                                "line so the expanded card wraps to several rows")));

    const QVariantMap expanded = settledMessage(overlay, QStringLiteral("kiwi.connection"));
    const QVariantMap expandedGeom =
        expanded.value(QStringLiteral("geometry")).toMap();
    const int expandedH = expandedGeom.value(QStringLiteral("h")).toInt();
    const int expandedW = expandedGeom.value(QStringLiteral("w")).toInt();
    check(expandedH > 0 && expandedW > 0, "expanded card has a geometry");

    overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true);
    const QVariantMap collapsed = settledMessage(overlay, QStringLiteral("kiwi.connection"));
    const QVariantMap collapsedGeom =
        collapsed.value(QStringLiteral("geometry")).toMap();
    const int collapsedH = collapsedGeom.value(QStringLiteral("h")).toInt();
    const int collapsedW = collapsedGeom.value(QStringLiteral("w")).toInt();

    check(collapsedH < expandedH,
          "collapsing makes the card shorter");
    check(collapsedW < expandedW,
          "collapsing makes the card narrower");
}

// The owner removing the card means the condition ended. A later re-raise is a
// new occurrence and must be fully visible rather than silently collapsed.
void testOwnerRemovalClearsCollapse()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(kiwiCard());
    overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true);
    check(overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "collapsed before the owner removes it");

    // setKiwiSdrConnectionOverlay(false) — the Kiwi reconnected.
    overlay.removeMessage(QStringLiteral("kiwi.connection"));
    check(!overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "owner removal clears the collapse");

    // ... and later drops again.
    overlay.upsertMessage(kiwiCard());
    check(!overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "a fresh occurrence is shown expanded, not silently collapsed");
}

void testExpandRestoresTheFullCard()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(kiwiCard());
    overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true);
    check(overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), false),
          "expanding is accepted");
    check(!overlay.isMessageCollapsed(QStringLiteral("kiwi.connection")),
          "the card reports expanded again");
    const QVariantMap m = findMessage(overlay, QStringLiteral("kiwi.connection"));
    check(!m.value(QStringLiteral("collapsed")).toBool(),
          "the snapshot reports the card as expanded");
}

// The collapsed branch is a separate paint path, so prove it actually draws:
// ink on screen, and materially less of it than the expanded card. A collapsed
// card that rendered nothing would satisfy every geometry assertion above while
// leaving the operator with no indicator at all.
void testCollapsedCardStillPaints()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    overlay.upsertMessage(
        kiwiCard(QStringLiteral("Disconnected - the remote receiver refused the "
                                "session, and this is a deliberately long detail "
                                "line so the expanded card wraps to several rows")));

    const auto inkedPixels = [&overlay]() {
        const QImage image = overlay.grab().toImage()
                                 .convertToFormat(QImage::Format_ARGB32);
        int inked = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) > 8) {
                    ++inked;
                }
            }
        }
        return inked;
    };

    settledMessage(overlay, QStringLiteral("kiwi.connection"));
    const int expandedInk = inkedPixels();
    check(expandedInk > 0, "the expanded card paints");

    overlay.setMessageCollapsed(QStringLiteral("kiwi.connection"), true);
    settledMessage(overlay, QStringLiteral("kiwi.connection"));
    const int collapsedInk = inkedPixels();

    check(collapsedInk > 0,
          "the collapsed card still paints — it is an indicator, not a hide");
    check(collapsedInk < expandedInk,
          "the collapsed card covers less of the panadapter than the expanded one");
}

// A plain dismissible card is unaffected by any of this.
void testNonCollapsibleCardRejectsCollapse()
{
    PanadapterMessageOverlay overlay;
    overlay.resize(900, 500);
    PanadapterOverlayMessage m;
    m.id = QStringLiteral("plain.card");
    m.title = QStringLiteral("Transmit disabled");
    m.detail = QStringLiteral("Slice is locked");
    overlay.upsertMessage(m);

    check(!overlay.setMessageCollapsed(QStringLiteral("plain.card"), true),
          "a non-collapsible card refuses to collapse");
    check(!overlay.isMessageCollapsed(QStringLiteral("plain.card")),
          "and does not report itself collapsed");
    check(!overlay.setMessageCollapsed(QStringLiteral("no.such.card"), true),
          "an unknown id is refused");
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    testOwnerManagedCardIsNotDismissible();
    testCollapseSurvivesOwnerReassertion();
    testCollapsedCardRemainsPresent();
    testCollapsedCardIsSmaller();
    testOwnerRemovalClearsCollapse();
    testExpandRestoresTheFullCard();
    testCollapsedCardStillPaints();
    testNonCollapsibleCardRejectsCollapse();
    if (failures == 0) {
        std::puts("Panadapter message overlay tests passed");
    }
    return failures == 0 ? 0 : 1;
}
