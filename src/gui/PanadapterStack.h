#pragma once

#include <QWidget>
#include <QMap>
#include <QSet>
#include <QSplitter>
#include <QStringList>

class QTimer;

namespace AetherSDR {

class BandStackPanel;
class PanFloatingWindow;
class PanadapterApplet;
class PanadapterRenderScheduler;
class SpectrumWidget;

// Vertical stack of N PanadapterApplet instances, each showing an
// independent FFT + waterfall for a different panadapter on the radio.
// Single-pan mode: one applet fills the stack (no visible divider).
class PanadapterStack : public QWidget {
    Q_OBJECT

public:
    explicit PanadapterStack(QWidget* parent = nullptr);

    // Add/remove panadapter displays
    PanadapterApplet* addPanadapter(const QString& panId);
    void removePanadapter(const QString& panId);
    void removeAll();  // remove all applets and reset splitter
    void rekey(const QString& oldId, const QString& newId);

    // Layout: rebuild splitter structure for a given layout ID
    // layoutId: "1", "2v", "2h", "2h1", "12h", "2x2"
    // panIds: the pan IDs to place in order (A, B, C, D)
    void applyLayout(const QString& layoutId, const QStringList& panIds);

    // Accessors
    PanadapterApplet* panadapter(const QString& panId) const;
    SpectrumWidget* spectrum(const QString& panId) const;
    int count() const { return m_pans.size(); }
    QList<PanadapterApplet*> allApplets() const { return m_pans.values(); }
    QStringList panIds() const { return m_pans.keys(); }

    // Active pan (determines which pan the applet column shows controls for)
    QString activePanId() const { return m_activePanId; }
    PanadapterApplet* activeApplet() const;
    SpectrumWidget* activeSpectrum() const;
    void setActivePan(const QString& panId);
    void setSplitterOrientation(Qt::Orientation o) { m_splitter->setOrientation(o); }
    BandStackPanel* bandStackPanel() const { return m_bandStackPanel; }
    void setBandStackVisible(bool visible);
    // Re-home the band-stack panel after workspace-canvas hosting (#4887
    // phase 4): back to slot 0 of the stack's own layout, left of the
    // splitter, exactly where the constructor put it.
    void reclaimBandStackPanel();
    void equalizeSizes();
    void rearrangeLayout(const QString& layoutId);

    // Re-assert every panadapter's GPU surface after the stack has been MOVED
    // within its own window — the applet-panel dock-side flip, which slides
    // the stack sideways without resizing it.
    //
    // The spectrum widgets are native children (WA_NativeWindow) with their
    // own swapchains.  A sideways move lays out through a transient geometry,
    // and the swapchain can end up sized for that transient rather than the
    // final width; the widget then keeps presenting a too-narrow drawable and
    // the strip it moved into is never painted by anyone.  Under
    // MainWindow's WA_TranslucentBackground that strip is see-through, not
    // merely blank.  Continuous rendering does NOT heal it — the pan redraws
    // every frame into the same stale drawable.
    //
    // This is the same hazard prepareForTopLevelChange()/refreshAfterReparent()
    // handle for reparenting between windows, minus the native-window
    // teardown, which a same-window move does not need.
    void refreshAfterLayoutShift();

    // Automation bridge hook: drive rearrangeLayout directly (or, with an empty
    // id, just report) so tests can exercise the splitter reparent path without
    // the radio granting extra panadapters. Rejects unknown ids (error map) and
    // reports fellBack/effectiveLayout when the id needed more applets than
    // exist. Returns saved layout id + counts; geometry settles next turn.
    Q_INVOKABLE QVariantMap automationRearrange(const QString& layoutId);
    // Minimum applets a layout id needs before its rearrangeLayout branch
    // takes it (-1 = unknown id). Mirrors the >= guards — keep in sync.
    static int layoutRequiredPanCount(const QString& layoutId);

    // Automation bridge hook (#4864): drive floatPanadapter()/dockPanadapter()
    // headlessly, so the reparent + GPU re-initialize path — the #2495/#4319/
    // #4617 crash lineage — is testable without a live multi-pan radio and a
    // hidden-button workaround. Rejects unknown pan ids (error map); float on
    // a floating pan / dock on a docked one is reported as alreadyThere
    // rather than an error, because the caller asked for a state that holds.
    // Returns the pan's floating state plus floating/docked counts.
    Q_INVOKABLE QVariantMap automationFloatDock(const QString& action,
                                                const QString& panId);

    // Workspace-canvas seam (RFC #4887 phase 4).  The stack stays the pans'
    // OWNER — creation, wiring, render scheduling, float/dock — and lends
    // applets to the canvas.  detachForCanvas() only decides WHETHER (a
    // floating pan stays out; pop-out is its own state and dockPanadapter()
    // is its way back); the canvas reparents the applet itself, a plain
    // same-top-level move exactly like applyLayout()'s splitter shuffles —
    // no GPU dance, which is reserved for real top-level changes (#2495).
    // returnFromCanvas() re-homes ONE applet into the docked splitter
    // (addWidget's one-step reparent — never through nullptr, #1344);
    // callers rebuild the arrangement afterwards via rearrangeLayout().
    PanadapterApplet* detachForCanvas(const QString& panId);
    void returnFromCanvas(const QString& panId, PanadapterApplet* applet);

