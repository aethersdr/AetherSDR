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
    const QString slicePanId = slice->panId();

    // If the net is on a different band than the slice currently sits on,
    // preselect that band's stack memory first — a bare `slice tune` will not
    // move the panadapter across bands, which is why a cross-band net appears
    // "not to tune". Same preselect path as a memory recall.
    if (freqMhz > 0.0) {
        const QString netBand = BandSettings::bandForFrequency(freqMhz);
        const QString currentBand = BandSettings::bandForFrequency(slice->frequency());
        if (netBand != currentBand) {
            const auto xvtrs = xvtrPolicyBandsFrom(m_radioModel.xvtrList());
            const auto stackKeyResult =
                XvtrPolicy::resolveBandStackKey(netBand, xvtrs, m_radioModel.capabilities());
            if (stackKeyResult.isSupported()) {
                clearSwrSweepForBandChange(-1, slicePanId, netBand);
                m_bandSettings.setCurrentBand(netBand);
                m_radioModel.sendCommand(
                    QString("display pan set %1 band=%2").arg(slicePanId, stackKeyResult.key));
                QTimer::singleShot(300, this, [this, slicePanId]() {
                    reassertUnmutedSliceAudioForPan(slicePanId);
                });
            } else {
                statusBar()->showMessage(
                    QString("Can't tune %1 — %2 isn't available on this radio.")
                        .arg(entry.name, netBand),
                    5000);
            }
        }
    }

    // Reuse the memory-recall command builders for the retune + repeater/tone
    // fixup, and set mode/filter/step directly (a net has no radio-side memory
    // slot to "memory apply").
    const QString retune = buildMemoryRecallRetuneCommand(sliceId, entry.preset);
    if (!retune.isEmpty())
        m_radioModel.sendCommand(retune);
    if (!entry.preset.mode.isEmpty())
        m_radioModel.sendCommand(
            QString("slice set %1 mode=%2").arg(sliceId).arg(entry.preset.mode));
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

    // After the band change + retune settle, recenter the panadapter on the net
    // frequency if it ended up off-screen (mirrors the memory-recall reveal).
    QTimer::singleShot(750, this, [this, sliceId, freqMhz]() {
        if (auto* s = m_radioModel.slice(sliceId)) {
            const double revealMhz = freqMhz > 0.0 ? freqMhz : s->frequency();
            revealFrequencyIfNeeded(s, revealMhz, TuneIntent::CommandedTargetCenter, "net-tune");
        }
    });

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
