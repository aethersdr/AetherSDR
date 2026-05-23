#pragma once

#include <QObject>

class QSocketNotifier;

namespace MasterSDR {

class RadioModel;
class RigctlProtocol;

// Virtual serial port (PTY) implementing the Hamlib rigctld protocol.
// Creates a pseudo-terminal pair using openpty(). The slave device path
// (e.g. /dev/ttys004) can be pointed to by CAT-capable software.
// A symlink at /tmp/MasterSDR-CAT is also created for convenience.
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

    // Override the default symlink path (e.g. /tmp/MasterSDR-CAT-B).
    void setSymlinkPath(const QString& path) { m_symlinkPath = path; }

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
    QString          m_symlinkPath{"/tmp/MasterSDR-CAT"};
    QSocketNotifier* m_notifier{nullptr};
    QByteArray       m_buffer;
};

} // namespace MasterSDR
