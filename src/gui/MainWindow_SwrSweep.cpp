// MainWindow_SwrSweep.cpp — the AetherSweep SWR-sweep engine of MainWindow.
//
// Part of the #3351 monolith decomposition (Phase 1e). Holds the complete
// sweep lifecycle: input locking, plot management, RF stepping, the sweep
// state machine (start / advance / finish / tune-stop handoff), and the
// pan overlay.
//
// Pure code motion from MainWindow.cpp — same class, no header changes.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "PhoneCwApplet.h"
#include "SwrSweepLicenseDialog.h"
#include "VfoWidget.h"
#include "MainWindowHelpers.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumWidget.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "models/BandPlanManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

void MainWindow::setSwrSweepInputsLocked(bool locked)
{
    if (locked) {
        m_swrSweep.appletPanelWasEnabled = !m_appletPanel || m_appletPanel->isEnabled();
        m_swrSweep.panStackWasEnabled = !m_panStack || m_panStack->isEnabled();
        if (m_appletPanel)
            m_appletPanel->setEnabled(false);
        if (m_panStack)
            m_panStack->setEnabled(false);
        setFocus(Qt::OtherFocusReason);
        return;
    }

    if (m_appletPanel)
        m_appletPanel->setEnabled(m_swrSweep.appletPanelWasEnabled);
    if (m_panStack)
        m_panStack->setEnabled(m_swrSweep.panStackWasEnabled);
}

void MainWindow::clearSwrSweepPlot()
{
    if (m_swrSweep.running) {
        m_swrSweep.clearPlotOnFinish = true;
        finishSwrSweep(true, QStringLiteral("SWR sweep cleared"));
        return;
    }

    m_swrSweep.samples.clear();
    m_swrSweep.sourceLabel.clear();
    m_swrSweep.originalBandName.clear();
    m_swrSweep.preserveBandSwitchOnFinish = false;
    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }
    statusBar()->showMessage(QStringLiteral("SWR sweep plot cleared"), 2500);
}

// Export the most recent completed sweep to a CSV (Frequency (Hz),SWR). The
// samples + band name persist in m_swrSweep after a sweep finishes (only an
// explicit clear or a band change drops them), so this is available whenever a
// trace is on the panadapter. Filename defaults to
// swr_sweep_<band>_<YYYYMMDD>_<HHMMSS>.csv. #2241.
void MainWindow::saveSwrSweepCsv()
{
    if (m_swrSweep.running) {
        statusBar()->showMessage(
            QStringLiteral("Wait for the SWR sweep to finish before saving."), 2500);
        return;
    }
    if (m_swrSweep.samples.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("No SWR sweep data to save — run a sweep first."), 2500);
        return;
    }

    // Sanitise the band name for use in a filename (e.g. "20m" stays, but guard
    // against any separators a band label might carry).
    QString band = m_swrSweep.originalBandName;
    band.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                 QStringLiteral("_"));
    if (band.isEmpty())
        band = QStringLiteral("band");

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString suggestedName =
        QStringLiteral("swr_sweep_%1_%2.csv").arg(band, stamp);
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggestedPath =
        dir.isEmpty() ? suggestedName : QDir(dir).filePath(suggestedName);

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save SWR Sweep CSV"), suggestedPath,
        QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;  // user cancelled

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(
            this, QStringLiteral("Save SWR Sweep CSV"),
            QStringLiteral("Could not open file for writing:\n%1").arg(path));
        return;
    }

    QTextStream out(&file);
    out << "Frequency (Hz),SWR\n";
    for (const SwrSweepSample& s : m_swrSweep.samples) {
        const qint64 freqHz = static_cast<qint64>(llround(s.freqMhz * 1.0e6));
        out << freqHz << ',' << QString::number(s.swr, 'f', 3) << '\n';
    }
    file.close();

    statusBar()->showMessage(
        QStringLiteral("Saved %1 SWR points to %2")
            .arg(m_swrSweep.samples.size())
            .arg(QDir::toNativeSeparators(path)),
        4000);
}

