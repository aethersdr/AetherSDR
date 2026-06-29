#pragma once

#ifdef HAVE_NVIDIA_AFX

#include <QObject>
#include <QString>
#include <QList>

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
    };
    // Components recorded in the installed pack's receipt (components.json),
    // written at install. Empty if no pack / a pack predating the receipt.
    static QList<ComponentInfo> installedComponents();

    QString statusText() const;
    bool busy() const { return m_busy; }

    void install();                       // v2 multi-source fetch + assemble
    void installFromFile(const QString& archivePath);  // offline single-tarball
    void cancel();

signals:
    void progress(int percent, const QString& status);  // percent <0 = indeterminate
    void finished(bool ok, const QString& message);

private:
    enum class Kind { Wheel, Tarball };
    struct Component {
        QString name;       // display name
        QString pypiPkg;    // for Wheel: PyPI package (url+sha resolved at runtime)
        QString pypiVer;    // for Wheel: pinned version
        QString url;        // for Tarball: direct URL (our host)
        QString sha256;     // for Tarball: pinned sha (Wheel sha comes from PyPI)
        Kind kind;
    };
    QList<Component> manifest(const QString& arch) const;

    void startNext();                                     // process m_queue[m_idx]
    void resolveWheelUrl(const Component& c);             // PyPI JSON -> url+sha
    void downloadTo(const QUrl& url, const QString& sha256,
                    const QString& dest, const QString& label);
    void extractInto(const QString& archive, Kind kind); // unzip *.so* / tar zst
    void assembleAndCommit();                             // symlink + atomic swap
    void writeReceipt(const QString& packDir);           // components.json from m_queue
    void fail(const QString& msg);
    void emitOverall(int compPct, const QString& label);

    QNetworkAccessManager* m_nam{nullptr};
    QNetworkReply* m_reply{nullptr};
    QList<Component> m_queue;
    int m_idx{0};
    QString m_arch;
    QString m_staging;       // staging pack root being assembled
    QString m_tmpFile;       // current download temp
    bool m_busy{false};
    bool m_cancelled{false};
};

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
