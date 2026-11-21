// Lock view's MainWindow call sites, read from the source. The decisions are
// run by pan_view_lock_policy_test and the widget's gestures by
// pan_view_lock_widget_test; what neither can reach is MainWindow, which no
// test in the default graph constructs. Each pin below is a guard or an
// ordering inside a MainWindow method, matched with whitespace collapsed.

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// A source file with `//` comments dropped (so commented-out code cannot meet
// a pin) and every whitespace run collapsed to one space.
QByteArray code(const char* relative)
{
    QFile f(QStringLiteral(AETHER_SOURCE_DIR "/") + QString::fromLatin1(relative));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QList<QByteArray> lines = f.readAll().split('\n');
    for (QByteArray& line : lines) {
        const qsizetype at = line.indexOf("//");
        if (at >= 0) {
            line.truncate(at);
        }
    }
    return lines.join('\n').simplified();
}

// The body of a top-level definition, from its signature to the next one.
QByteArray body(const QByteArray& src, const char* signature, const char* next)
{
    const qsizetype start = src.indexOf(signature);
    const qsizetype end = start < 0 ? -1 : src.indexOf(next, start + 1);
    return end < 0 ? QByteArray() : src.mid(start, end - start);
}

bool ordered(const QByteArray& src, const char* first, const char* second)
{
    const qsizetype a = src.indexOf(first);
    return a >= 0 && src.indexOf(second, a) > a;
}

} // namespace