void MainWindow::clearSwrSweepForBandChange(int sliceId, const QString& panId,
                                            const QString& newBandName)
{
    if (m_swrSweep.originalBandName.isEmpty()
        || newBandName.isEmpty()
        || newBandName == m_swrSweep.originalBandName) {
        return;
    }

    if (!panId.isEmpty() && !m_swrSweep.panId.isEmpty() && panId != m_swrSweep.panId)
        return;

    if (sliceId >= 0 && m_swrSweep.sliceId >= 0 && sliceId != m_swrSweep.sliceId)
        return;

    if (m_swrSweep.running) {
        m_swrSweep.clearPlotOnFinish = true;
        m_swrSweep.preserveBandSwitchOnFinish = true;
        finishSwrSweep(true, QStringLiteral("SWR sweep disabled on band change"));
        return;
    }

    if (m_swrSweep.samples.isEmpty())
        return;

    m_swrSweep.samples.clear();
    m_swrSweep.sourceLabel.clear();
    m_swrSweep.originalBandName.clear();
    m_swrSweep.preserveBandSwitchOnFinish = false;
    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }
    statusBar()->showMessage(QStringLiteral("SWR sweep cleared on band change"), 2500);
}

void MainWindow::updateSwrSweepOverlay(double currentFreqMhz)
{
    QVector<SpectrumWidget::SwrSweepPoint> points;
    points.reserve(m_swrSweep.samples.size());
    for (const auto& sample : m_swrSweep.samples)
        points.append({sample.freqMhz, sample.swr});

    if (!m_panStack)
        return;

    for (auto* applet : m_panStack->allApplets()) {
        auto* sw = applet ? applet->spectrumWidget() : nullptr;
        if (!sw)
            continue;
        if (applet->panId() == m_swrSweep.panId) {
            sw->setSwrSweepPoints(points, m_swrSweep.running, currentFreqMhz,
                                  m_swrSweep.sourceLabel);
        } else if (m_swrSweep.running) {
            sw->clearSwrSweepPoints();
        }
    }
}

void MainWindow::commandSwrSweepFrequency(double freqMhz, int settleMs)
{
    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    s->setFrequency(freqMhz);
    if (auto* sw = spectrumForSlice(s))
        sw->setSliceOverlayFreq(s->sliceId(), freqMhz);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_swrSweep.commandIssuedAtMs = now;
    m_swrSweep.sampleNotBeforeMs = now + settleMs;
    updateSwrSweepOverlay(freqMhz);
}

void MainWindow::beginSwrSweepRf()
{
    if (!m_swrSweep.running)
        return;

    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    if (m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl) {
        const auto& tuner = m_radioModel.tunerModel();
        if (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL bypass was not confirmed"));
            return;
        }
    }

    const QString bandName = BandSettings::bandForFrequency(m_swrSweep.originalFreqMhz);
    const BandDef& band = BandSettings::bandDef(bandName);
    const double displayCenter = (band.lowMhz + band.highMhz) * 0.5;
    const double displayBw = (band.highMhz - band.lowMhz) + 2.0 * kSwrSweepPanPaddingMhz;

    m_swrSweep.phase = SwrSweepPhase::Sweeping;
    m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    applyPanRangeRequest(m_swrSweep.panId, displayCenter, displayBw, "swr-sweep");

    commandSwrSweepFrequency(m_swrSweep.frequencies.first(), kSwrSweepInitialSettleMs);
    m_radioModel.transmitModel().startTune();
    m_swrSweep.tuneStarted = true;
    m_swrSweepTimer.start(kSwrSweepPollMs);

    statusBar()->showMessage(
        tr("SWR sweep running on %1 with Tune Power %2 W%3. Press Esc to stop.")
            .arg(QString::fromLatin1(band.name))
            .arg(m_swrSweep.sweepTunePower)
            .arg(m_swrSweep.sourceLabel.isEmpty()
                     ? QString()
                     : QStringLiteral(" (%1)").arg(m_swrSweep.sourceLabel)),
        5000);
}

