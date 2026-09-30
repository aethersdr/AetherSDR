// Right-clicking a client-side spot label must offer Remove Spot (#6037).
//
// The spectrum's right-click menu decided "was a label hit?" from the sign of
// the hit spot's ID (`hitSpotIdx >= 0`). Most spots this client places in
// SpotModel itself carry a negative ID — passive-local from -2000000 down
// (DX cluster, RBN, WSJT-X, POTA and manual spots on HL2, Icom or in Passive
// mode; N1MM; EiBi), memory markers from -1000000 down — so all of them fell
// through to the general-area menu: no Remove Spot, and Apply Memory was never
// reachable by right-click at all. TCI-injected spots are the exception: they
// are client-side with POSITIVE IDs, so removal routes on source, not sign.
//
// This drives the same helpers the production right-click path in
// SpectrumWidget::mousePressEvent calls: the label hit-test, the menu choice,
// the menu population whose actions are triggered here, and the removal route
// MainWindow's spotRemoveRequested handler switches on.
//
// SOCKET-FREE. Builds a QMenu offscreen; binds nothing, reaches no radio.
//
// The widget's right-click path is now one call, resolveLabelHit(), gated on
// the menu kind it returns; testResolveLabelHit feeds that call the widget's
// own shapes (hit rects + markers) with negative, zero and positive IDs, so
// the decision that used to be `hitSpotIdx >= 0` inline in the widget is
// driven here as behaviour.
//
// WHAT THIS FILE CANNOT OBSERVE: a constructed SpectrumWidget or MainWindow.
// Neither is buildable in a test here. That the widget calls resolveLabelHit,
// gates on its menu and re-tests no ID sign, and that MainWindow routes on
// removeRoute, are source pins at the end: text checks, not behaviour.
// Nothing here was run against a radio.

#include "gui/SpotLabelPolicy.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QRect>
#include <QRegularExpression>
#include <QVector>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::SpotLabelPolicy;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

// Same shape as SpectrumWidget::SpotHitRect, which is private.
struct HitRect {
    QRect rect;
    int markerIndex;
};

// The fields of SpectrumWidget::SpotMarker that resolveLabelHit reads.
struct Marker {
    int index;
    QString callsign;
    double freqMhz;
    QString source;
};

struct Recorder {
    int applied{0};
    int appliedId{0};
    int removed{0};
    int removedId{0};
    SpotLabelActions actions()
    {
        SpotLabelActions a;
        a.applyMemory = [this](int id) { ++applied; appliedId = id; };
        a.remove = [this](int id) { ++removed; removedId = id; };
        return a;
    }
};

