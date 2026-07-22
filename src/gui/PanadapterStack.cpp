#include "PanadapterStack.h"
#include "BandStackPanel.h"
#include "PanFloatingWindow.h"
#include "PanadapterApplet.h"
#include "PanadapterRenderScheduler.h"
#include "SpectrumWidget.h"
#include "core/AppSettings.h"

#include <QHBoxLayout>
#include <QLayout>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>
#include <QPointer>
#include <QTimer>
#include <QWindow>

// After moving a QRhiWidget between top-level windows, force a fresh initialize()
// cycle so Metal binds to the new NSView. The backing-store notification is sent
// before the actual reparent; sending it again here can make QRhiWidget remove a
// stale cleanup callback from the wrong QRhi during startup floating restore.
static void refreshAfterReparent(AetherSDR::SpectrumWidget* sw)
{
    if (!sw) return;
#if defined(Q_OS_MAC) && defined(AETHER_GPU_SPECTRUM)
    const bool wasVisible = sw->isVisible();
    sw->hide();
    sw->resetGpuResources();
    if (QWindow* windowHandle = sw->windowHandle()) {
        windowHandle->destroy();
    }
    // Re-realize the native leaf with its ancestor isolation intact — the helper
    // reasserts WA_NativeWindow *and* WA_DontCreateNativeAncestors as a pair, so a
    // reparent can't promote ancestors to native (redundant backing stores, #4339).
    sw->applyNativeWindowIsolationPolicy();
    if (wasVisible) {
        sw->show();
    }
    QTimer::singleShot(50, sw, [sw]() { sw->update(); });
#else
    sw->resetGpuResources();
#endif
}