void MainWindow::startSwrSweep(int requestedSliceId, int sweepPowerWatts)
{
    if (m_swrSweep.running)
        return;

    if (!m_radioModel.isConnected()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Connect to a radio before starting an SWR sweep."));
        return;
    }
    if (m_splitActive) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Disable split before running an SWR sweep."));
        return;
    }

    auto* s = swrSweepTargetSlice(requestedSliceId);
    if (!s || !s->isTxSlice()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Select the TX slice before running an SWR sweep."));
        return;
    }
    if (s->isLocked()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Unlock the TX slice before running an SWR sweep."));
        return;
    }
    if (s->panId().isEmpty() || !spectrumForSlice(s)) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("The TX slice needs a visible panadapter before running an SWR sweep."));
        return;
    }

    auto& tx = m_radioModel.transmitModel();
    if (tx.isTuning() || tx.isMox() || tx.isTransmitting()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Stop transmit or tune before starting an SWR sweep."));
        return;
    }
    if (m_radioModel.hasAmplifier() && m_radioModel.ampOperate()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("Put the Power Genius XL amplifier in STANDBY before running an SWR sweep."));
        return;
    }

    const QString bandName = BandSettings::bandForFrequency(s->frequency());
    const BandDef& band = BandSettings::bandDef(bandName);
    if (bandName == QLatin1String("GEN") || band.lowMhz <= 0.0 || band.highMhz <= band.lowMhz) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("The current TX frequency is not inside a supported amateur band."));
        return;
    }
    if (bandName == QLatin1String("60m")) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("SWR sweep is disabled on 60 m because the band is channelized."));
        return;
    }

    // Narrow the sweep range to the active regional band plan when one is
    // loaded.  BandDefs.h holds ARRL/US allocations only; without this the
    // sweep transmits outside the user's region (e.g. past 7.200 MHz on
    // 40 m for IARU R1) and trips the radio's interlock.  Mirrors the
    // pattern used by AtuPreTuneDialog::recomputeBands. (#2800)
    //
    // Today the SWR sweep treats the band as a single contiguous range —
    // discrete-channel bands like US 60 m are hard-blocked above. We use
    // contiguousRegionsForBand() (#2822) and union the regions so a
    // future enhancement can walk each region individually without
    // touching the per-region calculation.
    double effectiveLow = band.lowMhz;
    double effectiveHigh = band.highMhz;
    if (m_bandPlanMgr) {
        const auto regions =
            m_bandPlanMgr->contiguousRegionsForBand(band.lowMhz, band.highMhz);
        if (!regions.isEmpty()) {
            effectiveLow = std::max(effectiveLow, regions.first().lowMhz);
            effectiveHigh = std::min(effectiveHigh, regions.last().highMhz);
        }
    }

    const double safeLow = effectiveLow + kSwrSweepEdgeGuardMhz;
    const double safeHigh = effectiveHigh - kSwrSweepEdgeGuardMhz;
    if (safeHigh <= safeLow) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band is too narrow for the configured SWR sweep guard."));
        return;
    }

    const double displayBw = (band.highMhz - band.lowMhz) + 2.0 * kSwrSweepPanPaddingMhz;
    if (displayBw > m_radioModel.maxPanBandwidthMhz()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band is wider than the radio can display in one panadapter."));
        return;
    }

    QVector<double> sweepFreqs;
    for (double f = safeLow; f <= safeHigh + 1.0e-9; f += kSwrSweepStepMhz)
        sweepFreqs.append(std::round(f * 1.0e6) / 1.0e6);
    if (sweepFreqs.isEmpty()) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("No in-band sweep points were available."));
        return;
    }
    if (sweepFreqs.size() > kSwrSweepMaxPoints) {
        QMessageBox::warning(this, tr("SWR Sweep"),
                             tr("This band would need too many sweep points for one fast pass."));
        return;
    }

    // License gate.  First-press shows a modal disclaimer; subsequent
    // presses are silent once the user ticks "Remember my answer" and
    // accepts.  Placed after all preconditions clear so the dialog only
    // fires when an actual transmission would follow.
    if (!SwrSweepLicenseDialog::confirm(this)) {
        return;
    }

    m_swrSweep = SwrSweepState{};
    m_swrSweep.running = true;
    m_swrSweep.sliceId = s->sliceId();
    m_swrSweep.panId = s->panId();
    m_swrSweep.originalFreqMhz = s->frequency();
    m_swrSweep.originalBandName = bandName;
    m_swrSweep.frequencies = sweepFreqs;
    m_swrSweep.currentIndex = 0;
    m_swrSweep.originalTunePower = tx.tunePower();
    m_swrSweep.sweepTunePower = sweepPowerWatts;
    m_swrSweep.minimumForwardPowerW = qBound(0.05f,
                                             static_cast<float>(sweepPowerWatts) * 0.05f,
                                             1.0f);
    auto& tuner = m_radioModel.tunerModel();
    m_swrSweep.tgxlOriginalOperate = tuner.isOperate();
    m_swrSweep.tgxlOriginalBypass = tuner.isBypass();
    if (tuner.isPresent() && tuner.isOperate()) {
        m_swrSweep.sourceLabel = QStringLiteral("TGXL BYPASS");
        // Read radio-side SWR even when TGXL is bypassed: in bypass the TGXL
        // is a passive wire-through and stops emitting RL meter packets, so
        // tgxlSwrUpdatedAtMs never advances.  The radio's SWR coupler sees
        // the antenna directly through the bypassed relays — equivalent
        // reading, reliably emitted during the tune carrier.  (#2229)
        m_swrSweep.meterSource = SwrSweepMeterSource::Radio;
        if (!tuner.isBypass()) {
            m_swrSweep.tgxlBypassRequested = true;
            m_swrSweep.tgxlRestoreNeeded = true;
        }
    } else {
        m_swrSweep.sourceLabel = tuner.isPresent()
            ? QStringLiteral("RADIO")
            : QString();
    }

    if (auto* sw = spectrumForSlice(s)) {
        m_swrSweep.originalPanCenterMhz = sw->centerMhz();
        m_swrSweep.originalPanBandwidthMhz = sw->bandwidthMhz();
    }

    for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
        if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
            sw->clearSwrSweepPoints();
    }

    setSwrSweepInputsLocked(true);
    tx.setTunePower(sweepPowerWatts);

    if (m_swrSweep.tgxlBypassRequested) {
        m_swrSweep.phase = SwrSweepPhase::WaitingForTgxlBypass;
        m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        tuner.setBypass(true);
        m_swrSweepTimer.start(kSwrSweepPollMs);
        statusBar()->showMessage(
            tr("Preparing SWR sweep: placing TGXL in BYPASS before applying RF..."),
            5000);
        updateSwrSweepOverlay(-1.0);
        return;
    }

    beginSwrSweepRf();
}

