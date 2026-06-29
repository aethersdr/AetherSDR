#ifdef HAVE_NVIDIA_AFX

#include "NvidiaAfxPack.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

// ─── Platform layout ─────────────────────────────────────────────────────────
// Linux and Windows ship different AFX runtimes and pack layouts:
//   • Linux: core nvafx/lib/libnv_audiofx.so + a split download (CUDA from PyPI
//     wheels, AFX/TRT/model from our .tar.zst), feature lib symlinked onto RPATH.
//   • Windows: core bin/NVAudioEffects.dll. The Windows AFX-bits archive is a
//     SELF-CONTAINED .zip (AFX + CUDA + TensorRT DLLs all co-located in bin/,
//     model under features/) — Windows resolves sibling DLLs by directory, so no
//     wheel-flattening, no zstd tooling, and no symlink step are needed.
namespace {
#if defined(_WIN32)
constexpr char kCoreRelPath[] = "bin/NVAudioEffects.dll";
constexpr char kPlatformTag[] = "windows-x86_64";
// Pinned sha256 of afx-bits-2.1.0-windows-x86_64-<arch>.zip — filled in when the
// Windows pack asset is published.
constexpr char kWinTarballSha[] = "";
#else
constexpr char kCoreRelPath[] = "nvafx/lib/libnv_audiofx.so";
constexpr char kPlatformTag[] = "linux-x86_64";
#endif

// "12.3 MB/s" from a bytes-per-second rate.
QString humanRate(double bps)
{
    const char* unit = "B/s";
    double v = bps;
    if (v >= 1024.0) { v /= 1024.0; unit = "KB/s"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "MB/s"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "GB/s"; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10.0 ? 1 : 0).arg(QLatin1String(unit));
}

// "8s" / "1m 05s" / "1h 02m" from a seconds count.
QString humanEta(qint64 secs)
{
    if (secs < 60)   return QStringLiteral("%1s").arg(secs);
    if (secs < 3600) return QStringLiteral("%1m %2s").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1h %2m").arg(secs / 3600).arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'));
}
} // namespace

