#ifdef HAVE_NVIDIA_AFX

#include "NvidiaAfxPack.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QHash>
#include <QtConcurrent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

// ─── Platform layout ─────────────────────────────────────────────────────────
// Linux and Windows ship different AFX runtimes but the SAME split-sourcing
// strategy (host almost nothing; pull CUDA from NVIDIA's PyPI wheels):
//   • Linux: core nvafx/lib/libnv_audiofx.so. AFX/TRT/model from our small
//     .tar.zst; CUDA from PyPI wheels (→ external/cuda/lib); feature lib
//     symlinked onto the core's RPATH.
//   • Windows: core bin/NVAudioEffects.dll. A small .zip ships only the AFX bits
//     (core + denoiser feature DLL + OpenSSL + model); CUDA from PyPI wheels and
//     the TensorRT inference DLL from a separate hosted .zip — all flattened into
//     bin/, which the loader searches via LOAD_WITH_ALTERED_SEARCH_PATH (so no
//     symlink step). Hosting the slim AFX zip keeps it a ~33 MB release asset.
namespace {
#if defined(_WIN32)
constexpr char kCoreRelPath[] = "bin/NVAudioEffects.dll";
constexpr char kPlatformTag[] = "windows-x86_64";
// Pinned sha256 of the published afx-bits-2.1.0-windows-x86_64 zip.
constexpr char kWinTarballSha[] = "55e0a35bed70ade2e3b80d463c660da6b749223a998843f176af7da2d689a899";
// pypi.nvidia.com's tensorrt-cu12-libs wheel is 1.6 GB because it bundles the
// builder + plugins + ONNX parser. Maxine AFX uses a pre-baked .trtpkg engine
// and only needs the inference runtime (nvinfer_<ver>.dll, ~420 MB raw).
// Ship that one DLL ourselves so user-side download matches Linux footprint.
constexpr char kWinTensorrtVer[] = "10.9.0.34";
constexpr char kWinTensorrtSha[] = "3b5f3c3d774fd7fedb57a606f76a68d372725a0343d502917d829479d27960c2";
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
    if (secs < 60)   { return QStringLiteral("%1s").arg(secs); }
    if (secs < 3600) { return QStringLiteral("%1m %2s").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')); }
    return QStringLiteral("%1h %2m").arg(secs / 3600).arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'));
}

// Shared component-receipt JSON I/O (used by both the staging progress file and
// the installed components.json — same schema, one implementation).
QList<NvidiaAfxPack::ComponentInfo> readComponentJson(const QString& path)
{
    QList<NvidiaAfxPack::ComponentInfo> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { return out; }
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue v : arr) {
        const QJsonObject o = v.toObject();
        out.append({ o.value(QStringLiteral("name")).toString(),
                     o.value(QStringLiteral("version")).toString(),
                     o.value(QStringLiteral("sha256")).toString(),
                     static_cast<qint64>(o.value(QStringLiteral("bytes")).toDouble()),
                     o.value(QStringLiteral("key")).toString() });
    }
    return out;
}

void writeComponentJson(const QString& path,
                        const QList<NvidiaAfxPack::ComponentInfo>& comps,
                        QJsonDocument::JsonFormat fmt)
{
    QJsonArray arr;
    for (const NvidiaAfxPack::ComponentInfo& c : comps) {
        QJsonObject o;
        o.insert(QStringLiteral("key"), c.key);
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("version"), c.version);
        o.insert(QStringLiteral("sha256"), c.sha256);
        o.insert(QStringLiteral("bytes"), static_cast<double>(c.bytes));
        arr.append(o);
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(fmt));
    }
}
} // namespace

// ─── Static helpers ──────────────────────────────────────────────────────────
QString NvidiaAfxPack::detectArch()
{
    // The GPU's compute capability can't change during a session — query
    // nvidia-smi once and cache it (the call blocks up to 4s on a subprocess).
    static QString cached;
    static bool resolved = false;
    if (resolved)
        return cached;
    resolved = true;
    QProcess p;
    p.start(QStringLiteral("nvidia-smi"),
            {QStringLiteral("--query-gpu=compute_cap"), QStringLiteral("--format=csv,noheader")});
    if (!p.waitForFinished(4000) || p.exitStatus() != QProcess::NormalExit)
        return cached;
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        const QString digits = line.trimmed().remove('.');   // "8.9" -> "89"
        bool ok = false;
        const int v = digits.toInt(&ok);
        if (ok && v >= 75) {                                  // Turing+
            cached = QStringLiteral("sm_%1").arg(digits);
            break;
        }
    }
    return cached;
}