void MainWindow::advanceSwrSweep()
{
    if (!m_swrSweep.running)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto& tuner = m_radioModel.tunerModel();

    if (m_swrSweep.phase == SwrSweepPhase::WaitingForTgxlBypass) {
        if (!tuner.isPresent() || !tuner.isOperate()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL is no longer available"));
            return;
        }
        if (tuner.isBypass()) {
            m_swrSweep.phase = SwrSweepPhase::TgxlBypassSettle;
            m_swrSweep.phaseStartedAtMs = now;
            statusBar()->showMessage(
                tr("TGXL BYPASS confirmed. Waiting for relays to settle..."),
                2500);
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlBypassTimeoutMs) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL did not enter bypass"));
        }
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::TgxlBypassSettle) {
        if (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass()) {
            finishSwrSweep(true,
                           QStringLiteral("SWR sweep stopped: TGXL left bypass"));
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlRelaySettleMs)
            beginSwrSweepRf();
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::StoppingTune) {
        const bool waitedMinimum = now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTuneStopWaitMs;
        const bool stopped = !m_radioModel.transmitModel().isTuning();
        const bool timedOut = now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTuneStopTimeoutMs;
        if (!waitedMinimum || (!stopped && !timedOut))
            return;

        finishSwrSweepAfterTuneStopped();
        return;
    }

    if (m_swrSweep.phase == SwrSweepPhase::RestoringTgxl) {
        const bool restored = !tuner.isPresent()
            || tuner.isBypass() == m_swrSweep.tgxlOriginalBypass;
        if (restored) {
            completeSwrSweepFinish();
            return;
        }
        if (now - m_swrSweep.phaseStartedAtMs >= kSwrSweepTgxlRestoreTimeoutMs) {
            m_swrSweep.tgxlRestoreTimedOut = true;
            completeSwrSweepFinish();
        }
        return;
    }

    auto* s = m_radioModel.slice(m_swrSweep.sliceId);
    if (!s) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: slice closed"));
        return;
    }

    if (m_swrSweep.phase != SwrSweepPhase::Sweeping)
        return;

    if (m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl
        && (!tuner.isPresent() || !tuner.isOperate() || !tuner.isBypass())) {
        finishSwrSweep(true, QStringLiteral("SWR sweep stopped: TGXL left bypass"));
        return;
    }

    if (now < m_swrSweep.sampleNotBeforeMs)
        return;

    const auto& meters = m_radioModel.meterModel();
    const bool useTgxlMeters = m_swrSweep.meterSource == SwrSweepMeterSource::Tgxl;
    const bool swrFresh = useTgxlMeters
        ? meters.tgxlSwrUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs
        : meters.swrUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs;
    const bool fwdFresh = useTgxlMeters
        ? meters.tgxlFwdPowerUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs
        : meters.fwdPowerUpdatedAtMs() >= m_swrSweep.sampleNotBeforeMs;
    const bool hasForwardPower =
        (useTgxlMeters ? meters.tgxlFwdPower() : meters.fwdPowerInstant())
            >= m_swrSweep.minimumForwardPowerW;
    if (!swrFresh || !fwdFresh || !hasForwardPower) {
        if (now - m_swrSweep.commandIssuedAtMs < kSwrSweepMaxSettleMs)
            return;

        if (!swrFresh) {
            finishSwrSweep(true,
                           useTgxlMeters
                               ? QStringLiteral("SWR sweep stopped: no fresh TGXL SWR meter data")
                               : QStringLiteral("SWR sweep stopped: no fresh SWR meter data"));
        } else {
            finishSwrSweep(true,
                           useTgxlMeters
                               ? QStringLiteral("SWR sweep stopped: no TGXL forward power detected")
                               : QStringLiteral("SWR sweep stopped: no forward power detected"));
        }
        return;
    }

    if (m_swrSweep.currentIndex < 0
        || m_swrSweep.currentIndex >= m_swrSweep.frequencies.size()) {
        finishSwrSweep(false, QStringLiteral("SWR sweep complete"));
        return;
    }

    float swr = useTgxlMeters ? meters.tgxlSwr() : meters.swr();
    if (!std::isfinite(static_cast<double>(swr)) || swr < 1.0f)
        swr = 1.0f;

    const double sampleFreq = m_swrSweep.frequencies[m_swrSweep.currentIndex];
    m_swrSweep.samples.append({sampleFreq, swr});
    updateSwrSweepOverlay(sampleFreq);

    ++m_swrSweep.currentIndex;
    if (m_swrSweep.currentIndex >= m_swrSweep.frequencies.size()) {
        finishSwrSweep(false, QStringLiteral("SWR sweep complete"));
        return;
    }

    commandSwrSweepFrequency(m_swrSweep.frequencies[m_swrSweep.currentIndex],
                             kSwrSweepStepSettleMs);
}