int main()
{
    const QByteArray wiring = code("src/gui/MainWindow_Wiring.cpp");
    const QByteArray mainWindow = code("src/gui/MainWindow.cpp");
    const QByteArray session = code("src/gui/MainWindow_Session.cpp");
    const QByteArray shortcuts = code("src/gui/MainWindow_Shortcuts.cpp");
    const QByteArray actions = code("src/gui/MainWindow_ControlActions.cpp");
    check(!wiring.isEmpty() && !mainWindow.isEmpty() && !session.isEmpty()
              && !shortcuts.isEmpty() && !actions.isEmpty(),
          "read the MainWindow sources");

    // ---- tune-driven recenters ----

    const QByteArray reveal = body(
        wiring, "MainWindow::TuneCenteringResult MainWindow::revealFrequencyIfNeeded(",
        "MainWindow::TuneCenteringResult MainWindow::panFollowVfo(");
    check(ordered(reveal, "if (!viewLockAllowsRecenter(slice, mhz, intent)) { return result; }",
                  "if (intent == TuneIntent::CommandedTargetCenter)"),
          "reveal/follow consults the lock before any hard-center");
    check(mainWindow.contains(
              "const bool viewMayMove = viewLockAllowsRecenter(slice, mhz, intent); "
              "if (viewMayMove && (intent == TuneIntent::CommandedTargetCenter "
              "|| centerLockActiveForSlice(slice))) {")
              && mainWindow.contains("} else if (viewMayMove && (intent != TuneIntent::IncrementalTune "
                                     "|| panFollowEnabled())) {"),
          "a confirmed-receive tune asks the radio to preserve a view the lock holds");
    check(wiring.contains("&& !panViewLocked(pan->panId())) {"),
          "the edge-pan drag handler never writes a new center for a locked pan");
    check(wiring.contains("return sw && sw->viewLocked() && sw->frequencyInView(mhz);"),
          "lockedViewShows is true only for a locked pan that shows the target");
    check(wiring.contains("if (!lockedViewShows(panId, freqMhz)) { "
                          "m_radioModel.requestPanCenter(panId, freqMhz); }"),
          "a non-Flex band button tunes in place when a locked view shows it");
    check(mainWindow.contains("memoryBand != currentBand && lockedViewShows(slicePanId, memoryFreqMhz)"),
          "a cross-band memory already on a locked view skips the band-stack recall");
    check(mainWindow.contains("|| lockedViewShows(slice->panId(), mhz)) {"),
          "a cross-band typed tune already on a locked view skips the band-stack recall");

    // ---- operator range commands ----

    check(ordered(shortcuts, "if (panSpanFixedByBackend() && !kiwiSdrPanDisplaysKiwi(s->panId())) { return; }",
                  "if (refuseViewLockedRangeChange(s->panId())) { return; }"),
          "the zoom shortcut is refused while locked, after the silent fixed-span return");
    check(shortcuts.contains("if (refuseViewLockedRangeChange(s->panId())) return true;"),
          "a span-preset action is refused while locked, with the lock's own notice");
    check(shortcuts.contains("if (refusal != PanZoomModeRefusal::None || refuseViewLockedRangeChange(panId)) {")
              && shortcuts.contains("|| refuseViewLockedRangeChange(panId)) { return; }"),
          "band and segment zoom are refused while locked on every command path");

    // ---- exclusivity with Center Lock ----

    const QByteArray setForPan = body(
        wiring, "void MainWindow::setViewLockForPan(const QString& panId, bool locked, bool persist)",
        "double MainWindow::viewLockHoldFraction(");
    check(ordered(setForPan, "m_centerLockSessionOnlyPans.remove(panId);",
                  "clearCenterLockForPan(panId, saves);"),
          "an operator lock drops a TCI session mark first, or the saved Center Lock survives");
    const QByteArray setCenterLock = body(
        wiring, "void MainWindow::setCenterLockForPan(const QString& panId, int sliceId, bool on,",
        "void MainWindow::clearCenterLockForPan(");
    check(ordered(setCenterLock, "setViewLockForPan(panId, false, persist);",
                  "m_centerLockSliceByPan.insert(panId, sliceId);"),
          "engaging Center Lock releases the view lock on that pan");

    // ---- persistence and availability ----

    check(setForPan.contains("if (locked && viewLockHoldFraction(panId) <= 0.0) { "
                             "sw->setViewLocked(false); return; }"),
          "a lock the radio cannot hold is refused without touching the saved intent");
    check(setForPan.contains("const bool saves = persist && m_radioModel.isConnected();")
              && setForPan.contains("if (!saves) { "
                                    "m_pendingViewLockByPanIndex.insert(sw->panIndex(), locked); "
                                    "return; }"),
          "a lock or an unlock made while disconnected waits for the next radio, saving nothing");
    const QByteArray restore = body(
        wiring, "void MainWindow::restoreViewLockForPan(const QString& panId)",
        "bool MainWindow::snapCenterLockForSlice(");
    check(restore.contains("if (pending != m_pendingViewLockByPanIndex.cend() "
                           "&& (!pending.value() || viewLockHoldFraction(panId) > 0.0)) { "
                           "setViewLockForPan(panId, pending.value(), /*persist=*/true); return; }"),
          "connecting saves that choice: an unlock at once, a lock once the radio can hold");
    const QByteArray holdFraction = body(
        wiring, "double MainWindow::viewLockHoldFraction(const QString& panId) const",
        "void MainWindow::syncViewLockAvailability(");
    check(holdFraction.contains("if (!m_radioModel.isConnected()) { return 1.0; }")
              && holdFraction.contains("if (!hold || (pan && !pan->viewHolds())) { return 0.0; }"),
          "the hold is permissive on disconnect, else the record and the pan's live state");
    const QByteArray sync = body(
        wiring, "void MainWindow::syncViewLockAvailability(const QString& panId)",
        "void MainWindow::onViewLockCapabilitiesChanged(");
    check(sync.contains("if (!supported && sw->viewLocked()) { "
                        "setViewLockForPan(panId, false, /*persist=*/false); } "
                        "else if (supported && !wasAvailable) { restoreViewLockForPan(panId); }"),
          "losing the hold releases the live lock only; regaining it restores the saved one");
    const QByteArray capsChanged = body(
        wiring, "void MainWindow::onViewLockCapabilitiesChanged(bool connected)",
        "void MainWindow::restoreViewLockForPan(");
    check(capsChanged.contains("} else if (m_viewLockPendingSawConnection) { "
                               "m_viewLockPendingSawConnection = false; "
                               "m_pendingViewLockByPanIndex.clear(); }")
              && capsChanged.contains("syncViewLockAvailability(applet->panId());"),
          "each capability publication re-syncs every pan and ends an unapplied pending choice");
    check(session.contains("&RadioModel::capabilitiesChanged, this, "
                           "&MainWindow::onViewLockCapabilitiesChanged);")
              && session.contains("connect(pan, &PanadapterModel::viewHoldsChanged, this, "
                                  "[this, panId = pan->panId()] { syncViewLockAvailability(panId); });"),
          "capability and per-pan hold changes both reach the lock button");
    check(session.contains("syncViewLockAvailability(pan->panId()); restoreViewLockForPan(pan->panId());"),
          "a pan reclaimed on reconnect gets its availability and lock too");

    // ---- closing a pan forgets its lock ----

    check(wiring.contains("clearCenterLockForPan(panId, true); "
                          "setViewLockForPan(panId, false, /*persist=*/true);"),
          "closing a pan with its X drops its saved lock");
    check(actions.contains("clearCenterLockForPan(panId, true); "
                           "setViewLockForPan(panId, false, /*persist=*/true); "
                           "m_radioModel.removePanadapter(panId);"),
          "the Close Active Panadapter action drops it too");
    check(mainWindow.contains("qDebug() << \"applyPanLayout: closing pan\" << panId; "
                              "setViewLockForPan(panId, false, /*persist=*/true);"),
          "shrinking the layout drops the closed pans' saved locks");

    std::printf("%s: %d failure(s)\n", "pan_view_lock_wiring_test", g_failures);
    return g_failures == 0 ? 0 : 1;
}
