#pragma once

#ifdef HAVE_NVIDIA_AFX

#include <QObject>
#include <QString>
#include <QList>
#include <QElapsedTimer>
#include <QCryptographicHash>

class QFile;

class QNetworkAccessManager;
class QNetworkReply;

namespace AetherSDR {

// Download-on-demand cache for the NVIDIA AFX denoiser "pack" (AFX runtime libs
// + CUDA/TensorRT + the per-GPU denoiser model). Lives under the app data dir so
// the in-process NvidiaAfxFilter can dlopen it with no env var.
//
// v2 "split" sourcing — we host almost nothing:
//   * CUDA libs (cublas/cudart/cufft/nvrtc) come straight from NVIDIA's PyPI
//     wheels, anonymously. We pin (package, version); the wheel URL + sha256 are
//     resolved from the PyPI JSON API at runtime, so we get integrity for free
//     and don't hardcode volatile CDN paths.
//   * The AFX proprietary libs + TensorRT runtime libs + the denoiser model ship
//     as one small .tar.zst we host (NVIDIA redistributables we're permitted to
//     redistribute as part of the app).
// Components are fetched sequentially, extracted into a staging pack, then the
// pack is atomically swapped into place and the feature lib is symlinked onto
// the core's RPATH.
//
// install() runs the full v2 fetch; installFromFile() imports a single
// pre-assembled .tar.zst (offline / air-gapped). Network I/O is async; archive
// extraction runs via unzip/tar in a QProcess.
class NvidiaAfxPack : public QObject {
    Q_OBJECT
public:
    explicit NvidiaAfxPack(QObject* parent = nullptr);
    ~NvidiaAfxPack() override;

    static QString detectArch();          // "sm_89", or empty if no NVIDIA GPU
    // True if an NVIDIA GPU new enough for AFX at all is present — Ada /
    // RTX 40-series or later (compute capability >= 8.9) — regardless of whether
    // a pack is published for its exact arch. Broader than hasSupportedGpu():
    // a card can be AFX-capable yet unpublished (e.g. sm_120 / RTX 50-series). (#3933)
    static bool isAfxCapableGpu();
    // True only if an afx-bits pack is actually published for the detected GPU's
    // arch (a subset of isAfxCapableGpu()). The BNR UI enables the feature on this
    // so an AFX-capable-but-unpublished card (sm_120) doesn't dead-end on a 404.
    static bool hasSupportedGpu();
    static QString cacheRoot();           // <AppLocalData>/nvidia-afx
    static QString installedPackDir();    // cacheRoot/current if usable, else empty
    static bool isInstalled();
    static bool removeInstalled();

    // One downloaded component (AFX bundle / a CUDA wheel) with its pinned
    // version and the sha256 actually verified at install time.
    struct ComponentInfo {
        QString name;
        QString version;
        QString sha256;
        qint64  bytes = 0;   // download size (0 = unknown / not yet started)
        QString key;         // stable id for cache matching (survives label edits)
    };
    // Components recorded in the installed pack's receipt (components.json),
    // written at install. Empty if no pack / a pack predating the receipt.
    static QList<ComponentInfo> installedComponents();

    // Components already downloaded + verified into the resumable staging area
    // from a cancelled/interrupted install. A re-download skips these and only
    // fetches the missing ones. Empty if there's no partial download.
    static QList<ComponentInfo> stagedComponents();

    // The component versions this build of AetherSDR pins (name + version),
    // arch-independent. Used to hint "→ newer" and detect updates.
    QList<ComponentInfo> latestComponents() const;
    // True if an installed component's version differs from what this build
    // pins — i.e. updating the app shipped newer pinned versions, so a
    // Re-download would refresh the pack. False if not installed / no receipt /
    // an offline-imported pack (names won't match the standard manifest).
    bool updateAvailable() const;

    QString statusText() const;
    bool busy() const { return m_busy; }

    void install();                       // v2 multi-source fetch + assemble
    void installFromFile(const QString& archivePath);  // offline single-tarball
    void cancel();

signals:
    // The component list for this install, emitted once at the start so the UI
    // can lay out one row per component (versions known; sha/bytes fill in later).
    void planReady(const QList<ComponentInfo>& components);
    // Live progress for component `index`. percent <0 = indeterminate (resolving
    // / extracting); totalBytes is the download size once known; rateEta is the
    // "41 MB/s · 7s left" string (empty until enough data has moved).
    void componentProgress(int index, int percent, qint64 totalBytes, const QString& rateEta);
    // Component `index` is downloaded + verified + extracted — its row can swap
    // the bar for the version/sha/size detail line.
    void componentFinished(int index, const ComponentInfo& info);
    void finished(bool ok, const QString& message);

private:
    enum class Kind { Wheel, Tarball };
    struct Component {
        QString key;        // STABLE id for cache matching (never change once shipped)
        QString name;       // display name (free to change — not used for matching)
        QString pypiPkg;    // for Wheel: PyPI package (url+sha resolved at runtime)
        QString pypiVer;    // pinned version (display)
        QString pypiIndex;  // for Wheel: empty=pypi.org JSON, else simple-index base
                            //   (e.g. "https://pypi.nvidia.com/" for TensorRT)
        QString url;        // for Tarball: direct URL (our host)
        QString sha256;     // for Tarball: pinned sha (Wheel sha comes from PyPI)
        Kind kind;
        qint64 bytes = 0;   // download size, recorded as it completes (for receipt)
    };
    QList<Component> manifest(const QString& arch) const;

    void startNext();                                     // process m_queue[m_idx]
    void resolveWheelUrl(const Component& c);             // PyPI JSON -> url+sha
    void downloadTo(const QUrl& url, const QString& sha256,
                    const QString& dest, const QString& label);
    // Pure worker run off the GUI thread (QtConcurrent) so decompression doesn't
    // freeze the UI. Returns an empty string on success, else an error message.
    static QString runExtract(const QString& archive, Kind kind,
                              const QString& staging, const QString& cacheRoot);
    void assembleAndCommit();                             // symlink + atomic swap
    void writeReceipt(const QString& packDir);           // components.json from m_queue
    void writeStagingProgress();                          // .staging/.progress.json
    bool isCompleted(const Component& c) const;           // already in m_done?
    // Match a manifest component to a recorded one by stable key + version,
    // falling back to display name for entries written before keys existed.
    static bool sameComponent(const Component& c, const ComponentInfo& d);
    void fail(const QString& msg);
    QList<ComponentInfo> plannedComponents() const;       // m_queue -> ComponentInfo list
    static QString stagingPath();                         // cacheRoot()/.staging

    QNetworkAccessManager* m_nam{nullptr};
    QNetworkReply* m_reply{nullptr};
    QList<Component> m_queue;
    int m_idx{0};
    QString m_arch;
    QString m_staging;       // staging pack root being assembled
    QString m_tmpFile;       // current download temp
    QFile*  m_dlFile{nullptr};  // open handle for the in-flight download (closed on cancel)
    QCryptographicHash m_dlHash{QCryptographicHash::Sha256};  // streamed as bytes arrive
    bool    m_dlWriteFailed{false};  // a write() to m_dlFile came up short (disk full)
    QElapsedTimer m_dlTimer; // current component download timer (speed/ETA)
    QList<ComponentInfo> m_done;  // components completed into staging (resumable)
    bool m_busy{false};
    bool m_cancelled{false};
};

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
