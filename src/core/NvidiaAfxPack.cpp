#ifdef HAVE_NVIDIA_AFX

#include "NvidiaAfxPack.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

// ─── Static helpers ──────────────────────────────────────────────────────────
QString NvidiaAfxPack::detectArch()
{
    // Query the NVIDIA driver for the GPU compute capability, e.g. "8.9" -> sm_89.
    QProcess p;
    p.start(QStringLiteral("nvidia-smi"),
            {QStringLiteral("--query-gpu=compute_cap"),
             QStringLiteral("--format=csv,noheader")});
    if (!p.waitForFinished(4000) || p.exitStatus() != QProcess::NormalExit)
        return {};
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    // May list several GPUs; take the first supported (Turing+ = >= 7.5).
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        const QString cc = line.trimmed();           // "8.9"
        const QString digits = QString(cc).remove('.'); // "89"
        bool ok = false;
        const int v = digits.toInt(&ok);
        if (ok && v >= 75)
            return QStringLiteral("sm_%1").arg(digits);
    }
    return {};
}

QString NvidiaAfxPack::cacheRoot()
{
    QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (data.isEmpty())
        data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(data).filePath(QStringLiteral("nvidia-afx"));
}

QString NvidiaAfxPack::installedPackDir()
{
    const QString dir = QDir(cacheRoot()).filePath(QStringLiteral("current"));
    // A usable pack has the AFX core lib and at least one denoiser model.
    if (!QFile::exists(QDir(dir).filePath(QStringLiteral("nvafx/lib/libnv_audiofx.so"))))
        return {};
    const QDir models(QDir(dir).filePath(QStringLiteral("features/denoiser/models")));
    const QStringList sm = models.entryList({QStringLiteral("sm_*")},
                                            QDir::Dirs | QDir::NoDotAndDotDot);
    return sm.isEmpty() ? QString() : dir;
}

bool NvidiaAfxPack::isInstalled()
{
    return !installedPackDir().isEmpty();
}

QString NvidiaAfxPack::statusText() const
{
    if (m_busy)
        return QStringLiteral("Working…");
    const QString dir = installedPackDir();
    if (dir.isEmpty())
        return QStringLiteral("Not installed");
    // Report the installed arch from the model dir name.
    const QDir models(QDir(dir).filePath(QStringLiteral("features/denoiser/models")));
    const QStringList sm = models.entryList({QStringLiteral("sm_*")},
                                            QDir::Dirs | QDir::NoDotAndDotDot);
    return sm.isEmpty() ? QStringLiteral("Installed")
                        : QStringLiteral("Installed (%1)").arg(sm.first());
}