// Compute capabilities (sm_<cc>) for which an afx-bits pack is actually published
// on our releases. detectArch() can report a newer NVIDIA card — e.g. sm_120
// (consumer Blackwell / RTX 50-series) — that clears the Ada+ bar but has no
// published pack yet, so it must not dead-end at a Download that 404s. Keep this
// in sync with the afx-bits release assets + the build-afx-bits ValidateSet. (#3933)
static const QList<int> kPublishedComputeCaps = { 89 };   // sm_89 (Ada) — Linux + Windows

// Compute capability of the detected NVIDIA GPU (89 for "sm_89"), or -1 if none.
static int detectedComputeCap()
{
    const QString arch = NvidiaAfxPack::detectArch();
    if (!arch.startsWith(QStringLiteral("sm_")))
        return -1;
    bool ok = false;
    const int cc = QStringView{arch}.mid(3).toInt(&ok);   // "sm_89" -> 89
    return ok ? cc : -1;
}

bool NvidiaAfxPack::isAfxCapableGpu()
{
    // detectArch() returns "sm_<cc>" for Turing+ (>=75); AFX itself needs Ada
    // (sm_89 = RTX 40-series) or later. Earlier RTX (20/30) and non-NVIDIA
    // machines fail this. This says nothing about whether a *pack* is published.
    return detectedComputeCap() >= 89;
}

