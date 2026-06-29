#pragma once

#ifdef HAVE_NVIDIA_AFX

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace AetherSDR {

// Download-on-demand cache for the NVIDIA AFX denoiser "pack" (the AFX runtime
// libs + CUDA/TensorRT + the per-GPU denoiser model). Lives under the app data
// dir so the in-process NvidiaAfxFilter can dlopen it with no env var.
//
// install() accepts an http(s) URL (downloaded) or a local archive path / file://
// URL (imported — for offline/air-gapped use). The archive is a .tar.zst laid
// out as the AFX SDK root (nvafx/, external/cuda/, features/denoiser/). After
// extraction the denoiser feature lib is symlinked into nvafx/lib so the core's
// $ORIGIN RPATH can find it. Install is atomic: staged, then renamed into place.
//
// All network I/O is async (QNetworkAccessManager); extraction runs via tar in
// a QProcess. Progress + completion are reported by signals on the GUI thread.
class NvidiaAfxPack : public QObject {
    Q_OBJECT
public:
    explicit NvidiaAfxPack(QObject* parent = nullptr);
    ~NvidiaAfxPack() override;

    // GPU arch tag for this machine, e.g. "sm_89". Empty if no NVIDIA GPU found.
    static QString detectArch();
    // Cache root: <AppLocalData>/nvidia-afx
    static QString cacheRoot();
    // The active pack dir (cacheRoot/current) when a usable pack is present.
    static QString installedPackDir();
    static bool isInstalled();

    // One-line status for the panel, e.g. "Installed (sm_89)" / "Not installed".
    QString statusText() const;

    bool busy() const { return m_busy; }

    // Download (http/https) or import (local path / file://) + assemble the pack.
    void install(const QString& sourceUrlOrPath);
    void cancel();
    // Remove the installed pack (frees disk; next enable needs a re-download).
    static bool removeInstalled();

signals:
    void progress(int percent, const QString& status);  // percent <0 = indeterminate
    void finished(bool ok, const QString& message);

private:
    void importArchive(const QString& archivePath);  // extract + assemble + commit
    void fail(const QString& msg);

    QNetworkAccessManager* m_nam{nullptr};
    QNetworkReply* m_reply{nullptr};
    QString m_tmpArchive;   // downloaded archive awaiting extraction
    bool m_busy{false};
    bool m_cancelled{false};
};

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