bool NvidiaAfxPack::removeInstalled()
{
    const QString cur = QDir(cacheRoot()).filePath(QStringLiteral("current"));
    QFileInfo fi(cur);
    if (fi.isSymLink())
        return QFile::remove(cur);     // dev symlink — don't nuke the target
    return fi.exists() ? QDir(cur).removeRecursively() : true;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
NvidiaAfxPack::NvidiaAfxPack(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

NvidiaAfxPack::~NvidiaAfxPack() { cancel(); }

void NvidiaAfxPack::cancel()
{
    m_cancelled = true;
    if (m_reply) { m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
    if (!m_tmpArchive.isEmpty()) { QFile::remove(m_tmpArchive); m_tmpArchive.clear(); }
    m_busy = false;
}

void NvidiaAfxPack::fail(const QString& msg)
{
    if (!m_tmpArchive.isEmpty()) { QFile::remove(m_tmpArchive); m_tmpArchive.clear(); }
    m_busy = false;
    emit finished(false, msg);
}

// ─── Install (download or import) ────────────────────────────────────────────
void NvidiaAfxPack::install(const QString& sourceUrlOrPath)
{
    if (m_busy) return;
    m_busy = true;
    m_cancelled = false;

    const QUrl url(sourceUrlOrPath);
    const bool isHttp = url.scheme() == QLatin1String("http")
                     || url.scheme() == QLatin1String("https");

    if (!isHttp) {
        // Local archive path or file:// — import directly, no download.
        const QString path = url.isLocalFile() ? url.toLocalFile() : sourceUrlOrPath;
        if (!QFile::exists(path)) { fail(QStringLiteral("archive not found: %1").arg(path)); return; }
        emit progress(-1, QStringLiteral("Extracting…"));
        importArchive(path);
        return;
    }

    // Stream the download to a temp file in the cache root (same filesystem as
    // the final install, so the later rename is atomic).
    QDir().mkpath(cacheRoot());
    m_tmpArchive = QDir(cacheRoot()).filePath(QStringLiteral(".download.tar.zst"));
    auto* out = new QFile(m_tmpArchive);
    if (!out->open(QIODevice::WriteOnly)) {
        delete out;
        fail(QStringLiteral("cannot write to cache: %1").arg(cacheRoot()));
        return;
    }

    QNetworkRequest req{url};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    emit progress(0, QStringLiteral("Downloading…"));

    connect(m_reply, &QNetworkReply::readyRead, this, [this, out]() {
        out->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64 total) {
        emit progress(total > 0 ? int(got * 100 / total) : -1,
                      QStringLiteral("Downloading… %1 MB").arg(got / 1048576));
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, out]() {
        out->flush(); out->close();
        const bool ok = m_reply->error() == QNetworkReply::NoError && !m_cancelled;
        const QString err = m_reply->errorString();
        m_reply->deleteLater(); m_reply = nullptr;
        delete out;
        if (!ok) { fail(m_cancelled ? QStringLiteral("cancelled")
                                    : QStringLiteral("download failed: %1").arg(err)); return; }
        emit progress(-1, QStringLiteral("Extracting…"));
        const QString archive = m_tmpArchive;
        importArchive(archive);
    });
}

// Extract the .tar.zst into a staging dir, place the feature lib on the core's
// RPATH, then atomically swap it in as cacheRoot/current.
void NvidiaAfxPack::importArchive(const QString& archivePath)
{
    const QString root = cacheRoot();
    QDir().mkpath(root);
    const QString staging = QDir(root).filePath(QStringLiteral(".staging"));
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging)) { fail(QStringLiteral("cannot create staging dir")); return; }

    QProcess tar;
    tar.setWorkingDirectory(staging);
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("--zstd"), QStringLiteral("-xf"), archivePath});
    if (!tar.waitForFinished(600000) || tar.exitCode() != 0) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("extraction failed: %1")
                 .arg(QString::fromLocal8Bit(tar.readAllStandardError()).trimmed()));
        return;
    }

    // The archive may have a single top-level dir; resolve to the pack root
    // (the dir containing nvafx/lib).
    QString packRoot = staging;
    if (!QFile::exists(QDir(packRoot).filePath(QStringLiteral("nvafx/lib")))) {
        const QStringList subs = QDir(staging).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& s : subs) {
            const QString cand = QDir(staging).filePath(s);
            if (QFile::exists(QDir(cand).filePath(QStringLiteral("nvafx/lib")))) {
                packRoot = cand; break;
            }
        }
    }
    if (!QFile::exists(QDir(packRoot).filePath(QStringLiteral("nvafx/lib/libnv_audiofx.so")))) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("archive is not a valid AFX pack (no nvafx/lib/libnv_audiofx.so)"));
        return;
    }

    // Atomic swap: current.old <- current, current <- packRoot.
    const QString current = QDir(root).filePath(QStringLiteral("current"));
    const QString backup  = QDir(root).filePath(QStringLiteral("current.old"));
    QFileInfo curInfo(current);
    if (curInfo.isSymLink()) QFile::remove(current);
    else if (curInfo.exists()) { QDir(backup).removeRecursively(); QDir().rename(current, backup); }
    if (!QDir().rename(packRoot, current)) {
        QDir(staging).removeRecursively();
        fail(QStringLiteral("could not install pack into %1").arg(current));
        return;
    }

    // Put the denoiser feature lib on the core's $ORIGIN/../../nvafx/lib RPATH so
    // NvAFX_CreateEffect can dlopen it by soname (it requests the unversioned
    // name). Done AFTER the move so the symlinks point into current/, not the
    // now-deleted staging dir.
    const QString featDir  = QDir(current).filePath(QStringLiteral("features/denoiser/lib"));
    const QString nvafxDir = QDir(current).filePath(QStringLiteral("nvafx/lib"));
    for (const QFileInfo& fi : QDir(featDir).entryInfoList(
             {QStringLiteral("libnv_audiofx_denoiser.so*")}, QDir::Files)) {
        const QString link = QDir(nvafxDir).filePath(fi.fileName());
        QFile::remove(link);
        QFile::link(fi.absoluteFilePath(), link);   // absolute symlink into current/
    }

    QDir(staging).removeRecursively();
    QDir(backup).removeRecursively();
    if (!m_tmpArchive.isEmpty()) { QFile::remove(m_tmpArchive); m_tmpArchive.clear(); }

    m_busy = false;
    if (installedPackDir().isEmpty()) { emit finished(false, QStringLiteral("install verification failed")); return; }
    emit progress(100, statusText());
    emit finished(true, statusText());
}

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