bool NvidiaAfxPack::hasSupportedGpu()
{
    // BNR is only usable when an afx-bits pack is actually published for the
    // detected GPU's arch — not merely when the GPU is new enough for AFX. An
    // AFX-capable card with no published pack (e.g. sm_120) is reported distinctly
    // by the UI and steered to DFNR instead of a 404 Download. (#3933)
    return isAfxCapableGpu() && kPublishedComputeCaps.contains(detectedComputeCap());
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
    if (m_busy) { return QStringLiteral("Working…"); }
    const QString dir = installedPackDir();
    if (dir.isEmpty()) { return QStringLiteral("Not installed"); }
    const QDir models(QDir(dir).filePath(QStringLiteral("features/denoiser/models")));
    const QStringList sm = models.entryList({QStringLiteral("sm_*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    return sm.isEmpty() ? QStringLiteral("Installed") : QStringLiteral("Installed (%1)").arg(sm.first());
}

bool NvidiaAfxPack::removeInstalled()
{
    // Also clear any resumable staging so a removal is a clean slate.
    QDir(stagingPath()).removeRecursively();
    const QString cur = QDir(cacheRoot()).filePath(QStringLiteral("current"));
    QFileInfo fi(cur);
    if (fi.isSymLink()) { return QFile::remove(cur); }
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

    // NOTE: the first field is a STABLE key used for cache/resume matching —
    // never change it once shipped (display names in the 2nd field may change).
#if defined(_WIN32)
    // Windows: split sourcing matches the Linux design. Our small .zip ships
    // only the AFX-specific bits (AFX core, denoiser feature DLL, OpenSSL, model)
    // — ~33 MB. CUDA libs come from pypi.org wheels. The TensorRT inference
    // runtime (nvinfer_<ver>.dll) ships as a separate ~220 MB tarball on our
    // release — using NVIDIA's pypi.nvidia.com wheel would be 1.6 GB because
    // it bundles the builder/plugins/ONNX-parser that Maxine AFX doesn't use.
    const QString trtUrl =
        QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/download/"
                       "afx-bits-2.1.0/afx-bits-2.1.0-windows-x86_64-tensorrt-%1.zip")
            .arg(QString::fromLatin1(kWinTensorrtVer));
    return {
        { QStringLiteral("afx"), QStringLiteral("AFX runtime"),
          {}, QStringLiteral("2.1.0"), {}, afxUrl,
          QString::fromLatin1(kWinTarballSha), Kind::Tarball },
        { QStringLiteral("cuda-runtime"), QStringLiteral("CUDA runtime"), QStringLiteral("nvidia-cuda-runtime-cu12"), QStringLiteral("12.8.90"),  {}, {}, {}, Kind::Wheel },
        { QStringLiteral("cublas"),       QStringLiteral("cuBLAS"),       QStringLiteral("nvidia-cublas-cu12"),       QStringLiteral("12.8.4.1"),  {}, {}, {}, Kind::Wheel },
        { QStringLiteral("cufft"),        QStringLiteral("cuFFT"),        QStringLiteral("nvidia-cufft-cu12"),        QStringLiteral("11.3.3.83"), {}, {}, {}, Kind::Wheel },
        { QStringLiteral("nvrtc"),        QStringLiteral("nvRTC"),        QStringLiteral("nvidia-cuda-nvrtc-cu12"),   QStringLiteral("12.8.93"),  {}, {}, {}, Kind::Wheel },
        { QStringLiteral("tensorrt"),     QStringLiteral("TensorRT runtime"),
          {}, QString::fromLatin1(kWinTensorrtVer), {}, trtUrl,
          QString::fromLatin1(kWinTensorrtSha), Kind::Tarball },
    };
#else
    // Linux: AFX/TRT/model from our tarball; CUDA libs from NVIDIA's PyPI wheels.
    return {
        { QStringLiteral("afx"), QStringLiteral("AFX runtime"),
          {}, QStringLiteral("2.1.0"), {}, afxUrl,
          QStringLiteral("0bfe85b0faeb322958303c145996350d0fea8a203899f9215fc0d3a341395b67"),
          Kind::Tarball },
        { QStringLiteral("cuda-runtime"), QStringLiteral("CUDA runtime"), QStringLiteral("nvidia-cuda-runtime-cu12"), QStringLiteral("12.8.90"),  {}, {}, {}, Kind::Wheel },
        { QStringLiteral("cublas"),       QStringLiteral("cuBLAS"),       QStringLiteral("nvidia-cublas-cu12"),       QStringLiteral("12.8.4.1"),  {}, {}, {}, Kind::Wheel },
        { QStringLiteral("cufft"),        QStringLiteral("cuFFT"),        QStringLiteral("nvidia-cufft-cu12"),        QStringLiteral("11.3.3.83"), {}, {}, {}, Kind::Wheel },
        { QStringLiteral("nvrtc"),        QStringLiteral("nvRTC"),        QStringLiteral("nvidia-cuda-nvrtc-cu12"),   QStringLiteral("12.8.93"),  {}, {}, {}, Kind::Wheel },
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
    if (m_reply) {
        // Drop our slots BEFORE abort(): abort() emits finished() synchronously,
        // and when cancel() runs from ~NvidiaAfxPack (the AetherDSP window closed
        // mid-download), re-entering the finished handler touches a half-destroyed
        // pack and emits back at the dying widget — that's the close-mid-download
        // crash. Disconnected, abort() is inert.
        m_reply->disconnect();
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    // Close the open download handle BEFORE removing the temp file — an open
    // handle blocks QFile::remove() on Windows, leaking the partial file.
    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
    // Remove only the in-flight partial download; KEEP staging so already-
    // completed components survive for a resumable re-download.
    if (!m_tmpFile.isEmpty()) { QFile::remove(m_tmpFile); m_tmpFile.clear(); }
    m_busy = false;
}

void NvidiaAfxPack::fail(const QString& msg)
{
    // Drop only the partial download; completed components stay staged so a
    // retry resumes instead of starting over.
    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
    if (!m_tmpFile.isEmpty()) { QFile::remove(m_tmpFile); m_tmpFile.clear(); }
    m_busy = false;
    emit finished(false, msg);
}

QList<NvidiaAfxPack::ComponentInfo> NvidiaAfxPack::plannedComponents() const
{
    QList<ComponentInfo> out;
    for (const Component& c : m_queue)
        out.append({ c.name, c.pypiVer, c.sha256, c.bytes, c.key });
    return out;
}

QList<NvidiaAfxPack::ComponentInfo> NvidiaAfxPack::latestComponents() const
{
    // Versions are arch-independent (arch only varies the model file inside the
    // AFX bundle, not the pinned versions), so any arch tag yields the same list.
    QList<ComponentInfo> out;
    for (const Component& c : manifest(QStringLiteral("any")))
        out.append({ c.name, c.pypiVer, {}, 0, c.key });
    return out;
}

bool NvidiaAfxPack::updateAvailable() const
{
    const auto installed = installedComponents();
    if (installed.isEmpty()) return false;            // not installed / no receipt
    QHash<QString, QString> byKey, byName;            // -> installed version
    for (const auto& c : installed) {
        if (!c.key.isEmpty()) { byKey.insert(c.key, c.version); }
        byName.insert(c.name, c.version);
    }
    // An installed component whose version differs, OR a manifest component the
    // installed pack lacks entirely (a newly-added component, e.g. the Windows
    // TensorRT runtime), both mean the pack is out of date. Exception: a single
    // offline-imported entry (key "afx", name "AFX pack (imported)") never
    // carries the full component set, so don't nag those — treat as up to date.
    const bool imported = installed.size() == 1
                          && installed.first().name.contains(QStringLiteral("imported"));
    if (imported) { return false; }
    for (const auto& c : latestComponents()) {
        if (!c.key.isEmpty() && byKey.contains(c.key)) {
            if (byKey.value(c.key) != c.version) { return true; }
        } else if (byName.contains(c.name)) {
            if (byName.value(c.name) != c.version) { return true; }
        } else {
            return true;   // manifest has a component the installed pack lacks
        }
    }
    return false;
}

// ─── Resumable staging ───────────────────────────────────────────────────────
QString NvidiaAfxPack::stagingPath()
{
    return QDir(cacheRoot()).filePath(QStringLiteral(".staging"));
}

// Components already downloaded + verified into staging (a .progress.json
// written as each one completes). Survives cancel so a re-download resumes.
QList<NvidiaAfxPack::ComponentInfo> NvidiaAfxPack::stagedComponents()
{
    return readComponentJson(QDir(stagingPath()).filePath(QStringLiteral(".progress.json")));
}

void NvidiaAfxPack::writeStagingProgress()
{
    writeComponentJson(QDir(m_staging).filePath(QStringLiteral(".progress.json")),
                       m_done, QJsonDocument::Compact);
}

bool NvidiaAfxPack::sameComponent(const Component& c, const ComponentInfo& d)
{
    if (d.version != c.pypiVer) { return false; }
    if (!c.key.isEmpty() && !d.key.isEmpty())
        return d.key == c.key;          // stable-key match (survives label edits)
    return d.name == c.name;            // fallback for pre-key receipts/progress
}

bool NvidiaAfxPack::isCompleted(const Component& c) const
{
    for (const ComponentInfo& d : m_done)
        if (sameComponent(c, d))
            return true;
    return false;
}

// ─── v2 multi-source install ─────────────────────────────────────────────────
void NvidiaAfxPack::install()
{
    if (m_busy) { return; }
    m_arch = detectArch();
    if (m_arch.isEmpty()) { emit finished(false, QStringLiteral("no supported NVIDIA GPU found")); return; }
    m_busy = true; m_cancelled = false;
    QDir().mkpath(cacheRoot());
    m_staging = stagingPath();
    m_queue = manifest(m_arch);
    m_done = stagedComponents();

    // Reuse existing staging (resume) ONLY if every staged component still
    // matches the current manifest. If a staged component's version moved on, or
    // it's no longer in the manifest (an app update changed pins), the staging
    // tree may hold stale files that tar -C would not overwrite — discard it and
    // start clean rather than committing a mixed-version pack.
    bool stagingStale = false;
    for (const ComponentInfo& d : m_done) {
        bool stillWanted = false;
        for (const Component& c : m_queue) {
            if (sameComponent(c, d)) { stillWanted = true; break; }
        }
        if (!stillWanted) { stagingStale = true; break; }
    }
    if (stagingStale) {
        QDir(m_staging).removeRecursively();
        m_done.clear();
    }

    // Reuse any surviving staging (resume): keep already-downloaded components
    // and only fetch the missing ones.
    QDir().mkpath(m_staging);
#if !defined(_WIN32)
    // Linux flattens CUDA wheel .so files here; Windows DLLs ride in the zip.
    QDir().mkpath(QDir(m_staging).filePath(QStringLiteral("external/cuda/lib")));
#endif
    m_idx = 0;
    emit planReady(plannedComponents());
    startNext();
}

void NvidiaAfxPack::startNext()
{
    if (m_cancelled) { m_busy = false; return; }   // keep staging for resume
    if (m_idx >= m_queue.size()) { assembleAndCommit(); return; }
    Component& c = m_queue[m_idx];
    if (isCompleted(c)) {
        // Already downloaded + extracted in a prior run — restore its recorded
        // sha/bytes and mark the row done without re-fetching.
        for (const ComponentInfo& d : m_done) {
            if (sameComponent(c, d)) {
                c.sha256 = d.sha256; c.bytes = d.bytes;
                emit componentProgress(m_idx, 100, d.bytes, QString());
                emit componentFinished(m_idx, { c.name, c.pypiVer, d.sha256, d.bytes, c.key });
                break;
            }
        }
        ++m_idx;
        startNext();
        return;
    }
    if (c.kind == Kind::Wheel)
        resolveWheelUrl(c);                        // PyPI JSON -> url+sha -> download
    else
        downloadTo(QUrl(c.url), c.sha256,
                   QDir(cacheRoot()).filePath(QStringLiteral(".dl.tar.zst")), c.name);
}

// Resolve the platform-correct wheel URL + sha256 for the current OS:
//   * Linux  → manylinux*_x86_64.whl
//   * Windows → win_amd64.whl
// Source is the standard pypi.org JSON API unless the component pins a
// simple-index URL (pypi.nvidia.com hosts TensorRT but only via the HTML
// simple index — no JSON API), in which case we parse anchors of the form
// `<a href="<file>.whl#sha256=...">`.
void NvidiaAfxPack::resolveWheelUrl(const Component& c)
{
    emit componentProgress(m_idx, -1, 0, QStringLiteral("resolving…"));
#if defined(_WIN32)
    auto matchesPlatform = [](const QString& fn) {
        return fn.endsWith(QStringLiteral(".whl")) && fn.contains(QStringLiteral("win_amd64"));
    };
    const QString platformDesc = QStringLiteral("win_amd64");
#else
    auto matchesPlatform = [](const QString& fn) {
        return fn.endsWith(QStringLiteral(".whl"))
               && fn.contains(QStringLiteral("x86_64"))
               && fn.contains(QStringLiteral("manylinux"));
    };
    const QString platformDesc = QStringLiteral("manylinux x86_64");
#endif

    if (!c.pypiIndex.isEmpty()) {
        // Simple-index HTML — used for tensorrt-cu12-libs on pypi.nvidia.com.
        // Format: <a href="<filename>#sha256=<hex>">…</a>. The href is relative
        // to the index URL.
        const QString indexUrl = c.pypiIndex
                                 + (c.pypiIndex.endsWith(QLatin1Char('/'))
                                        ? QString() : QStringLiteral("/"))
                                 + c.pypiPkg + QStringLiteral("/");
        QNetworkRequest req{QUrl(indexUrl)};   // braces to avoid most-vexing parse
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        m_reply = m_nam->get(req);
        connect(m_reply, &QNetworkReply::finished, this,
                [this, name = c.name, pkg = c.pypiPkg, ver = c.pypiVer,
                 indexUrl, platformDesc, matchesPlatform]() {
            if (!m_reply) { return; }
            const QByteArray body = m_reply->readAll();
            const auto err = m_reply->error();
            m_reply->deleteLater(); m_reply = nullptr;
            if (err != QNetworkReply::NoError) { fail(QStringLiteral("simple-index lookup failed for %1").arg(name)); return; }
            // Match anchors: href="<filename-with-version>#sha256=<hex>"
            // We pin a specific version, so embed it in the regex to skip others fast.
            // The version appears between the package name and the next dash, e.g.
            //   tensorrt_cu12_libs-10.9.0.34-py2.py3-none-win_amd64.whl
            // Package name in filenames uses underscores not hyphens.
            const QString pkgUnderscored = QString(pkg).replace(QLatin1Char('-'), QLatin1Char('_'));
            const QRegularExpression re(
                QStringLiteral("href=\"([^\"#]*%1-%2[^\"#]*\\.whl)#sha256=([0-9a-f]{64})\"")
                    .arg(QRegularExpression::escape(pkgUnderscored),
                         QRegularExpression::escape(ver)));
            const QString html = QString::fromUtf8(body);
            QRegularExpressionMatchIterator it = re.globalMatch(html);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                const QString fn = m.captured(1);
                if (!matchesPlatform(fn)) { continue; }
                const QString sha = m.captured(2);
                // href is relative to the index dir.
                const QUrl base(indexUrl);
                const QUrl wheelUrl = base.resolved(QUrl(fn));
                if (m_idx < m_queue.size()) { m_queue[m_idx].sha256 = sha; }
                downloadTo(wheelUrl, sha,
                           QDir(cacheRoot()).filePath(QStringLiteral(".dl.whl")), name);
                return;
            }
            fail(QStringLiteral("no %1 wheel for %2 %3 at %4").arg(platformDesc, pkg, ver, indexUrl));
        });
        return;
    }

    // pypi.org JSON path.
    const QUrl api(QStringLiteral("https://pypi.org/pypi/%1/%2/json").arg(c.pypiPkg, c.pypiVer));
    QNetworkRequest req(api);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Track in m_reply so cancel() (e.g. window closed mid-resolve) aborts it.
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this,
            [this, name = c.name, platformDesc, matchesPlatform]() {
        if (!m_reply) { return; }
        const QByteArray body = m_reply->readAll();
        const auto err = m_reply->error();
        m_reply->deleteLater(); m_reply = nullptr;
        if (err != QNetworkReply::NoError) { fail(QStringLiteral("PyPI lookup failed for %1").arg(name)); return; }
        const QJsonObject root = QJsonDocument::fromJson(body).object();
        for (const QJsonValue v : root.value(QStringLiteral("urls")).toArray()) {
            const QJsonObject u = v.toObject();
            const QString fn = u.value(QStringLiteral("filename")).toString();
            if (matchesPlatform(fn)) {
                const QString url = u.value(QStringLiteral("url")).toString();
                const QString sha = u.value(QStringLiteral("digests")).toObject()
                                       .value(QStringLiteral("sha256")).toString();
                // Record the resolved sha so the install receipt can report it.
                if (m_idx < m_queue.size()) { m_queue[m_idx].sha256 = sha; }
                downloadTo(QUrl(url), sha,
                           QDir(cacheRoot()).filePath(QStringLiteral(".dl.whl")), name);
                return;
            }
        }
        fail(QStringLiteral("no %1 wheel for %2").arg(platformDesc, name));
    });
}

void NvidiaAfxPack::downloadTo(const QUrl& url, const QString& sha256,
                               const QString& dest, const QString& label)
{
    m_tmpFile = dest;
    // Tracked in a member so cancel() can close the handle before removing the
    // temp file — on Windows an open handle blocks QFile::remove().
    m_dlFile = new QFile(dest, this);
    if (!m_dlFile->open(QIODevice::WriteOnly)) {
        delete m_dlFile; m_dlFile = nullptr;
        fail(QStringLiteral("cannot write cache"));
        return;
    }
    m_dlHash.reset();   // hash incrementally as bytes arrive (no post-download re-read)
    m_dlWriteFailed = false;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    m_dlTimer.start();
    emit componentProgress(m_idx, 0, 0, QString());
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_reply->readAll();
        if (m_dlFile && m_dlFile->write(chunk) != chunk.size()) {
            // Short write (disk full / quota): stop now and flag it. Otherwise
            // the running hash — which sees the full network chunk — would still
            // match the pinned sha and "verify" bytes that never hit disk,
            // leaving a truncated archive that fails later with a misleading
            // "extract failed". Abort so finished() reports the real cause.
            m_dlWriteFailed = true;
            m_reply->abort();
            return;
        }
        m_dlHash.addData(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        // Average rate since this file started — smoother than per-tick deltas.
        const qint64 ms = m_dlTimer.elapsed();
        QString rateEta;
        if (ms > 500 && got > 0) {
            const double bps = got * 1000.0 / double(ms);
            rateEta = humanRate(bps);
            if (total > got && bps > 1.0)
                rateEta += QStringLiteral(" · %1 left").arg(humanEta(qint64((total - got) / bps)));
        }
        emit componentProgress(m_idx, total > 0 ? int(got * 100 / total) : -1, total, rateEta);
    });
    connect(m_reply, &QNetworkReply::finished, this,
            [this, sha256, label, dest]() {
        if (m_dlFile) { m_dlFile->flush(); m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
        const auto err = m_reply->error();
        const QString es = m_reply->errorString();
        m_reply->deleteLater(); m_reply = nullptr;
        if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
        // Checked before the network-error branch: abort() above also sets an
        // error, but the disk-full cause is the one worth reporting.
        if (m_dlWriteFailed) { fail(QStringLiteral("write failed for %1 (disk full?)").arg(label)); return; }
        if (err != QNetworkReply::NoError) { fail(QStringLiteral("download failed (%1): %2").arg(label, es)); return; }
        if (!sha256.isEmpty()
            && m_dlHash.result().toHex() != sha256.toLatin1()) {
            // A mismatch is not transient (the bytes are pinned) — say so rather
            // than inviting an endless re-download of the same bad artifact.
            fail(QStringLiteral("checksum mismatch for %1 (corrupt or stale mirror)").arg(label));
            return;
        }
        const Kind kind = m_queue[m_idx].kind;
        const qint64 bytes = QFileInfo(dest).size();
        m_queue[m_idx].bytes = bytes;                  // recorded into the receipt
        emit componentProgress(m_idx, -1, bytes, QStringLiteral("extracting…"));
        // Extract off the GUI thread so decompression doesn't freeze the UI.
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, dest, bytes]() {
            const QString err = watcher->result();
            watcher->deleteLater();
            QFile::remove(dest); m_tmpFile.clear();
            if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
            if (!err.isEmpty()) { fail(err); return; }
            const Component& c = m_queue[m_idx];
            const ComponentInfo info{ c.name, c.pypiVer, c.sha256, bytes, c.key };
            // Persist into the resumable staging manifest so a cancel keeps it.
            m_done.removeIf([&](const ComponentInfo& d) {
                return d.key.isEmpty() ? d.name == info.name : d.key == info.key; });
            m_done.append(info);
            writeStagingProgress();
            emit componentFinished(m_idx, info);
            ++m_idx;
            startNext();
        });
        watcher->setFuture(QtConcurrent::run(&NvidiaAfxPack::runExtract,
                                             dest, kind, m_staging, cacheRoot()));
    });
}