namespace AetherSDR {

namespace {

static const QMap<QString, int> kLayoutPanCount = {
    {"1", 1}, {"2v", 2}, {"2h", 2}, {"2h1", 3}, {"12h", 3}, {"3v", 3},
    {"2x2", 4}, {"4v", 4}, {"3h2", 5}, {"2x3", 6}, {"4h3", 7}, {"2x4", 8}
};

QString defaultDockedLayoutForCount(int panCount)
{
    static const QMap<int, QString> kDefaultLayouts = {
        {1, QStringLiteral("1")},
        {2, QStringLiteral("2v")},
        {3, QStringLiteral("2h1")},
        {4, QStringLiteral("2x2")},
        {5, QStringLiteral("3h2")},
        {6, QStringLiteral("2x3")},
        {7, QStringLiteral("4h3")},
        {8, QStringLiteral("2x4")}
    };
    return kDefaultLayouts.value(panCount, QStringLiteral("1"));
}

} // namespace

PanadapterStack::PanadapterStack(QWidget* parent)
    : QWidget(parent)
{
    m_renderScheduler = new PanadapterRenderScheduler(this);

    auto* hbox = new QHBoxLayout(this);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // Band stack panel (hidden by default, left of panadapter)
    m_bandStackPanel = new BandStackPanel(this);
    m_bandStackPanel->setVisible(false);
    hbox->addWidget(m_bandStackPanel);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    hbox->addWidget(m_splitter, 1);
}

void PanadapterStack::setBandStackVisible(bool visible)
{
    m_bandStackPanel->setVisible(visible);
}

PanadapterApplet* PanadapterStack::addPanadapter(const QString& panId)
{
    m_seenPanIds.insert(panId);

    if (m_pans.contains(panId))
        return m_pans[panId];

    auto* applet = new PanadapterApplet(m_splitter);
    applet->setPanId(panId);
    QSet<int> usedPanIndices;
    for (PanadapterApplet* existing : m_pans) {
        if (existing && existing->spectrumWidget()) {
            usedPanIndices.insert(existing->spectrumWidget()->panIndex());
        }
    }
    int panIndex = 0;
    while (usedPanIndices.contains(panIndex)) {
        ++panIndex;
    }
    applet->spectrumWidget()->setPanIndex(panIndex);
    applet->spectrumWidget()->setRenderScheduler(m_renderScheduler);
    applet->spectrumWidget()->loadSettings();
    m_splitter->addWidget(applet);

    // Equal stretch for all pans
    const int idx = m_splitter->indexOf(applet);
    m_splitter->setStretchFactor(idx, 1);

    m_pans[panId] = applet;

    // First pan becomes active
    if (m_activePanId.isEmpty())
        setActivePan(panId);

    // Update multi-pan mode decorations on all applets
    bool multi = m_pans.size() > 1;
    for (auto* a : m_pans)
        a->setMultiPanMode(multi);

    // Equalize sizes whenever a pan is added
    if (multi)
        equalizeSizes();

    return applet;
}

void PanadapterStack::removePanadapter(const QString& panId)
{
    // Close floating window if this pan is floating
    if (auto* fw = m_floatingWindows.take(panId)) {
        fw->setShuttingDown(true);
        // Don't takeApplet — the applet will be deleted below
        // just disconnect and hide the window
        fw->disconnect();
        fw->hide();
        fw->deleteLater();
    }

    auto* applet = m_pans.take(panId);
    if (!applet) return;

    delete applet;

    // If active was removed, switch to first remaining
    if (m_activePanId == panId) {
        if (!m_pans.isEmpty())
            setActivePan(m_pans.firstKey());
        else
            m_activePanId.clear();
    }

    // Update multi-pan mode decorations
    bool multi = m_pans.size() > 1;
    for (auto* a : m_pans)
        a->setMultiPanMode(multi);
}

void PanadapterStack::rekey(const QString& oldId, const QString& newId)
{
    if (auto* applet = m_pans.take(oldId)) {
        m_pans[newId] = applet;
        m_seenPanIds.remove(oldId);
        m_seenPanIds.insert(newId);
        if (m_activePanId == oldId)
            m_activePanId = newId;
    }
}

PanadapterApplet* PanadapterStack::panadapter(const QString& panId) const
{
    return m_pans.value(panId, nullptr);
}

SpectrumWidget* PanadapterStack::spectrum(const QString& panId) const
{
    auto* applet = m_pans.value(panId, nullptr);
    return applet ? applet->spectrumWidget() : nullptr;
}

PanadapterApplet* PanadapterStack::activeApplet() const
{
    return m_pans.value(m_activePanId, nullptr);
}

SpectrumWidget* PanadapterStack::activeSpectrum() const
{
    auto* applet = activeApplet();
    return applet ? applet->spectrumWidget() : nullptr;
}

void PanadapterStack::setActivePan(const QString& panId)
{
    if (m_activePanId == panId) return;
    m_activePanId = panId;

    // Visual indicator: update active border via property (no stylesheet churn)
    for (auto it = m_pans.begin(); it != m_pans.end(); ++it) {
        it.value()->setProperty("activePan", it.key() == panId);
        it.value()->update();
    }

    emit activePanChanged(panId);
}

static void equalizeSplitter(QSplitter* splitter)
{
    const int count = splitter->count();
    if (count < 2) return;
    const int total = (splitter->orientation() == Qt::Horizontal)
                        ? splitter->width() : splitter->height();
    const int each = total / count;
    QList<int> sizes;
    for (int i = 0; i < count; ++i) {
        sizes.append(each);
        // Recurse into nested splitters
        if (auto* nested = qobject_cast<QSplitter*>(splitter->widget(i)))
            equalizeSplitter(nested);
    }
    splitter->setSizes(sizes);
}

void PanadapterStack::equalizeSizes()
{
    equalizeSplitter(m_splitter);
}

int PanadapterStack::layoutRequiredPanCount(const QString& layoutId)
{
    // Minimum applet count each layout id needs before rearrangeLayout()'s
    // guarded branch takes it; below that (or for an unknown id) the final
    // else falls back to a plain vertical stack. Mirrors the `>=` guards in
    // rearrangeLayout()/rebuildDockedSplitter() — keep in sync. Returns -1
    // for an id no branch recognizes, so callers can reject typos instead of
    // silently exercising the trivial fallback (#4091 test honesty).
    static const QHash<QString, int> kRequired = {
        {QStringLiteral("1"), 1},   {QStringLiteral("2v"), 2},
        {QStringLiteral("2h"), 2},  {QStringLiteral("2h1"), 3},
        {QStringLiteral("12h"), 3}, {QStringLiteral("3v"), 3},
        {QStringLiteral("2x2"), 4}, {QStringLiteral("4v"), 4},
        {QStringLiteral("3h2"), 5}, {QStringLiteral("2x3"), 6},
        {QStringLiteral("4h3"), 7}, {QStringLiteral("2x4"), 8},
    };
    return kRequired.value(layoutId, -1);
}

QVariantMap PanadapterStack::automationRearrange(const QString& layoutId)
{
    // Non-empty id drives the real production rearrange (exercising the
    // splitter teardown/reparent path); empty id is a query-only report.
    // An unknown id is rejected up-front: rearrangeLayout()'s final else
    // would silently build a trivial vertical stack — no nested splitters —
    // and a test asserting ok:true would believe it exercised the #4091
    // reparent path when it didn't. An id needing more applets than exist
    // takes the same fallback; that IS reachable intent (production allows
    // it), so it runs but is reported honestly via fellBack.
    bool fellBack = false;
    if (!layoutId.isEmpty()) {
        const int required = layoutRequiredPanCount(layoutId);
        if (required < 0) {
            return QVariantMap{
                {QStringLiteral("error"),
                 QStringLiteral("unknown layout id: ") + layoutId
                     + QStringLiteral(" (1|2v|2h|2h1|12h|3v|2x2|4v|3h2|2x3|4h3|2x4)")},
            };
        }
        fellBack = m_pans.size() < required;
        rearrangeLayout(layoutId);
    }

    const int floating = m_floatingWindows.size();
    return QVariantMap{
        {QStringLiteral("requested"), layoutId},
        {QStringLiteral("applied"), !layoutId.isEmpty()},
        // True when the id needed more applets than exist, so rearrange built
        // the vertical-stack fallback rather than the requested layout.
        {QStringLiteral("fellBack"), fellBack},
        {QStringLiteral("effectiveLayout"),
         fellBack ? QStringLiteral("vstack") : layoutId},
        {QStringLiteral("panCount"), m_pans.size()},
        {QStringLiteral("dockedCount"), m_pans.size() - floating},
        {QStringLiteral("floatingCount"), floating},
        // Ex-floating pans re-show and sizes equalize on the NEXT event-loop
        // turn (rearrangeLayout defers them) — re-poll get rhi / grab after.
        {QStringLiteral("settlesNextTurn"), !layoutId.isEmpty()},
        {QStringLiteral("savedLayout"),
         AppSettings::instance().value(QStringLiteral("PanadapterLayout"),
                                       QStringLiteral("1")).toString()},
    };
}

void PanadapterStack::rearrangeLayout(const QString& layoutId)
{
    // Dock any floating pans first.  m_pans always contains every applet
    // including those currently in a PanFloatingWindow (floatPanadapter
    // never removes from m_pans), so the reparent loop below would yank
    // a floating applet out of its native window via setParent(nullptr),
    // bypassing the GPU-reset path that float/dock use.  The QRhiWidget
    // would stay bound to the floating window's HWND/NSView — the next
    // render produces a black surface and the next input event into it
    // crashes (#2495).  Stale entries would also remain in
    // m_floatingWindows, permanently blocking future float requests.
    QList<QPointer<SpectrumWidget>> rebound;
    auto appendRebound = [&rebound](SpectrumWidget* sw) {
        if (!sw) return;
        for (SpectrumWidget* existing : rebound) {
            if (existing == sw) {
                return;
            }
        }
        rebound.append(sw);
    };
    if (!m_floatingWindows.isEmpty()) {
        const QList<QString> floatingIds = m_floatingWindows.keys();
        for (const QString& panId : floatingIds) {
            PanFloatingWindow* fw = m_floatingWindows.take(panId);
            if (!fw) continue;
            fw->saveWindowGeometry();
            PanadapterApplet* applet = fw->applet();
            if (auto* sw = applet ? applet->spectrumWidget() : nullptr) {
                sw->hide();
                sw->prepareForTopLevelChange();
                sw->resetGpuResources();
                appendRebound(sw);
            }
            applet = fw->takeApplet();
            fw->hide();
            fw->deleteLater();
            if (applet) {
                applet->setFloatingState(false);
                applet->spectrumWidget()->setFloating(false);
            }
            emit panDocked(panId);
        }
        saveFloatingState();
    }

    // Collect applets in order
    QList<PanadapterApplet*> applets = m_pans.values();
    if (applets.isEmpty()) return;

    // Build the new splitter first, then move applets straight into it below.
    // addWidget() reparents splitter→splitter within the same top-level window
    // in one step — the same pattern rebuildDockedSplitter() and
    // dockPanadapter() already use — so the QRhiWidget's backing-store QRhi
    // and cleanup-callback registration never change and no GPU teardown is
    // needed for pans that stay docked. The old code detoured every applet
    // through setParent(nullptr), which puts the QRhiWidget in a transient
    // top-level state and forces the full hide/prepare/reset/rebuild dance;
    // that destroy→recreate→resize storm is what the 2016-era Intel D3D11
    // UMD (igd10iumd64.dll) null-derefed on when adding a 2nd pan (#4091).
    // Only pans returning from floating windows (handled above) change
    // top-level windows and need the reset.
    QSplitter* oldSplitter = m_splitter;
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    layout()->addWidget(m_splitter);

    // Create a horizontal sub-splitter already attached to the (in-window)
    // m_splitter, so pans reparented into it never transit a parentless,
    // transient top-level window. A *live* QRhiWidget that changes top-level
    // window mid-rearrange changes its backing-store QRhi; since this path
    // deliberately skips the teardown dance (docked pans keep their QRhi —
    // see above), that would leave a stale cleanup callback / reconfigure the
    // swapchain mid-render → Intel D3D11 null-deref. This only bit the nested
    // layouts (3+ pans): the flat 2v/2h/3v/4v cases reparent straight into the
    // already-in-window m_splitter and were fixed already; the sub-splitters
    // were still built parentless-then-filled, so the 3rd-pan add kept
    // crashing (#4091, follow-up). Sub-splitter must join the window BEFORE it
    // is filled — hence addWidget(sub) here, addWidget(pan) at the call site.
    auto addRow = [this]() {
        auto* s = new QSplitter(Qt::Horizontal);
        s->setHandleWidth(3);
        s->setChildrenCollapsible(false);
        m_splitter->addWidget(s);
        return s;
    };

    if (layoutId == "2h" && applets.size() >= 2) {
        m_splitter->setOrientation(Qt::Horizontal);
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
    }
    else if (layoutId == "2h1" && applets.size() >= 3) {
        // A|B on top, C on bottom
        auto* topSplit = addRow();
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        m_splitter->addWidget(applets[2]);
    }
    else if (layoutId == "12h" && applets.size() >= 3) {
        // A on top, B|C on bottom
        m_splitter->addWidget(applets[0]);
        auto* botSplit = addRow();
        botSplit->addWidget(applets[1]);
        botSplit->addWidget(applets[2]);
    }
    else if (layoutId == "3v" && applets.size() >= 3) {
        // A / B / C vertical stack
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
        m_splitter->addWidget(applets[2]);
    }
    else if (layoutId == "2x2" && applets.size() >= 4) {
        // A|B on top, C|D on bottom
        auto* topSplit = addRow();
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        auto* botSplit = addRow();
        botSplit->addWidget(applets[2]);
        botSplit->addWidget(applets[3]);
    }
    else if (layoutId == "4v" && applets.size() >= 4) {
        // A / B / C / D vertical stack
        m_splitter->addWidget(applets[0]);
        m_splitter->addWidget(applets[1]);
        m_splitter->addWidget(applets[2]);
        m_splitter->addWidget(applets[3]);
    }
    else if (layoutId == "3h2" && applets.size() >= 5) {
        // A|B|C on top, D|E on bottom
        auto* topSplit = addRow();
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        topSplit->addWidget(applets[2]);
        auto* botSplit = addRow();
        botSplit->addWidget(applets[3]);
        botSplit->addWidget(applets[4]);
    }
    else if (layoutId == "2x3" && applets.size() >= 6) {
        // A|B / C|D / E|F — three rows of two
        for (int r = 0; r < 3; ++r) {
            auto* rowSplit = addRow();
            rowSplit->addWidget(applets[r * 2]);
            rowSplit->addWidget(applets[r * 2 + 1]);
        }
    }
    else if (layoutId == "4h3" && applets.size() >= 7) {
        // A|B|C|D on top, E|F|G on bottom
        auto* topSplit = addRow();
        topSplit->addWidget(applets[0]);
        topSplit->addWidget(applets[1]);
        topSplit->addWidget(applets[2]);
        topSplit->addWidget(applets[3]);
        auto* botSplit = addRow();
        botSplit->addWidget(applets[4]);
        botSplit->addWidget(applets[5]);
        botSplit->addWidget(applets[6]);
    }
    else if (layoutId == "2x4" && applets.size() >= 8) {
        // A|B / C|D / E|F / G|H — four rows of two
        for (int r = 0; r < 4; ++r) {
            auto* rowSplit = addRow();
            rowSplit->addWidget(applets[r * 2]);
            rowSplit->addWidget(applets[r * 2 + 1]);
        }
    }
    else {
        // Default: vertical stack (2v, 1, or fallback)
        for (auto* a : applets)
            m_splitter->addWidget(a);
    }

    // Safety sweep: layout branches only place the applets their layout id
    // calls for (e.g. "2h" places two). If the count ever mismatches the id,
    // any leftover would still be a child of oldSplitter and die with its
    // deleteLater() below, leaving dangling pointers in m_pans — fold
    // stragglers into the new splitter instead.
    for (auto* a : applets) {
        if (a && !m_splitter->isAncestorOf(a))
            m_splitter->addWidget(a);
    }

    // All applets have moved to the new splitter — retire the old one.
    layout()->removeWidget(oldSplitter);
    oldSplitter->hide();
    oldSplitter->deleteLater();

    // Equalize immediately so each pan's first layout pass in the new
    // splitter already lands at final geometry. The new splitter hasn't had
    // its layout pass yet, so the absolute values are provisional — but
    // QSplitter redistributes proportionally on resize, so equal stays
    // equal. This collapses the old default-sizes-then-deferred-equalize
    // two-step into a single resize per pan; every avoided resize is one
    // fewer swapchain rebuild for marginal GPU drivers to survive (#4091).
    equalizeSizes();

    // Deferred pass: re-show + refresh GPU surfaces for any spectrum widgets
    // that came out of a floating window, so they bind to the new top-level
    // window before the first render (mirrors the dockPanadapter() refresh
    // dance). The trailing equalize is a no-op when sizes are already equal;
    // it only settles integer-rounding drift after the real layout pass.
    QTimer::singleShot(0, this, [this, rebound]() {
        for (SpectrumWidget* sw : rebound) {
            if (!sw) continue;
            refreshAfterReparent(sw);
            sw->show();
        }
        equalizeSizes();
    });
}

void PanadapterStack::removeAll()
{
    // Close floating windows first
    for (auto* fw : m_floatingWindows) {
        fw->takeApplet();
        delete fw;
    }
    m_floatingWindows.clear();

    qDeleteAll(m_pans);
    m_pans.clear();
    m_activePanId.clear();

    // Delete the old splitter and create a fresh one
    delete m_splitter;
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(3);
    m_splitter->setChildrenCollapsible(false);
    layout()->addWidget(m_splitter);
}

void PanadapterStack::rebuildDockedSplitter()
{
    QList<PanadapterApplet*> docked;
    for (auto it = m_pans.cbegin(); it != m_pans.cend(); ++it) {
        if (!m_floatingWindows.contains(it.key())) {
            docked.append(it.value());
        }
    }

    QSplitter* oldSplitter = m_splitter;
    auto* newSplitter = new QSplitter(Qt::Vertical, this);
    newSplitter->setHandleWidth(3);
    newSplitter->setChildrenCollapsible(false);
    layout()->addWidget(newSplitter);

    QString layoutId = AppSettings::instance().value("PanadapterLayout", "1").toString();
    if (!m_floatingWindows.isEmpty() || kLayoutPanCount.value(layoutId, -1) != docked.size()) {
        layoutId = defaultDockedLayoutForCount(docked.size());
    }

    // Same transient-top-level hazard as rearrangeLayout(): attach each
    // horizontal sub-splitter to the in-window newSplitter BEFORE filling it,
    // so a live QRhiWidget never transits a parentless top-level on dock/float
    // (#4091). Fill happens at the call site, after addRow().
    auto addRow = [&]() {
        auto* s = new QSplitter(Qt::Horizontal);
        s->setHandleWidth(3);
        s->setChildrenCollapsible(false);
        newSplitter->addWidget(s);
        return s;
    };

    auto addVertical = [&]() {
        newSplitter->setOrientation(Qt::Vertical);
        for (PanadapterApplet* applet : docked) {
            newSplitter->addWidget(applet);
            applet->show();
        }
    };

    if (layoutId == "2h" && docked.size() >= 2) {
        newSplitter->setOrientation(Qt::Horizontal);
        newSplitter->addWidget(docked[0]);
        newSplitter->addWidget(docked[1]);
    } else if (layoutId == "2h1" && docked.size() >= 3) {
        auto* topSplit = addRow();
        topSplit->addWidget(docked[0]);
        topSplit->addWidget(docked[1]);
        newSplitter->addWidget(docked[2]);
    } else if (layoutId == "12h" && docked.size() >= 3) {
        newSplitter->addWidget(docked[0]);
        auto* botSplit = addRow();
        botSplit->addWidget(docked[1]);
        botSplit->addWidget(docked[2]);
    } else if (layoutId == "2x2" && docked.size() >= 4) {
        auto* topSplit = addRow();
        topSplit->addWidget(docked[0]);
        topSplit->addWidget(docked[1]);
        auto* botSplit = addRow();
        botSplit->addWidget(docked[2]);
        botSplit->addWidget(docked[3]);
    } else if (layoutId == "3h2" && docked.size() >= 5) {
        auto* topSplit = addRow();
        topSplit->addWidget(docked[0]);
        topSplit->addWidget(docked[1]);
        topSplit->addWidget(docked[2]);
        auto* botSplit = addRow();
        botSplit->addWidget(docked[3]);
        botSplit->addWidget(docked[4]);
    } else if (layoutId == "2x3" && docked.size() >= 6) {
        for (int r = 0; r < 3; ++r) {
            auto* rowSplit = addRow();
            rowSplit->addWidget(docked[r * 2]);
            rowSplit->addWidget(docked[r * 2 + 1]);
        }
    } else if (layoutId == "4h3" && docked.size() >= 7) {
        auto* topSplit = addRow();
        topSplit->addWidget(docked[0]);
        topSplit->addWidget(docked[1]);
        topSplit->addWidget(docked[2]);
        topSplit->addWidget(docked[3]);
        auto* botSplit = addRow();
        botSplit->addWidget(docked[4]);
        botSplit->addWidget(docked[5]);
        botSplit->addWidget(docked[6]);
    } else if (layoutId == "2x4" && docked.size() >= 8) {
        for (int r = 0; r < 4; ++r) {
            auto* rowSplit = addRow();
            rowSplit->addWidget(docked[r * 2]);
            rowSplit->addWidget(docked[r * 2 + 1]);
        }
    } else {
        addVertical();
    }

    layout()->removeWidget(m_splitter);
    oldSplitter->hide();
    oldSplitter->deleteLater();
    m_splitter = newSplitter;

    QTimer::singleShot(0, this, [this]() { equalizeSizes(); });
}

void PanadapterStack::applyLayout(const QString& layoutId, const QStringList& panIds)
{
    // Build structure based on layout ID.
    // Each layout adds applets to the correct splitter position.
    // panIds must have at least as many entries as the layout requires.

    if (layoutId == "1" && panIds.size() >= 1) {
        // Single pan — just add to vertical splitter
        addPanadapter(panIds[0]);
    }
    else if (layoutId == "2v" && panIds.size() >= 2) {
        // A / B — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
    }
    else if (layoutId == "2h" && panIds.size() >= 2) {
        // A | B — horizontal split
        // Replace the vertical splitter orientation
        m_splitter->setOrientation(Qt::Horizontal);
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
    }
    else if (layoutId == "2h1" && panIds.size() >= 3) {
        // A|B / C — horizontal top, single bottom
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(topSplit);

        auto* a = new PanadapterApplet(topSplit);
        a->setPanId(panIds[0]);
        a->spectrumWidget()->setPanIndex(0);
        a->spectrumWidget()->loadSettings();
        topSplit->addWidget(a);
        m_pans[panIds[0]] = a;

        auto* b = new PanadapterApplet(topSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        topSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* c = addPanadapter(panIds[2]);
        Q_UNUSED(c);

        // Equal row heights
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "12h" && panIds.size() >= 3) {
        // A / B|C — single top, horizontal bottom
        addPanadapter(panIds[0]);

        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(botSplit);

        auto* b = new PanadapterApplet(botSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        botSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* c = new PanadapterApplet(botSplit);
        c->setPanId(panIds[2]);
        c->spectrumWidget()->setPanIndex(2);
        c->spectrumWidget()->loadSettings();
        botSplit->addWidget(c);
        m_pans[panIds[2]] = c;

        // Equal row heights
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "2x2" && panIds.size() >= 4) {
        // A|B / C|D — 2×2 grid
        auto* topSplit = new QSplitter(Qt::Horizontal);
        topSplit->setHandleWidth(3);
        topSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(topSplit);

        auto* a = new PanadapterApplet(topSplit);
        a->setPanId(panIds[0]);
        a->spectrumWidget()->setPanIndex(0);
        a->spectrumWidget()->loadSettings();
        topSplit->addWidget(a);
        m_pans[panIds[0]] = a;

        auto* b = new PanadapterApplet(topSplit);
        b->setPanId(panIds[1]);
        b->spectrumWidget()->setPanIndex(1);
        b->spectrumWidget()->loadSettings();
        topSplit->addWidget(b);
        m_pans[panIds[1]] = b;

        auto* botSplit = new QSplitter(Qt::Horizontal);
        botSplit->setHandleWidth(3);
        botSplit->setChildrenCollapsible(false);
        m_splitter->addWidget(botSplit);

        auto* c = new PanadapterApplet(botSplit);
        c->setPanId(panIds[2]);
        c->spectrumWidget()->setPanIndex(2);
        c->spectrumWidget()->loadSettings();
        botSplit->addWidget(c);
        m_pans[panIds[2]] = c;

        auto* d = new PanadapterApplet(botSplit);
        d->setPanId(panIds[3]);
        d->spectrumWidget()->setPanIndex(3);
        d->spectrumWidget()->loadSettings();
        botSplit->addWidget(d);
        m_pans[panIds[3]] = d;

        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);

        if (m_activePanId.isEmpty()) setActivePan(panIds[0]);
    }
    else if (layoutId == "3v" && panIds.size() >= 3) {
        // A / B / C — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
        addPanadapter(panIds[2]);
    }
    else if (layoutId == "4v" && panIds.size() >= 4) {
        // A / B / C / D — vertical stack
        addPanadapter(panIds[0]);
        addPanadapter(panIds[1]);
        addPanadapter(panIds[2]);
        addPanadapter(panIds[3]);
    }
}

// ── Float / Dock ──────────────────────────────────────────────────────────

void PanadapterStack::floatPanadapter(const QString& panId)
{
    PanadapterApplet* applet = m_pans.value(panId, nullptr);
    if (!applet || m_floatingWindows.contains(panId)) return;

    // Hide the SpectrumWidget and release GPU resources *before* reparenting.
    // On macOS, QRhiWidget with WA_NativeWindow has a native NSView; orphaning
    // the widget to nullptr creates a transient top-level NSWindow whose
    // destruction can corrupt the main window's NSResponder chain (#1344).
    SpectrumWidget* sw = applet->spectrumWidget();
    sw->hide();
    sw->prepareForTopLevelChange();
    sw->resetGpuResources();

    // Reparent directly from splitter into the floating window — never through
    // nullptr. Using adoptApplet() calls addWidget() internally which sets
    // the parent to the floating window in one step, avoiding the transient
    // top-level NSWindow that corrupts the NSResponder chain (#1668).
    // Parenting the floating window to MainWindow keeps it on top of the
    // main window without becoming WindowStaysOnTopHint (which would
    // float above other apps too).  Qt::Window inside PanFloatingWindow
    // keeps it as a top-level window — the parent only affects z-order
    // and lifetime.
    auto* fw = new PanFloatingWindow(window());
    fw->adoptApplet(applet);
    applet->spectrumWidget()->setFloating(true);

    m_floatingWindows[panId] = fw;
    rebuildDockedSplitter();
    fw->restoreWindowGeometry();
    fw->show();
    fw->raise();
    saveFloatingState();
    emit panFloated(panId);

    connect(fw, &PanFloatingWindow::dockRequested,
            this, &PanadapterStack::dockPanadapter);

    // Defer sw->show() until after refreshAfterReparent() so Metal binds to
    // the new NSView before the first render. Showing before the reset can
    // trigger render() on a stale/transitional Metal surface.
    QTimer::singleShot(0, this, [this, sw]() {
        refreshAfterReparent(sw);
        sw->show();
        equalizeSizes();
    });
}

void PanadapterStack::dockPanadapter(const QString& panId)
{
    PanFloatingWindow* fw = m_floatingWindows.take(panId);
    if (!fw) return;

    fw->saveWindowGeometry();

    // Hide the SpectrumWidget and release GPU resources *before* reparenting.
    // On macOS with WA_NativeWindow, the native NSView must be torn down
    // cleanly while still owned by a valid parent window. If we let
    // takeApplet() orphan to nullptr first, the double NSView lifecycle
    // (destroy → create top-level → destroy → embed) breaks the parent
    // window's NSResponder chain on macOS Tahoe, freezing all input (#1344).
    PanadapterApplet* applet = fw->applet();
    SpectrumWidget* sw = applet ? applet->spectrumWidget() : nullptr;
    if (sw) {
        sw->hide();
        sw->prepareForTopLevelChange();
        sw->resetGpuResources();
    }

    applet = fw->takeApplet();
    fw->hide();
    fw->deleteLater();
    saveFloatingState();

    if (!applet) return;

    applet->setFloatingState(false);
    applet->spectrumWidget()->setFloating(false);
    // Update multi-pan mode on all applets
    bool multi = m_pans.size() > 1;
    for (auto* a : m_pans)
        a->setMultiPanMode(multi);

    // Reparent directly into the splitter — addWidget() calls setParent()
    // internally, so the widget goes straight from the floating window to
    // the splitter without an intermediate top-level state.
    m_splitter->addWidget(applet);
    applet->show();
    rebuildDockedSplitter();

    const int count = m_splitter->count();
    if (count > 1) {
        int total = (m_splitter->orientation() == Qt::Horizontal)
                        ? m_splitter->width() : m_splitter->height();
        int each = total / count;
        QList<int> sizes;
        for (int i = 0; i < count; ++i)
            sizes.append(each);
        m_splitter->setSizes(sizes);
    }

    // Force layout recalculation to prevent blank gaps on macOS
    m_splitter->updateGeometry();
    if (auto* w = window()) {
        if (auto* l = w->layout())
            l->invalidate();
    }

    // Re-show and reinitialize GPU resources after reparenting is complete.
    // Use QTimer::singleShot(0, ...) so Qt processes the reparent events first.
    if (sw) {
        sw->show();
        QTimer::singleShot(0, this, [sw]() {
            refreshAfterReparent(sw);
        });
    }
    emit panDocked(panId);
}

bool PanadapterStack::isFloating(const QString& panId) const
{
    return m_floatingWindows.contains(panId);
}

void PanadapterStack::setFramelessMode(bool on)
{
    for (auto* fw : m_floatingWindows) {
        fw->setFramelessMode(on);
    }
}

void PanadapterStack::setShuttingDown(bool on)
{
    for (auto* fw : m_floatingWindows) {
        fw->setShuttingDown(on);
    }
}

void PanadapterStack::prepareShutdown()
{
    if (m_shutdownPrepared) {
        return;
    }
    m_shutdownPrepared = true;

    setShuttingDown(true);
    saveFloatingState();

    // Explicitly delete floating windows rather than calling close().
    // close() without WA_DeleteOnClose only hides the window, leaving it alive
    // as a Qt::Window child of MainWindow.  When MainWindow's QWidget::~QWidget()
    // later tears down its QRhi, Qt fires the QRhiWidgetPrivate cleanup callbacks
    // for those still-parented-but-hidden floating windows in an order that can
    // invalidate QRhiWidgetPrivate before the callback returns → crash at 0x1e2.
    // Deleting fw explicitly runs QRhiWidget::~QRhiWidget() → removeCleanupCallback
    // while the QRhi is still valid, so no stale callbacks remain when MainWindow
    // tears down (#2495 exit crash on macOS).
    const QList<QString> floatingIds = m_floatingWindows.keys();
    for (const QString& panId : floatingIds) {
        PanFloatingWindow* fw = m_floatingWindows.take(panId);
        if (!fw) continue;
        fw->saveWindowGeometry();
        if (PanadapterApplet* applet = fw->applet()) {
            if (SpectrumWidget* sw = applet->spectrumWidget()) {
                sw->prepareForShutdown();
            }
        }
        m_pans.remove(panId);
        delete fw;
    }

    // Explicitly delete docked applets so ~QRhiWidget() runs (and calls
    // removeCleanupCallback) while MainWindow's QRhi is still alive.
    // If we leave applets alive, Qt's destructor chain for QWidget destroys
    // the QRhi *before* ~QObject()::deleteChildren() deletes the child
    // SpectrumWidgets — QRhi::runCleanup() then fires against QRhiWidgetPrivate
    // objects that are still live but have internal Qt fields in a stale state
    // → crash at $0_cleanup +24 on exit (#2495 macOS).
    const QList<QString> dockedIds = m_pans.keys();
    for (const QString& panId : dockedIds) {
        PanadapterApplet* applet = m_pans.take(panId);
        if (!applet) continue;
        if (SpectrumWidget* sw = applet->spectrumWidget()) {
            sw->prepareForShutdown();
        }
        delete applet;
    }
}

void PanadapterStack::saveFloatingState() const
{
    QStringList ids;
    const QString saved =
        AppSettings::instance().value("FloatingPanIds", "").toString();
    for (const QString& id : saved.split(',', Qt::SkipEmptyParts)) {
        if (!m_seenPanIds.contains(id) && !ids.contains(id)) {
            ids << id;
        }
    }
    for (const QString& id : m_floatingWindows.keys()) {
        if (!ids.contains(id)) {
            ids << id;
        }
    }
    AppSettings::instance().setValue("FloatingPanIds", ids.join(','));
    AppSettings::instance().save();
}

void PanadapterStack::restoreFloatingState()
{
    const QString saved =
        AppSettings::instance().value("FloatingPanIds", "").toString();
    if (saved.isEmpty()) return;
    for (const QString& id : saved.split(',', Qt::SkipEmptyParts)) {
        if (m_pans.contains(id) && !m_floatingWindows.contains(id)) {
            floatPanadapter(id);
        }
    }
}

} // namespace AetherSDR