// ─── Static helpers ──────────────────────────────────────────────────────────
QString NvidiaAfxPack::detectArch()
{
    QProcess p;
    p.start(QStringLiteral("nvidia-smi"),
            {QStringLiteral("--query-gpu=compute_cap"), QStringLiteral("--format=csv,noheader")});
    if (!p.waitForFinished(4000) || p.exitStatus() != QProcess::NormalExit)
        return {};
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        const QString digits = line.trimmed().remove('.');   // "8.9" -> "89"
        bool ok = false;
        const int v = digits.toInt(&ok);
        if (ok && v >= 75)                                    // Turing+
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
    if (!QFile::exists(QDir(dir).filePath(QString::fromLatin1(kCoreRelPath))))
        return {};
    const QDir models(QDir(dir).filePath(QStringLiteral("features/denoiser/models")));
    return models.entryList({QStringLiteral("sm_*")}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()
               ? QString() : dir;
}

bool NvidiaAfxPack::isInstalled() { return !installedPackDir().isEmpty(); }

QString NvidiaAfxPack::statusText() const
{
    if (m_busy) return QStringLiteral("Working…");
    const QString dir = installedPackDir();
    if (dir.isEmpty()) return QStringLiteral("Not installed");
    const QDir models(QDir(dir).filePath(QStringLiteral("features/denoiser/models")));
    const QStringList sm = models.entryList({QStringLiteral("sm_*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    return sm.isEmpty() ? QStringLiteral("Installed") : QStringLiteral("Installed (%1)").arg(sm.first());
}

bool NvidiaAfxPack::removeInstalled()
{
    const QString cur = QDir(cacheRoot()).filePath(QStringLiteral("current"));
    QFileInfo fi(cur);
    if (fi.isSymLink()) return QFile::remove(cur);
    return fi.exists() ? QDir(cur).removeRecursively() : true;
}

// ─── Manifest (pinned) ───────────────────────────────────────────────────────
// CUDA libs from NVIDIA's PyPI wheels (url+sha resolved at runtime). The AFX
// proprietary libs + TensorRT runtime libs + the per-arch denoiser model ship
// in one small .tar.zst we host. Versions match what libnv_audiofx was built
// against (CUDA 12.8.x / TensorRT 10.9.0.34).
QList<NvidiaAfxPack::Component> NvidiaAfxPack::manifest(const QString& arch) const
{
    // The AFX-bits archive is per-arch (it carries the sm_XX denoiser model).
    const QString afxUrl =
        QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/download/"
                       "afx-bits-2.1.0/afx-bits-2.1.0-%1-%2.%3")
            .arg(QString::fromLatin1(kPlatformTag), arch,
#if defined(_WIN32)
                 QStringLiteral("zip"));
#else
                 QStringLiteral("tar.zst"));
#endif

#if defined(_WIN32)
    // Windows: one self-contained .zip — AFX + CUDA + TensorRT DLLs + model.
    return {
        { QStringLiteral("AFX runtime + CUDA + TensorRT + model"),
          {}, QStringLiteral("2.1.0"), afxUrl,
          QString::fromLatin1(kWinTarballSha), Kind::Tarball },
    };
#else
    // Linux: AFX/TRT/model from our tarball; CUDA libs from NVIDIA's PyPI wheels.
    return {
        { QStringLiteral("AFX runtime + TensorRT + model"),
          {}, QStringLiteral("2.1.0"), afxUrl,
          QStringLiteral("0bfe85b0faeb322958303c145996350d0fea8a203899f9215fc0d3a341395b67"),
          Kind::Tarball },
        { QStringLiteral("CUDA runtime"), QStringLiteral("nvidia-cuda-runtime-cu12"), QStringLiteral("12.8.90"), {}, {}, Kind::Wheel },
        { QStringLiteral("cuBLAS"),       QStringLiteral("nvidia-cublas-cu12"),       QStringLiteral("12.8.4.1"), {}, {}, Kind::Wheel },
        { QStringLiteral("cuFFT"),        QStringLiteral("nvidia-cufft-cu12"),        QStringLiteral("11.3.3.83"), {}, {}, Kind::Wheel },
        { QStringLiteral("nvRTC"),        QStringLiteral("nvidia-cuda-nvrtc-cu12"),   QStringLiteral("12.8.93"), {}, {}, Kind::Wheel },
    };
#endif
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
NvidiaAfxPack::NvidiaAfxPack(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

NvidiaAfxPack::~NvidiaAfxPack() { cancel(); }

void NvidiaAfxPack::cancel()
{
    m_cancelled = true;
    if (m_reply) { m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
    if (!m_tmpFile.isEmpty()) { QFile::remove(m_tmpFile); m_tmpFile.clear(); }
    if (!m_staging.isEmpty()) { QDir(m_staging).removeRecursively(); m_staging.clear(); }
    m_busy = false;
}

void NvidiaAfxPack::fail(const QString& msg)
{
    if (!m_tmpFile.isEmpty()) { QFile::remove(m_tmpFile); m_tmpFile.clear(); }
    if (!m_staging.isEmpty()) { QDir(m_staging).removeRecursively(); m_staging.clear(); }
    m_busy = false;
    emit finished(false, msg);
}

void NvidiaAfxPack::emitOverall(int compPct, const QString& label)
{
    const int n = m_queue.isEmpty() ? 1 : m_queue.size();
    const int base = m_idx * 100 / n;
    const int within = (compPct < 0 ? 0 : compPct) / n;
    emit progress(compPct < 0 && m_idx == 0 ? -1 : base + within,
                  QStringLiteral("%1 (%2/%3)").arg(label).arg(m_idx + 1).arg(n));
}

// ─── v2 multi-source install ─────────────────────────────────────────────────
void NvidiaAfxPack::install()
{
    if (m_busy) return;
    m_arch = detectArch();
    if (m_arch.isEmpty()) { emit finished(false, QStringLiteral("no supported NVIDIA GPU found")); return; }
    m_busy = true; m_cancelled = false;
    QDir().mkpath(cacheRoot());
    m_staging = QDir(cacheRoot()).filePath(QStringLiteral(".staging"));
    QDir(m_staging).removeRecursively();
    QDir().mkpath(m_staging);
#if !defined(_WIN32)
    // Linux flattens CUDA wheel .so files here; Windows DLLs ride in the zip.
    QDir().mkpath(QDir(m_staging).filePath(QStringLiteral("external/cuda/lib")));
#endif
    m_queue = manifest(m_arch);
    m_idx = 0;
    startNext();
}

void NvidiaAfxPack::startNext()
{
    if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
    if (m_idx >= m_queue.size()) { assembleAndCommit(); return; }
    const Component& c = m_queue[m_idx];
    if (c.kind == Kind::Wheel)
        resolveWheelUrl(c);                        // PyPI JSON -> url+sha -> download
    else
        downloadTo(QUrl(c.url), c.sha256,
                   QDir(cacheRoot()).filePath(QStringLiteral(".dl.tar.zst")), c.name);
}

// Resolve the manylinux x86_64 wheel URL + sha256 from the PyPI JSON API.
void NvidiaAfxPack::resolveWheelUrl(const Component& c)
{
    emitOverall(-1, QStringLiteral("Resolving %1").arg(c.name));
    const QUrl api(QStringLiteral("https://pypi.org/pypi/%1/%2/json").arg(c.pypiPkg, c.pypiVer));
    QNetworkRequest req(api);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, name = c.name]() {
        const QByteArray body = reply->readAll();
        const auto err = reply->error();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) { fail(QStringLiteral("PyPI lookup failed for %1").arg(name)); return; }
        const QJsonObject root = QJsonDocument::fromJson(body).object();
        for (const QJsonValue v : root.value(QStringLiteral("urls")).toArray()) {
            const QJsonObject u = v.toObject();
            const QString fn = u.value(QStringLiteral("filename")).toString();
            if (fn.endsWith(QStringLiteral(".whl")) && fn.contains(QStringLiteral("x86_64"))
                && fn.contains(QStringLiteral("manylinux"))) {
                const QString url = u.value(QStringLiteral("url")).toString();
                const QString sha = u.value(QStringLiteral("digests")).toObject()
                                       .value(QStringLiteral("sha256")).toString();
                // Record the resolved sha so the install receipt can report it.
                if (m_idx < m_queue.size()) m_queue[m_idx].sha256 = sha;
                downloadTo(QUrl(url), sha,
                           QDir(cacheRoot()).filePath(QStringLiteral(".dl.whl")), name);
                return;
            }
        }
        fail(QStringLiteral("no manylinux x86_64 wheel for %1").arg(name));
    });
}

void NvidiaAfxPack::downloadTo(const QUrl& url, const QString& sha256,
                               const QString& dest, const QString& label)
{
    m_tmpFile = dest;
    auto* out = new QFile(dest);
    if (!out->open(QIODevice::WriteOnly)) { delete out; fail(QStringLiteral("cannot write cache")); return; }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    m_dlTimer.start();
    emitOverall(0, QStringLiteral("Downloading %1").arg(label));
    connect(m_reply, &QNetworkReply::readyRead, this, [this, out]() { out->write(m_reply->readAll()); });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this, label](qint64 got, qint64 total) {
        // Average rate since this file started — smoother than per-tick deltas.
        const qint64 ms = m_dlTimer.elapsed();
        QString suffix;
        if (ms > 500 && got > 0) {
            const double bps = got * 1000.0 / double(ms);
            suffix = QStringLiteral(" — %1").arg(humanRate(bps));
            if (total > got && bps > 1.0)
                suffix += QStringLiteral(", %1 left").arg(humanEta(qint64((total - got) / bps)));
        }
        emitOverall(total > 0 ? int(got * 100 / total) : -1,
                    QStringLiteral("Downloading %1%2").arg(label, suffix));
    });
    connect(m_reply, &QNetworkReply::finished, this,
            [this, out, sha256, label, dest]() {
        out->flush(); out->close(); delete out;
        const auto err = m_reply->error();
        const QString es = m_reply->errorString();
        m_reply->deleteLater(); m_reply = nullptr;
        if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
        if (err != QNetworkReply::NoError) { fail(QStringLiteral("download failed (%1): %2").arg(label, es)); return; }
        if (!sha256.isEmpty()) {
            QFile f(dest);
            if (!f.open(QIODevice::ReadOnly)) { fail(QStringLiteral("cannot re-open %1 to verify").arg(label)); return; }
            QCryptographicHash h(QCryptographicHash::Sha256); h.addData(&f);
            if (h.result().toHex() != sha256.toLatin1()) { fail(QStringLiteral("checksum mismatch: %1").arg(label)); return; }
        }
        const Kind kind = m_queue[m_idx].kind;
        emitOverall(100, QStringLiteral("Extracting %1").arg(label));
        extractInto(dest, kind);
        QFile::remove(dest); m_tmpFile.clear();
        ++m_idx;
        startNext();
    });
}

