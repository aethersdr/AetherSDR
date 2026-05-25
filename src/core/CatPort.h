#pragma once

#include <QObject>
#include <QList>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QSocketNotifier;

namespace AetherSDR {

class RadioModel;
class RigctlProtocol;
class SmartCatProtocol;
class SmartCatSession;

enum class CatDialect {
    Rigctld,   // Hamlib rigctld protocol (newline-framed)
    TS2000,    // Kenwood TS-2000 CAT, no Flex ZZ extensions
    FlexCAT    // Kenwood TS-2000 + FlexRadio ZZ extensions
};

// Unified CAT port: one TCP server + one PTY (non-Windows), any dialect.
// Replaces the separate RigctlServer / SmartCatServer / RigctlPty hierarchy.
//
// Dialect and VFO assignments must be set before calling start().
// They cannot be changed while the port is running (per SmartSDR for Mac UX).
class CatPort : public QObject {
    Q_OBJECT

public:
    static constexpr int kVfoNone = -1;  // VFO B = simplex

    explicit CatPort(RadioModel* model, QObject* parent = nullptr);
    ~CatPort() override;

    bool start(quint16 port);
    void stop();

    bool    isRunning() const;
    quint16 port() const;
    int     clientCount() const;
    QString ptyPath() const;  // empty on Windows or if PTY not running

    // Config (effective only when stopped)
    void       setDialect(CatDialect d) { m_dialect = d; }
    CatDialect dialect() const          { return m_dialect; }
    void       setVfoA(int idx)         { m_vfoA = idx; }
    void       setVfoB(int idx)         { m_vfoB = idx; }
    int        vfoA() const             { return m_vfoA; }
    int        vfoB() const             { return m_vfoB; }

    // Per-user symlink path for the PTY slave device (GHSA-qxhr-cwrc-pvrm).
    // Mirrors RigctlPty::defaultSymlinkPath() — same platform/path logic.
    static QString defaultSymlinkPath(int portIndex);
    void    setSymlinkPath(const QString& path) { m_symlinkPath = path; }
    QString symlinkPath() const                 { return m_symlinkPath; }

signals:
    void clientCountChanged(int count);
    void ptyPathChanged(const QString& path);

private slots:
    void onNewConnection();
    // Rigctld TCP I/O
    void onRigctlData();
    void onRigctlDisconnected();
    // SmartCAT TCP
    void onCatSessionEnded(SmartCatSession* session);
    // PTY data
    void onPtyData();

private:
    void startPty();
    void stopPty();

    // Rigctld client state (inline, mirrors former RigctlServer)
    struct RigctlClient {
        QTcpSocket*     socket{nullptr};
        RigctlProtocol* protocol{nullptr};
        QByteArray      buffer;
    };

    RadioModel*             m_model;
    CatDialect              m_dialect{CatDialect::Rigctld};
    int                     m_vfoA{0};
    int                     m_vfoB{kVfoNone};
    QString                 m_symlinkPath;

    QTcpServer*             m_tcpServer{nullptr};
    QList<RigctlClient>     m_rigctlClients;
    QList<SmartCatSession*> m_catSessions;

    // PTY (non-Windows only)
#ifndef Q_OS_WIN
    int              m_ptyMasterFd{-1};
    int              m_ptySlaveFd{-1};
    QString          m_ptySlavePath;
    QSocketNotifier* m_ptyNotifier{nullptr};
    QByteArray       m_ptyBuffer;
    // Protocol handler for PTY (one of these is active, the other null)
    RigctlProtocol*  m_ptyRigctlProtocol{nullptr};
    SmartCatProtocol* m_ptyCatProtocol{nullptr};
#endif
};

} // namespace AetherSDR