// Pure worker — runs on a QtConcurrent thread (no member access, no signals) so
// decompression doesn't block the GUI event loop. Returns "" on success.
QString NvidiaAfxPack::runExtract(const QString& archive, Kind kind,
                                  const QString& staging,
                                  [[maybe_unused]] const QString& cacheRoot)
{
    QProcess p;
    if (kind == Kind::Wheel) {
#if defined(_WIN32)
        // Wheels are zip files. tar.exe (bsdtar, ships in Win10+) extracts
        // zips but has no "flatten paths" mode. So extract into a temp dir
        // then move all *.dll up into staging/bin/. CUDA wheels put DLLs at
        // nvidia/<pkg>/bin/*.dll; tensorrt-cu12-libs at tensorrt_libs/*.dll.
        const QString binDir = QDir(staging).filePath(QStringLiteral("bin"));
        QDir().mkpath(binDir);
        const QString tmpDir = QDir(cacheRoot).filePath(QStringLiteral(".whl-extract"));
        QDir(tmpDir).removeRecursively();
        QDir().mkpath(tmpDir);
        p.start(QStringLiteral("tar"),
                {QStringLiteral("-xf"), archive, QStringLiteral("-C"), tmpDir});
        if (!p.waitForFinished(600000) || p.exitCode() != 0) {
            return QStringLiteral("wheel extract failed: %1")
                       .arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        }
        // Flatten: move any *.dll from anywhere in the wheel into bin/.
        QDirIterator it(tmpDir, {QStringLiteral("*.dll")},
                        QDir::Files, QDirIterator::Subdirectories);
        int moved = 0;
        while (it.hasNext()) {
            const QString src = it.next();
            const QString dst = QDir(binDir).filePath(QFileInfo(src).fileName());
            QFile::remove(dst);  // idempotent: overwrite on retry
            if (QFile::rename(src, dst)) { ++moved; }
        }
        QDir(tmpDir).removeRecursively();
        if (moved == 0) {
            return QStringLiteral("wheel %1 contained no DLLs").arg(QFileInfo(archive).fileName());
        }
        return {};  // success
#else
        // Linux: flatten the wheel's *.so* into the pack's external/cuda/lib.
        const QString libDir = QDir(staging).filePath(QStringLiteral("external/cuda/lib"));
        p.start(QStringLiteral("unzip"),
                {QStringLiteral("-o"), QStringLiteral("-j"), QStringLiteral("-d"), libDir,
                 archive, QStringLiteral("*.so*")});
#endif
    } else {
#if defined(_WIN32)
        // Windows AFX-bits is a self-contained .zip laid out at root (bin/,
        // features/). bsdtar (tar.exe, shipped with Windows 10+) reads zip.
        p.start(QStringLiteral("tar"),
                {QStringLiteral("-xf"), archive, QStringLiteral("-C"), staging});
#else
        // Our AFX-bits tarball is laid out at root (nvafx/, features/, external/).
        p.start(QStringLiteral("tar"),
                {QStringLiteral("--zstd"), QStringLiteral("-xf"), archive,
                 QStringLiteral("-C"), staging});
#endif
    }
    if (!p.waitForFinished(600000) || p.exitCode() != 0) {
        return QStringLiteral("extract failed: %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
    }
    return {};  // success
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
    // Drop the resume marker so it doesn't ride into the committed pack.
    QFile::remove(QDir(packRoot).filePath(QStringLiteral(".progress.json")));
    const QString current = QDir(root).filePath(QStringLiteral("current"));
    QFileInfo curInfo(current);
    if (curInfo.isSymLink()) { QFile::remove(current); }
    else if (curInfo.exists()) { QDir(current).removeRecursively(); }
    if (!QDir().rename(packRoot, current)) { fail(QStringLiteral("could not install into %1").arg(current)); return; }
    m_staging.clear();
    m_done.clear();   // staging consumed — next install starts fresh

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
    emit finished(true, statusText());
}

// Record each downloaded component's name/version/sha256 into the pack so the
// UI can show exactly what's installed. m_queue carries the pinned versions and
// (after resolve/verify) the actual sha256 of every component.
void NvidiaAfxPack::writeReceipt(const QString& packDir)
{
    writeComponentJson(QDir(packDir).filePath(QStringLiteral("components.json")),
                       plannedComponents(), QJsonDocument::Indented);
}

QList<NvidiaAfxPack::ComponentInfo> NvidiaAfxPack::installedComponents()
{
    const QString dir = installedPackDir();
    if (dir.isEmpty()) { return {}; }
    return readComponentJson(QDir(dir).filePath(QStringLiteral("components.json")));
}

// ─── Offline single-archive import ───────────────────────────────────────────
void NvidiaAfxPack::installFromFile(const QString& archivePath)
{
    if (m_busy) { return; }
    if (!QFile::exists(archivePath)) { emit finished(false, QStringLiteral("archive not found")); return; }
    m_busy = true; m_cancelled = false;
    m_idx = 0;
    m_done.clear();   // offline import is always a fresh, single-archive install
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
    const qint64 importBytes = QFileInfo(archivePath).size();
    m_queue = { { QStringLiteral("afx"), QStringLiteral("AFX pack (imported)"),
                  {}, QStringLiteral("2.1.0"), {}, {}, importSha, Kind::Tarball, importBytes } };
    QDir().mkpath(cacheRoot());
    m_staging = QDir(cacheRoot()).filePath(QStringLiteral(".staging"));
    QDir(m_staging).removeRecursively(); QDir().mkpath(m_staging);
    emit planReady(plannedComponents());
    emit componentProgress(0, -1, importBytes, QStringLiteral("extracting…"));
    // Extract off the GUI thread, then assemble on completion.
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, importBytes]() {
        const QString err = watcher->result();
        watcher->deleteLater();
        if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
        if (!err.isEmpty()) { fail(err); return; }
        emit componentFinished(0, { m_queue[0].name, m_queue[0].pypiVer,
                                    m_queue[0].sha256, importBytes, m_queue[0].key });
        // A pre-assembled pack may have a single top-level dir — descend to it.
        if (!QFile::exists(QDir(m_staging).filePath(QString::fromLatin1(kCoreRelPath)))) {
            const QStringList subs = QDir(m_staging).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& s : subs) {
                const QString cand = QDir(m_staging).filePath(s);
                if (QFile::exists(QDir(cand).filePath(QString::fromLatin1(kCoreRelPath)))) { m_staging = cand; break; }
            }
        }
        assembleAndCommit();
    });
    watcher->setFuture(QtConcurrent::run(&NvidiaAfxPack::runExtract,
                                         archivePath, Kind::Tarball, m_staging, cacheRoot()));
}

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
