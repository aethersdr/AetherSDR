#include "EspFirmwareJob.h"

#include "Ctr2MaxRegistration.h"
#include "Ctr2MaxStockFirmware.h"
#include "EspFirmwareInstaller.h"
#ifdef HAVE_SERIALPORT
#include "EspFlasher.h"
#endif

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

namespace AetherSDR {

namespace {

QString infoPath(const QString& backupPath)
{
    return backupPath + QStringLiteral(".json");
}

QString macText(const QByteArray& mac)
{
    return QString::fromLatin1(mac.toHex(':')).toUpper();
}

} // namespace

bool EspFirmwareJob::readBackupInfo(const QString& backupPath, BackupInfo* info, QString* problem)
{
    QFile f(infoPath(backupPath));
    if (!f.open(QIODevice::ReadOnly)) {
        *problem = tr("%1 is missing, so this backup cannot be matched to a device.")
                       .arg(QFileInfo(f).fileName());
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    info->chip = o.value("chip").toString();
    info->mac = QByteArray::fromHex(o.value("mac").toString().remove(QLatin1Char(':')).toLatin1());
    info->flashSize = static_cast<quint32>(o.value("flashSize").toDouble());
    info->sha256 = QByteArray::fromHex(o.value("sha256").toString().toLatin1());
    if (info->mac.size() != 6 || info->flashSize == 0 || info->sha256.size() != 32) {
        *problem = tr("The backup's description (%1) is incomplete.").arg(QFileInfo(f).fileName());
        return false;
    }
    return true;
}

EspFirmwareJob::EspFirmwareJob(QObject* parent)
    : QObject(parent)
{
}

EspFirmwareJob::~EspFirmwareJob()
{
    if (m_thread) {
        m_cancel = true;
        m_thread->wait();
        delete m_thread;
    }
}

bool EspFirmwareJob::start(const Request& request)
{
    if (m_thread)
        return false;
    m_cancel = false;
    m_thread = QThread::create([this, request] { run(request); });
    // Report only once the thread is done, so a slot can start the next job.
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
        emit finished(m_ok, m_message);
    });
    m_thread->start();
    return true;
}

void EspFirmwareJob::cancel()
{
    m_cancel = true;
}