    // Bracket a canvas-driven reparent of this pan's widget ACROSS TOP
    // LEVELS (RFC #4887 phase 7: additional canvas windows).  Same GPU
    // recipe as floatPanadapter()/dockPanadapter() — hide + release the
    // QRhi resources BEFORE the move, deferred refresh + show after —
    // because a QRhiWidget crossing a top-level boundary without it is
    // the #2495/#4617/#4319/#1344 crash lineage.  The move itself (a
    // one-step reparent) is the caller's; these only own the GPU side.
    void preparePanForTopLevelMove(const QString& panId);
    void finishPanTopLevelMove(const QString& panId);

    // Float/dock panadapters
    void floatPanadapter(const QString& panId);
    void dockPanadapter(const QString& panId);
    bool isFloating(const QString& panId) const;

    // Follow the main-window frameless setting for all active floating windows.
    void setFramelessMode(bool on);
    void setShuttingDown(bool on);

    // Persist / restore which pans are currently floating (AppSettings key
    // "FloatingPanIds").  saveFloatingState is called automatically on every
    // float/dock transition and at shutdown; restoreFloatingState is called
    // once after all pans have been added following a radio connect.
    void saveFloatingState() const;
    void restoreFloatingState();

    // How long a freshly floated panadapter must stay alive before the
    // crash-loop marker is retired (#4617). The float path's failure mode is
    // immediate — the reparent, the GPU re-initialize and the first frame in
    // the new window all land within one event-loop turn — so this only has to
    // outlast that turn plus the deferred refreshAfterReparent().
    static constexpr int kFloatingRestoreSettleMs = 5000;

    void prepareShutdown();

signals:
    void activePanChanged(const QString& panId);
    void panFloated(const QString& panId);
    void panDocked(const QString& panId);
    // Pan lifecycle, for the workspace controller (RFC #4887 phase 4).
    // panAdded fires once per applet however it was created (addPanadapter
    // or an applyLayout branch); panRemoved fires after the applet is
    // unregistered and BEFORE it is destroyed, so a canvas can release its
    // entry rather than relying on the destroyed-watch; panRekeyed follows
    // the FLEX band-recall id swap (same applet, new id).
    void panAdded(const QString& panId);
    void panRemoved(const QString& panId);
    void panRekeyed(const QString& oldId, const QString& newId);
    // The previous session died while floating panadapters, so they were
    // dropped and this one came up docked. Carries how many were dropped —
    // enough for plural agreement in the operator notice, and nothing more:
    // the internal 0x4000… pan IDs appear nowhere else in the UI, so they
    // would give the operator no action they could take.
    void floatingRestoreAbandoned(int abandonedPanCount);

private:
    void rebuildDockedSplitter();
    // Arm / retire the persisted "a float is in flight" marker (#4617).
    void armFloatingRestoreMarker();
    void clearFloatingRestoreMarker();
    void announceAbandonedFloatingRestore();

    PanadapterRenderScheduler* m_renderScheduler{nullptr};
    BandStackPanel* m_bandStackPanel{nullptr};
    QSplitter* m_splitter{nullptr};
    QMap<QString, PanadapterApplet*> m_pans;
    QMap<QString, PanFloatingWindow*> m_floatingWindows;
    // Preserve floating state for restored pans that were unavailable this run.
    QSet<QString> m_seenPanIds;
    // Applets currently ON LOAN to the workspace canvas (RFC #4887 phase 4).
    // THE INVARIANT EVERY REBUILD PATH MUST HONOR: an applet in this set is
    // not the stack's to arrange.  rebuildDockedSplitter() and
    // rearrangeLayout() both enumerate m_pans and reparent what they find —
    // five call sites reach them (float, dock, the connect-time layout
    // restore, pan close, the layout dialog), and before this set existed
    // every one of them silently reclaimed canvas-hosted applets into the
    // hidden stack: the canvas kept a healthy model while the widget on
    // screen answered to a splitter, which surfaced as "the pan won't snap,
    // won't stack, won't restore" (the 8600 field report).  detachForCanvas
    // lends, returnFromCanvas/floatPanadapter/removePanadapter collect.
    QSet<QString> m_lentToCanvas;
    QString m_activePanId;
    bool m_shutdownPrepared{false};
    // applyLayout() creates applets in its branches (some via addPanadapter,
    // some inline); panAdded is suppressed during it and swept at its end so
    // each applet is announced exactly once.
    bool m_inApplyLayout{false};
    // How many pans the constructor discarded because the previous process
    // never survived floating them. Announced on a zero-timer at launch (once
    // wirePanLifecycle() has connected to us) and again from
    // restoreFloatingState(), which consumes it.
    int m_floatingRestoreAbandonedCount{0};
    QTimer* m_floatingRestoreSettleTimer{nullptr};
};

} // namespace AetherSDR