void NvidiaAfxPack::extractInto(const QString& archive, Kind kind)
{
    QProcess p;
    if (kind == Kind::Wheel) {
        // Flatten the wheel's *.so* into the pack's external/cuda/lib.
        const QString libDir = QDir(m_staging).filePath(QStringLiteral("external/cuda/lib"));
        p.start(QStringLiteral("unzip"),
                {QStringLiteral("-o"), QStringLiteral("-j"), QStringLiteral("-d"), libDir,
                 archive, QStringLiteral("*.so*")});
    } else {
#if defined(_WIN32)
        // Windows AFX-bits is a self-contained .zip laid out at root (bin/,
        // features/). bsdtar (tar.exe, shipped with Windows 10+) reads zip.
        p.start(QStringLiteral("tar"),
                {QStringLiteral("-xf"), archive, QStringLiteral("-C"), m_staging});
#else
        // Our AFX-bits tarball is laid out at root (nvafx/, features/, external/).
        p.start(QStringLiteral("tar"),
                {QStringLiteral("--zstd"), QStringLiteral("-xf"), archive,
                 QStringLiteral("-C"), m_staging});
#endif
    }
    if (!p.waitForFinished(600000) || p.exitCode() != 0)
        fail(QStringLiteral("extract failed: %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed()));
}

// Symlink the feature lib onto the core RPATH, then atomically swap into place.
void NvidiaAfxPack::assembleAndCommit()
{
    const QString root = cacheRoot();
    QString packRoot = m_staging;
    if (!QFile::exists(QDir(packRoot).filePath(QString::fromLatin1(kCoreRelPath)))) {
        fail(QStringLiteral("assembled pack missing %1 (AFX-bits archive not published yet?)")
                 .arg(QString::fromLatin1(kCoreRelPath)));
        return;
    }
    const QString current = QDir(root).filePath(QStringLiteral("current"));
    QFileInfo curInfo(current);
    if (curInfo.isSymLink()) QFile::remove(current);
    else if (curInfo.exists()) QDir(current).removeRecursively();
    if (!QDir().rename(packRoot, current)) { fail(QStringLiteral("could not install into %1").arg(current)); return; }
    m_staging.clear();

#if !defined(_WIN32)
    // Linux: symlink the denoiser feature lib onto the core's RPATH dir so the
    // core finds it by unversioned soname. (Windows co-locates every DLL in
    // bin/, which the loader already searches — nothing to link.)
    const QString featDir  = QDir(current).filePath(QStringLiteral("features/denoiser/lib"));
    const QString nvafxDir = QDir(current).filePath(QStringLiteral("nvafx/lib"));
    for (const QFileInfo& fi : QDir(featDir).entryInfoList(
             {QStringLiteral("libnv_audiofx_denoiser.so*")}, QDir::Files)) {
        const QString link = QDir(nvafxDir).filePath(fi.fileName());
        QFile::remove(link);
        QFile::link(fi.absoluteFilePath(), link);
    }
#endif

    writeReceipt(current);

    m_busy = false;
    if (installedPackDir().isEmpty()) { emit finished(false, QStringLiteral("install verification failed")); return; }
    emit progress(100, statusText());
    emit finished(true, statusText());
}

// Record each downloaded component's name/version/sha256 into the pack so the
// UI can show exactly what's installed. m_queue carries the pinned versions and
// (after resolve/verify) the actual sha256 of every component.
void NvidiaAfxPack::writeReceipt(const QString& packDir)
{
    QJsonArray arr;
    for (const Component& c : m_queue) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("version"), c.pypiVer);
        o.insert(QStringLiteral("sha256"), c.sha256);
        arr.append(o);
    }
    QFile f(QDir(packDir).filePath(QStringLiteral("components.json")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

QList<NvidiaAfxPack::ComponentInfo> NvidiaAfxPack::installedComponents()
{
    QList<ComponentInfo> out;
    const QString dir = installedPackDir();
    if (dir.isEmpty()) return out;
    QFile f(QDir(dir).filePath(QStringLiteral("components.json")));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue v : arr) {
        const QJsonObject o = v.toObject();
        out.append({ o.value(QStringLiteral("name")).toString(),
                     o.value(QStringLiteral("version")).toString(),
                     o.value(QStringLiteral("sha256")).toString() });
    }
    return out;
}

// ─── Offline single-archive import ───────────────────────────────────────────
void NvidiaAfxPack::installFromFile(const QString& archivePath)
{
    if (m_busy) return;
    if (!QFile::exists(archivePath)) { emit finished(false, QStringLiteral("archive not found")); return; }
    m_busy = true; m_cancelled = false;
    m_idx = 0;
    // Synthesize a one-entry queue so the install receipt records the imported
    // archive's sha256 (there are no separately-fetched components offline).
    QString importSha;
    {
        QFile a(archivePath);
        if (a.open(QIODevice::ReadOnly)) {
            QCryptographicHash h(QCryptographicHash::Sha256); h.addData(&a);
            importSha = QString::fromLatin1(h.result().toHex());
        }
    }
    m_queue = { { QStringLiteral("AFX pack (imported)"),
                  {}, QStringLiteral("2.1.0"), {}, importSha, Kind::Tarball } };
    QDir().mkpath(cacheRoot());
    m_staging = QDir(cacheRoot()).filePath(QStringLiteral(".staging"));
    QDir(m_staging).removeRecursively(); QDir().mkpath(m_staging);
    emit progress(-1, QStringLiteral("Extracting…"));
    extractInto(archivePath, Kind::Tarball);
    if (!m_busy) return;  // extractInto called fail()
    // A pre-assembled pack may have a single top-level dir — descend to it.
    if (!QFile::exists(QDir(m_staging).filePath(QString::fromLatin1(kCoreRelPath)))) {
        const QStringList subs = QDir(m_staging).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& s : subs) {
            const QString cand = QDir(m_staging).filePath(s);
            if (QFile::exists(QDir(cand).filePath(QString::fromLatin1(kCoreRelPath)))) { m_staging = cand; break; }
        }
    }
    assembleAndCommit();
}

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