void EspFirmwareJob::run(const Request& request)
{
    // The outcome, reported by the thread's finished() handler; progress
    // signals from this thread are queued to the job's owner.
    const auto finish = [this](bool ok, const QString& message) {
        m_ok = ok;
        m_message = message;
    };
    QString problem;
    std::unique_ptr<EspFlashTarget> target;
    if (m_opener) {
        target = m_opener(request.portName, &problem);
    } else {
#ifdef HAVE_SERIALPORT
        auto flasher = std::make_unique<EspFlasher>();
        if (flasher->open(request.portName, &problem))
            target = std::move(flasher);
#else
        problem = tr("This build of AetherSDR has no serial port support.");
#endif
    }
    if (!target) {
        finish(false, problem);
        return;
    }
    EspFlashTarget& flasher = *target;
    // The device the operator confirmed, and no other: a port name can be
    // reused by another device, and enumeration can change while a prompt
    // is open. Nothing has been written yet.
    if (flasher.mac().size() != 6
        || (!request.expectedMac.isEmpty() && flasher.mac() != request.expectedMac)) {
        finish(false, flasher.mac().size() != 6
                          ? tr("The device's MAC address could not be read. Nothing was written.")
                          : tr("The device in download mode is %1, not %2, the one you chose. "
                               "Nothing was written.")
                                .arg(macText(flasher.mac()), macText(request.expectedMac)));
        return;
    }
    {
        const QString chip = flasher.chipName();
        const QByteArray mac = flasher.mac();
        const quint32 size = flasher.flashSize();
        QMetaObject::invokeMethod(this, [=, this] { emit connected(chip, mac, size); },
                                  Qt::QueuedConnection);
    }
    EspFirmwareInstaller installer(&flasher, [this](const QString& step, qint64 done, qint64 total) {
        QMetaObject::invokeMethod(this, [=, this] { emit progress(step, done, total); },
                                  Qt::QueuedConnection);
        return !m_cancel.load();
    });

    const auto backup = [&]() -> bool {
        QByteArray flash;
        if (!installer.backup(&flash, &problem))
            return false;
        QSaveFile out(request.backupPath);
        if (!out.open(QIODevice::WriteOnly) || out.write(flash) != flash.size() || !out.commit()) {
            problem = tr("Could not save the backup to %1.").arg(request.backupPath);
            return false;
        }
        const QJsonObject info{
            {"chip", flasher.chipName()},
            {"mac", macText(flasher.mac())},
            {"flashSize", static_cast<double>(flasher.flashSize())},
            {"sha256", QString::fromLatin1(
                           QCryptographicHash::hash(flash, QCryptographicHash::Sha256).toHex())},
            {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        };
        QSaveFile json(infoPath(request.backupPath));
        if (!json.open(QIODevice::WriteOnly) || json.write(QJsonDocument(info).toJson()) < 0
            || !json.commit()) {
            problem = tr("Could not save the backup's description beside it.");
            return false;
        }
        return true;
    };

    // Before installing on a CTR2-Max: is it registered? Reads, never writes.
    const auto registered = [&]() -> bool {
        if (!request.requireCtr2MaxRegistration)
            return true;
        QByteArray volume;
        const QString step = tr("Checking the registration");
        if (!flasher.read(Ctr2MaxRegistration::kOffset, Ctr2MaxRegistration::kSize, &volume,
                          [&](qint64 done, qint64 total) {
                              QMetaObject::invokeMethod(
                                  this, [=, this] { emit progress(step, done, total); },
                                  Qt::QueuedConnection);
                              return !m_cancel.load();
                          },
                          &problem))
            return false;
        QString call;
        switch (Ctr2MaxRegistration::check(volume, &call)) {
        case Ctr2MaxRegistration::Result::Registered:
            return true;
        case Ctr2MaxRegistration::Result::NotRegistered:
            problem = tr("This CTR2-Max is not registered with CTR2-Max firmware v1.02.00m or "
                         "later. Update its CTR2-Max firmware if it is older (Open File…), "
                         "make sure it is registered, then install again. Nothing was "
                         "written.");
            return false;
        case Ctr2MaxRegistration::Result::Unreadable:
            problem = tr("This CTR2-Max's settings could not be read, so its registration "
                         "cannot be checked. Nothing was written.");
            return false;
        }
        return false;
    };

    // Firmware laid out by the device's own partition table: plan it now,
    // before anything is written.
    EspFirmwareManifest manifest = request.manifest;
    QHash<QString, QByteArray> files = request.files;
    const auto planned = [&]() -> bool {
        if (!request.plan)
            return true;
        QByteArray table;
        return flasher.read(Ctr2MaxStockFirmware::kTableOffset, Ctr2MaxStockFirmware::kTableSize,
                            &table, {}, &problem)
               && request.plan(table, flasher.flashSize(), &manifest, &files, &problem);
    };

    bool ok = false;
    bool restart = true;
    switch (request.action) {
    case Action::Backup:
        ok = backup();
        break;
    case Action::Install:
        ok = planned() && installer.check(manifest, &problem) && registered()
             && (request.backupPath.isEmpty() || backup())
             && installer.install(manifest, files, &problem);
        break;
    case Action::Start:
        ok = planned() && installer.start(manifest, &problem);
        break;
    case Action::Restore: {
        BackupInfo info;
        if (!readBackupInfo(request.restoreFrom, &info, &problem))
            break;
        if (info.mac != flasher.mac()) {
            problem = tr("This backup is of the device %1; the one connected is %2. Nothing was "
                         "written.")
                          .arg(macText(info.mac), macText(flasher.mac()));
            break;
        }
        QFile f(request.restoreFrom);
        if (!f.open(QIODevice::ReadOnly)) {
            problem = tr("Could not read %1.").arg(request.restoreFrom);
            break;
        }
        const QByteArray flash = f.readAll();
        if (QCryptographicHash::hash(flash, QCryptographicHash::Sha256) != info.sha256) {
            problem = tr("The backup file has changed since it was made. Nothing was written.");
            break;
        }
        ok = installer.restore(flash, &problem);
        break;
    }
    }
    if (!ok && m_cancel.load() && problem.isEmpty())
        problem = tr("Cancelled.");
    // After a failure the device stays in download mode, so the operator can
    // try again; a failed backup changed nothing, so it may as well restart.
    if (!ok && request.action != Action::Backup)
        restart = false;
    QString restartProblem;
    if (restart && !flasher.restart(&restartProblem)) {
        finish(ok, ok ? restartProblem : problem);
        return;
    }
    finish(ok, problem);
}

} // namespace AetherSDR