void MainWindow::finishSwrSweep(bool aborted, const QString& reason)
{
    if (!m_swrSweep.running)
        return;

    if (!reason.isEmpty())
        m_swrSweep.finalReason = reason;
    m_swrSweep.finalAborted = m_swrSweep.finalAborted || aborted;

    if (m_swrSweep.phase == SwrSweepPhase::StoppingTune
        || m_swrSweep.phase == SwrSweepPhase::RestoringTgxl) {
        return;
    }

    if (m_radioModel.isConnected() && m_swrSweep.tuneStarted) {
        m_radioModel.transmitModel().stopTune();
        m_swrSweep.phase = SwrSweepPhase::StoppingTune;
        m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_swrSweepTimer.start(kSwrSweepPollMs);
        statusBar()->showMessage(tr("Stopping SWR sweep tune carrier..."), 2500);
        return;
    }

    finishSwrSweepAfterTuneStopped();
}

void MainWindow::finishSwrSweepAfterTuneStopped()
{
    if (!m_swrSweep.running)
        return;

    if (m_radioModel.isConnected()) {
        if (!m_swrSweep.preserveBandSwitchOnFinish) {
            if (auto* s = m_radioModel.slice(m_swrSweep.sliceId);
                s && m_swrSweep.originalFreqMhz > 0.0) {
                s->setFrequency(m_swrSweep.originalFreqMhz);
            }
        }
        if (m_swrSweep.originalTunePower != m_swrSweep.sweepTunePower)
            m_radioModel.transmitModel().setTunePower(m_swrSweep.originalTunePower);

        if (!m_swrSweep.preserveBandSwitchOnFinish
            && m_swrSweep.finalAborted && !m_swrSweep.panId.isEmpty()
            && m_swrSweep.originalPanCenterMhz > 0.0
            && m_swrSweep.originalPanBandwidthMhz > 0.0) {
            applyPanRangeRequest(m_swrSweep.panId,
                                 m_swrSweep.originalPanCenterMhz,
                                 m_swrSweep.originalPanBandwidthMhz,
                                 "swr-sweep-stop");
        }

        auto& tuner = m_radioModel.tunerModel();
        if (m_swrSweep.tgxlRestoreNeeded
            && tuner.isPresent()
            && tuner.isOperate()
            && (tuner.isBypass() != m_swrSweep.tgxlOriginalBypass
                || m_swrSweep.tgxlBypassRequested)) {
            tuner.setBypass(m_swrSweep.tgxlOriginalBypass);
            m_swrSweep.phase = SwrSweepPhase::RestoringTgxl;
            m_swrSweep.phaseStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_swrSweepTimer.start(kSwrSweepPollMs);
            statusBar()->showMessage(tr("Restoring TGXL tuner state..."), 3000);
            return;
        }
    }

    completeSwrSweepFinish();
}

