#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;

namespace AetherSDR {

class RadioModel;
class RigctlProtocol;

// Virtual serial port (PTY) implementing the Hamlib rigctld protocol.
// Creates a pseudo-terminal pair using openpty(). The slave device path
// (e.g. /dev/ttys004) can be pointed to by CAT-capable software. A
// convenience symlink is also created in a per-user directory so CAT
// software can auto-discover the PTY without knowing the per-session
// slave device path.
//
// Symlink location (per GHSA-qxhr-cwrc-pvrm — moved out of /tmp to
// eliminate cross-user conflicts and the TOCTOU window on non-sticky
// /tmp filesystems):
//   Linux:    $XDG_RUNTIME_DIR/aethersdr/cat-<letter>
//             (fallback: $HOME/.cache/aethersdr/cat-<letter>)
//   macOS:    $HOME/Library/Caches/AetherSDR/cat-<letter>
//   Windows:  N/A (PTY path is not supported there)
class RigctlPty : public QObject {
    Q_OBJECT

public:
    explicit RigctlPty(RadioModel* model, QObject* parent = nullptr);
    ~RigctlPty() override;

    bool start();
    void stop();

    bool isRunning() const { return m_masterFd >= 0; }
    QString slavePath() const { return m_slavePath; }
    QString symlinkPath() const { return m_symlinkPath; }

    // Which slice index this PTY's protocol will control.
    void setSliceIndex(int idx) { m_sliceIndex = idx; }
    int  sliceIndex() const     { return m_sliceIndex; }

    // Override the default symlink path. Use defaultSymlinkPath() to
    // compute the canonical per-user path for a given slice index.
    void setSymlinkPath(const QString& path) { m_symlinkPath = path; }

    // Compute the canonical per-user symlink path for a slice index
    // (0 → "A", 1 → "B", ...). The parent directory is NOT created
    // here — start() handles that. Used by UI surfaces that need to
    // display the path before the PTY actually starts (#2940 / GHSA-
    // qxhr-cwrc-pvrm).
    static QString defaultSymlinkPath(int sliceIndex);

signals:
    void started(const QString& path);
    void stopped();

private slots:
    void onDataReady();

private:
    RadioModel*      m_model;
    RigctlProtocol*  m_protocol{nullptr};
    int              m_sliceIndex{0};
    int              m_masterFd{-1};
    int              m_slaveFd{-1};
    QString          m_slavePath;
    QString          m_symlinkPath{defaultSymlinkPath(0)};
    QSocketNotifier* m_notifier{nullptr};
    QByteArray       m_buffer;
};

} // namespace AetherSDR