QAction* findAction(QMenu& menu, const QString& text)
{
    for (QAction* action : menu.actions()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

// One label at markers[0], one SHistory/QRM rect whose markerIndex is past
// the marker list (as drawSpotMarkers appends them), and empty space.
const QVector<HitRect> kRects{
    {QRect(100, 10, 60, 14), 0},
    {QRect(300, 10, 60, 14), 7},
};
constexpr qsizetype kMarkerCount = 1;
const QPoint kOnLabel(120, 15);
const QPoint kOnQrm(320, 15);
const QPoint kEmpty(220, 15);

void testHitTestIgnoresIdSpace()
{
    report("a label rect with a valid marker is a hit",
           labelMarkerAt(kRects, kMarkerCount, kOnLabel) == 0);
    report("empty space is no hit",
           labelMarkerAt(kRects, kMarkerCount, kEmpty) == -1);
    report("an SHistory/QRM rect is no spot-label hit",
           labelMarkerAt(kRects, kMarkerCount, kOnQrm) == -1);
}

void testResolveLabelHit()
{
    // Three labels side by side plus an SHistory/QRM rect past the marker
    // list: the widget's own layout, with every ID class under the cursor.
    const QVector<HitRect> rects{
        {QRect(0, 10, 60, 14), 0},
        {QRect(100, 10, 60, 14), 1},
        {QRect(200, 10, 60, 14), 2},
        {QRect(300, 10, 60, 14), 3},
        {QRect(400, 10, 60, 14), 9},
    };
    const QVector<Marker> markers{
        {-kPassiveSpotIdBase, QStringLiteral("ON4ABC"), 7.074, QStringLiteral("DXCluster")},
        {-kMemorySpotIdBase, QStringLiteral("Net"), 3.705, QStringLiteral("Memory")},
        {0, QStringLiteral("K1ABC"), 14.025, QStringLiteral("RBN")},
        {10000, QStringLiteral("DL1XYZ"), 14.074, QStringLiteral("TCI")},
    };

    const LabelHit local = resolveLabelHit(rects, markers, QPoint(20, 15));
    report("resolve: a passive-local label (-2000000) opens the spot menu",
           local.menu == Menu::Spot && local.spotId == -kPassiveSpotIdBase
           && local.callsign == QLatin1String("ON4ABC") && local.freqMhz == 7.074);

    const LabelHit memory = resolveLabelHit(rects, markers, QPoint(120, 15));
    report("resolve: a memory label (-1000000) opens Apply Memory",
           memory.menu == Menu::ApplyMemory && memory.spotId == -kMemorySpotIdBase);

    const LabelHit radio = resolveLabelHit(rects, markers, QPoint(220, 15));
    report("resolve: a radio label (0) opens the spot menu",
           radio.menu == Menu::Spot && radio.spotId == 0);

    const LabelHit tci = resolveLabelHit(rects, markers, QPoint(320, 15));
    report("resolve: a TCI label (10000) opens the spot menu",
           tci.menu == Menu::Spot && tci.spotId == 10000
           && tci.source == QLatin1String("TCI"));

    const LabelHit qrm = resolveLabelHit(rects, markers, QPoint(420, 15));
    report("resolve: an SHistory/QRM rect opens the general-area menu",
           qrm.menu == Menu::General && qrm.spotId == -1);

    const LabelHit empty = resolveLabelHit(rects, markers, QPoint(80, 15));
    report("resolve: empty space opens the general-area menu",
           empty.menu == Menu::General && empty.spotId == -1);
}

void testMenuKinds()
{
    report("a passive-local label opens the spot menu",
           menuFor(true, QStringLiteral("DXCluster")) == Menu::Spot);
    report("a memory label opens Apply Memory",
           menuFor(true, QStringLiteral("Memory")) == Menu::ApplyMemory);
    report("no label hit opens the general-area menu",
           menuFor(false, QString()) == Menu::General);
}

void testLocalSpotRemovesLocally()
{
    const int localId = -kPassiveSpotIdBase;  // first passive-local ID
    const int hit = labelMarkerAt(kRects, kMarkerCount, kOnLabel);
    QMenu menu;
    Recorder rec;
    addSpotLabelActions(menu, &menu, menuFor(hit >= 0, QStringLiteral("DXCluster")),
                        localId, QStringLiteral("ON4ABC"), 7.074, rec.actions());
    QAction* remove = findAction(menu, QStringLiteral("Remove Spot"));
    report("local ID -2000000: Remove Spot is offered", remove != nullptr);
    if (remove)
        remove->trigger();
    report("local ID -2000000: Remove Spot requests that exact ID",
           rec.removed == 1 && rec.removedId == localId);
    report("local ID -2000000: removal routes to SpotModel, no wire text",
           removeRoute(localId, QStringLiteral("DXCluster")) == RemoveRoute::LocalModel);
    report("a later-allocated passive-local ID is still local",
           removeRoute(-kPassiveSpotIdBase - 12345, QStringLiteral("N1MM")) == RemoveRoute::LocalModel);
}

void testRadioSpotUnchanged()
{
    QMenu menu;
    Recorder rec;
    addSpotLabelActions(menu, &menu, menuFor(true, QStringLiteral("RBN")),
                        0, QStringLiteral("K1ABC"), 14.025, rec.actions());
    QAction* remove = findAction(menu, QStringLiteral("Remove Spot"));
    report("radio ID 0: Remove Spot is offered", remove != nullptr);
    report("radio ID 0: Tune and lookup actions remain",
           findAction(menu, QStringLiteral("Tune to K1ABC")) != nullptr
           && findAction(menu, QStringLiteral("Copy Callsign")) != nullptr
           && findAction(menu, QStringLiteral("Lookup on QRZ")) != nullptr);
    if (remove)
        remove->trigger();
    report("radio ID 0: Remove Spot requests ID 0",
           rec.removed == 1 && rec.removedId == 0);
    report("radio ID 0: removal still goes to the radio as `spot remove`",
           removeRoute(0, QStringLiteral("RBN")) == RemoveRoute::RadioCommand
           && removeRoute(42, QStringLiteral("DXCluster")) == RemoveRoute::RadioCommand);
}

void testMemoryOffersApplyOnly()
{
    const int memoryId = -kMemorySpotIdBase;
    QMenu menu;
    Recorder rec;
    addSpotLabelActions(menu, &menu, menuFor(true, QStringLiteral("Memory")),
                        memoryId, QStringLiteral("Net"), 3.705, rec.actions());
    QAction* apply = findAction(menu, QStringLiteral("Apply Net"));
    report("memory ID -1000000: Apply Memory is offered", apply != nullptr);
    report("memory ID -1000000: Remove Spot is not offered",
           findAction(menu, QStringLiteral("Remove Spot")) == nullptr);
    if (apply)
        apply->trigger();
    report("memory ID -1000000: Apply triggers that exact ID",
           rec.applied == 1 && rec.appliedId == memoryId);
    report("a memory ID never becomes `spot remove` wire text",
           removeRoute(memoryId, QStringLiteral("Memory")) == RemoveRoute::Ignore);
}

// TCI-injected spots are client-side but carry POSITIVE IDs
// (TciProtocol::cmdSpot allocates them from 10000 up). The sign of the ID
// cannot tell them from a radio spot; the source must (review of #6041).
void testTciSpotRemovesLocally()
{
    const int tciId = 10000;  // the first ID TciProtocol::cmdSpot hands out
    QMenu menu;
    Recorder rec;
    addSpotLabelActions(menu, &menu, menuFor(true, QStringLiteral("TCI")),
                        tciId, QStringLiteral("DL1XYZ"), 14.074, rec.actions());
    QAction* remove = findAction(menu, QStringLiteral("Remove Spot"));
    report("TCI ID 10000: Remove Spot is offered", remove != nullptr);
    if (remove)
        remove->trigger();
    report("TCI ID 10000: Remove Spot requests that exact ID",
           rec.removed == 1 && rec.removedId == tciId);
    report("TCI ID 10000: removal routes to SpotModel, no `spot remove` wire text",
           removeRoute(tciId, QStringLiteral("TCI")) == RemoveRoute::LocalModel);
    report("the TCI source matches case-insensitively, as spotTriggered does",
           removeRoute(tciId + 7, QStringLiteral("tci")) == RemoveRoute::LocalModel);
    report("a radio spot at the same positive ID still goes to the radio",
           removeRoute(tciId, QStringLiteral("RBN")) == RemoveRoute::RadioCommand);
    report("a radio ID no longer in the model (empty source) still goes to the radio",
           removeRoute(tciId, QString()) == RemoveRoute::RadioCommand);
}

void testGeneralAreaAddsNoSpotActions()
{
    QMenu menu;
    Recorder rec;
    addSpotLabelActions(menu, &menu, menuFor(false, QString()), -1, QString(), 0.0,
                        rec.actions());
    report("empty space: no spot actions are added", menu.actions().isEmpty());
}

QByteArray readSource(const char* relative)
{
    QFile f(QString::fromUtf8(AETHER_SOURCE_DIR) + QLatin1Char('/')
            + QString::fromUtf8(relative));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void testShippingCallSites()
{
    const QByteArray widget = readSource("src/gui/SpectrumWidget.cpp");
    report("the test can read SpectrumWidget.cpp", !widget.isEmpty());
    // Any sign test on the hit spot's ID, however spelt: `>= 0`, `> -1`,
    // `!= -1`, `< 0`, with or without spaces.
    const QRegularExpression signGate(QStringLiteral(
        R"((hitSpotIdx|hit\.spotId)\s*(>=\s*0|>\s*-1|!=\s*-1|<\s*0|==\s*-1)\b)"));
    report("the right-click path no longer gates on the ID sign",
           !signGate.match(QString::fromUtf8(widget)).hasMatch());
    report("the right-click path resolves the label through the shared helper",
           widget.contains("SpotLabelPolicy::resolveLabelHit("));
    report("the right-click path gates the spot menu on the resolved menu kind",
           widget.contains("if (hit.menu != SpotLabelPolicy::Menu::General)"));
    report("the right-click path populates the menu through the helper",
           widget.contains("SpotLabelPolicy::addSpotLabelActions("));

    const QByteArray wiring = readSource("src/gui/MainWindow_Wiring.cpp");
    report("the test can read MainWindow_Wiring.cpp", !wiring.isEmpty());
    report("spotRemoveRequested switches on the removal route",
           wiring.contains("SpotLabelPolicy::removeRoute(spotIndex, source)"));
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    testHitTestIgnoresIdSpace();
    testResolveLabelHit();
    testMenuKinds();
    testLocalSpotRemovesLocally();
    testRadioSpotUnchanged();
    testMemoryOffersApplyOnly();
    testTciSpotRemovesLocally();
    testGeneralAreaAddsNoSpotActions();
    testShippingCallSites();

    return g_failed == 0 ? 0 : 1;
}