void MainWindow::completeSwrSweepFinish()
{
    if (!m_swrSweep.running)
        return;

    m_swrSweepTimer.stop();

    const bool clearPlot = m_swrSweep.clearPlotOnFinish;
    const bool aborted = m_swrSweep.finalAborted;
    QString msg = m_swrSweep.finalReason.isEmpty()
        ? (aborted ? QStringLiteral("SWR sweep stopped")
                   : QStringLiteral("SWR sweep complete"))
        : m_swrSweep.finalReason;
    if (m_swrSweep.tgxlRestoreTimedOut)
        msg += QStringLiteral("; TGXL restore was not confirmed");

    m_swrSweep.running = false;
    m_swrSweep.phase = SwrSweepPhase::Idle;

    if (clearPlot) {
        m_swrSweep.samples.clear();
        m_swrSweep.sourceLabel.clear();
        m_swrSweep.originalBandName.clear();
        m_swrSweep.preserveBandSwitchOnFinish = false;
        for (auto* applet : m_panStack ? m_panStack->allApplets() : QList<PanadapterApplet*>{}) {
            if (auto* sw = applet ? applet->spectrumWidget() : nullptr)
                sw->clearSwrSweepPoints();
        }
        msg = QStringLiteral("SWR sweep plot cleared");
    } else {
        updateSwrSweepOverlay(-1.0);
    }

    setSwrSweepInputsLocked(false);

    statusBar()->showMessage(msg, aborted ? 3000 : 5000);
}


} // namespace AetherSDR
