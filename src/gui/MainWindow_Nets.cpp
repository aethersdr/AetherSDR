// Net Reminder Scheduler wiring for MainWindow (#3351-style sibling TU).
//
// Owns the operator's saved net schedule: loads/persists it as client-side JSON
// (NOT radio memory slots — nets are operator-scoped per Constitution XIII and
// must work with no radio connected), drives the single recompute-and-rearm
// reminder timer, and surfaces reminders through an in-app actionable banner
// plus a best-effort OS tray notification. "Tune Now" reuses the same
// MemoryRecallPolicy command builders as a memory recall.

#include "MainWindow.h"
#include "MainWindowHelpers.h"

#include "NetReminderBanner.h"
#include "NetSchedulerDialog.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/MemoryRecallPolicy.h"
#include "core/NetScheduleStore.h"
#include "core/NetScheduler.h"
#include "models/BandSettings.h"
#include "models/NetEntry.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/XvtrPolicy.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QStatusBar>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>

namespace AetherSDR {

QString MainWindow::netScheduleFilePath() const
{
    const QString settingsPath = AppSettings::instance().filePath();
    const QString dir = QFileInfo(settingsPath).absolutePath();
    return QDir(dir).filePath(QStringLiteral("NetSchedule.json"));
}

void MainWindow::initNetScheduler()
{
    m_netScheduler = new NetScheduler(this);
    connect(m_netScheduler, &NetScheduler::reminderDue, this, &MainWindow::onNetReminderDue);

    QList<NetEntry> nets;
    QFile file(netScheduleFilePath());
    if (file.open(QIODevice::ReadOnly)) {
        const auto result = NetScheduleStore::parse(file.readAll());
        nets = result.nets;
    }
    m_netScheduler->setEntries(nets);
}

void MainWindow::showNetSchedulerDialog()
{
    const bool wasFresh = !m_netSchedulerDialog;
    NetSchedulerDialog::CaptureFn capture = [this]() { return captureCurrentNetPreset(); };
    showOrRaisePersistent(m_netSchedulerDialog,
                          m_netScheduler ? m_netScheduler->entries() : QList<NetEntry>(),
                          capture);
    if (wasFresh && m_netSchedulerDialog) {
        connect(m_netSchedulerDialog.data(), &NetSchedulerDialog::entriesChanged, this,
                [this](const QList<NetEntry>& nets) { persistNetSchedule(nets); });
        connect(m_netSchedulerDialog.data(), &NetSchedulerDialog::tuneRequested, this,
                [this](const NetEntry& entry) { tuneToNet(entry); });
    }
}

void MainWindow::persistNetSchedule(const QList<NetEntry>& nets)
{
    const QByteArray json = NetScheduleStore::serialize(
        nets, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QSaveFile file(netScheduleFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(json);
        file.commit();
    }
    if (m_netScheduler)
        m_netScheduler->setEntries(nets);
}

MemoryEntry MainWindow::captureCurrentNetPreset() const
{
    MemoryEntry preset;  // freq defaults to 0.0 → "nothing to capture"
    SliceModel* slice = preferredMemorySlice({});
    if (!slice)
        return preset;
    preset.freq = slice->frequency();
    preset.mode = slice->mode();
    preset.rxFilterLow = slice->filterLow();
    preset.rxFilterHigh = slice->filterHigh();
    return preset;
}

void MainWindow::tuneToNet(const NetEntry& entry)
{
    SliceModel* slice = preferredMemorySlice({});
    if (!slice) {
        statusBar()->showMessage(QStringLiteral("Open a slice before tuning to a net."), 3000);
        return;
    }
    if (slice->isLocked()) {
        slice->notifyTuneBlockedByLock();
        statusBar()->showMessage(QStringLiteral("Unlock the slice to tune to a net."), 3000);
        return;
    }

    const int sliceId = slice->sliceId();
    if (!activeSlice() || activeSlice()->sliceId() != sliceId)
        setActiveSlice(sliceId);
    slice = m_radioModel.slice(sliceId);
    if (!slice)
        return;

    const double freqMhz = entry.preset.freq;

    // Nets can sit on transverter-only bands. applyTuneRequest() runs the
    // band-stack preselect (home of the XVTR support check and its "band isn't
    // available" feedback) only for CommandedTargetCenter, so an AbsoluteJump
    // net tune would silently move the slice onto a band this radio can't
    // reach (#3930). Mirror the preselectBandStackForTune() guard here and
    // refuse before touching the radio.
    if (freqMhz > 54.0 || slice->frequency() > 54.0) {
        const QString targetBand = BandSettings::bandForFrequency(freqMhz);
        const QString currentBand = BandSettings::bandForFrequency(slice->frequency());
        if (targetBand != currentBand) {
            const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
            const auto stackKeyResult = XvtrPolicy::resolveBandStackKey(
                targetBand, xvtrs, m_radioModel.capabilities());
            if (!stackKeyResult.isSupported()) {
                QString reason = stackKeyResult.unsupportedReason;
                if (freqMhz > 54.0 && xvtrs.isEmpty()) {
                    reason = QString("Band %1 requires a configured XVTR before "
                                     "Aether can tune it.")
                                 .arg(targetBand);
                }
                qCWarning(lcProtocol).noquote().nospace()
                    << "MainWindow: net tune cannot preselect band stack"
                    << " source=net-tune net=" << entry.name
                    << " from_band=" << currentBand
                    << " to_band=" << targetBand
                    << " freq_mhz=" << QString::number(freqMhz, 'f', 6)
                    << " reason=" << reason
                    << " available_xvtrs=" << xvtrListSummary(xvtrs);
                statusBar()->showMessage(
                    QString("Can't tune %1 — %2").arg(entry.name, reason), 5000);
                return;
            }
        }
    }

    // Route the net's frequency change through the canonical tune-and-recenter
    // policy — the same AbsoluteJump path a DX-cluster spot uses to jump to an
    // arbitrary frequency on any band. applyTuneRequest() moves the slice with
    // `slice tune <freq>` (which the radio echoes back as a slice RF_frequency
    // status, so the VFO display tracks it) and recenters the panadapter on the
    // target, crossing bands as needed.
    //
    // The previous bespoke path issued `display pan set <pan> band=<key>` first,
    // which reloaded the band stack: the radio retuned the slice to that band's
    // *last-used* frequency (and echoed it), then emitted no status echo for the
    // subsequent net retune — so the VFO display stuck on the band frequency
    // while RX/TX ran on the net's (#3918). Reusing applyTuneRequest avoids the
    // band-stack reload entirely and keeps the display radio-authoritative.
    if (freqMhz > 0.0)
        applyTuneRequest(slice, freqMhz, TuneIntent::AbsoluteJump, "net-tune");

    // Net-specific slice settings the tune policy doesn't cover (a net has no
    // radio-side memory slot to "memory apply").
    if (!entry.preset.mode.isEmpty())
        slice->setMode(entry.preset.mode);
    if (entry.preset.rxFilterLow != entry.preset.rxFilterHigh) {
        m_radioModel.sendCommand(QString("filt %1 %2 %3")
                                     .arg(sliceId)
                                     .arg(entry.preset.rxFilterLow)
                                     .arg(entry.preset.rxFilterHigh));
    }
    if (entry.preset.step > 0)
        m_radioModel.sendCommand(QString("slice set %1 step=%2").arg(sliceId).arg(entry.preset.step));
    const QString fixup = buildMemoryRecallSliceFixupCommand(sliceId, entry.preset);
    if (!fixup.isEmpty())
        m_radioModel.sendCommand(fixup);

    statusBar()->showMessage(QString("Tuned to %1").arg(entry.name), 3000);
}

void MainWindow::onNetReminderDue(const NetEntry& entry, const QDateTime& occurrenceUtc)
{
    const qint64 minutes = QDateTime::currentDateTimeUtc().secsTo(occurrenceUtc) / 60;
    QString headline;
    if (minutes > 1)
        headline = QString("%1 starts in %2 min").arg(entry.name).arg(minutes);
    else if (minutes >= 0)
        headline = QString("%1 is starting now").arg(entry.name);
    else
        headline = QString("%1 is on the air").arg(entry.name);

    const QString detail = QString("%1 MHz · %2")
                               .arg(entry.preset.freq, 0, 'f', 4)
                               .arg(entry.preset.mode.isEmpty() ? QStringLiteral("—")
                                                                : entry.preset.mode);

    const bool canTune = preferredMemorySlice({}) != nullptr;

    // In-app banner — the guaranteed actionable path (created once).
    if (!m_netReminderBanner) {
        m_netReminderBanner = new NetReminderBanner(this);
        connect(m_netReminderBanner, &NetReminderBanner::tuneRequested, this,
                [this](const QString& netId) {
                    if (!m_netScheduler)
                        return;
                    for (const NetEntry& e : m_netScheduler->entries()) {
                        if (e.id == netId) {
                            tuneToNet(e);
                            break;
                        }
                    }
                    // Bring the window forward without disturbing its state.
                    // showNormal() would clear a Maximized/FullScreen window
                    // (#3918) — only un-minimize if actually minimized.
                    if (isMinimized())
                        showNormal();
                    raise();
                    activateWindow();
                });
    }
    m_netReminderBanner->showReminder(entry.id, headline, detail, canTune);

    // Best-effort OS notification (attention-getter). Created lazily so users
    // with no scheduled nets never get a persistent tray icon. Click raises the
    // window; the actionable button lives in the in-app banner above because
    // QSystemTrayIcon::showMessage() cannot carry one.
    if (!m_trayIcon && QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayIcon = new QSystemTrayIcon(windowIcon(), this);
        m_trayIcon->setToolTip(QStringLiteral("AetherSDR"));
        m_trayIcon->show();
        connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this, [this] {
            // Raise without un-maximizing the window (#3918).
            if (isMinimized())
                showNormal();
            raise();
            activateWindow();
        });
    }
    if (m_trayIcon) {
        m_trayIcon->showMessage(entry.name, headline + '\n' + detail,
                                QSystemTrayIcon::Information, 15000);
    }
}

} // namespace AetherSDR
