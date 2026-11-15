#pragma once

#include <QObject>
#include <QList>
#include <QString>

#include <memory>

class QTcpServer;
class QTcpSocket;
class QSocketNotifier;

namespace AetherSDR {

class RadioModel;
class RigctlProtocol;
class SmartCatProtocol;
class SmartCatSession;
class OtrspProtocol;

enum class CatDialect {
    Rigctld,   // Hamlib rigctld protocol (newline-framed)
    TS2000,    // Kenwood TS-2000 CAT, no Flex ZZ extensions
    FlexCAT,   // Kenwood TS-2000 + FlexRadio ZZ extensions
    OTRSP      // Two-slice RX/TX focus (carriage-return-framed)
};

// Whether a dialect has a configurable VFO B. rigctld (Hamlib) is single-VFO per
// port — RigctlProtocol has no VFO B input; split is create-on-demand. The
// TS-2000, FlexCAT and OTRSP support a configured VFO B (dual-VFO).
// Single definition so the config UI — and any future config surface — agrees.
inline bool dialectSupportsVfoB(CatDialect d) { return d != CatDialect::Rigctld; }

inline CatDialect dialectFromKey(const QString& key)
{
    if (key == "TS2000") { return CatDialect::TS2000; }
    if (key == "FlexCAT") { return CatDialect::FlexCAT; }
    if (key == "OTRSP") { return CatDialect::OTRSP; }
    return CatDialect::Rigctld;
}

inline QString dialectKey(CatDialect dialect)
{
    switch (dialect) {
    case CatDialect::TS2000: return QStringLiteral("TS2000");
    case CatDialect::FlexCAT: return QStringLiteral("FlexCAT");
    case CatDialect::OTRSP: return QStringLiteral("OTRSP");
    case CatDialect::Rigctld: return QStringLiteral("Rigctld");
    }
    return QStringLiteral("Rigctld");
}

inline bool validOtrspMapping(int first, int second, int receiverCount)
{
    return first >= 0 && second >= 0 && first != second
        && first < receiverCount && second < receiverCount;
}

// Unified CAT port: one TCP server + one PTY (non-Windows), any dialect.
// Replaces the separate RigctlServer / SmartCatServer / RigctlPty hierarchy.
//
// Dialect and VFO assignments must be set before calling start().
// They cannot be changed while the port is running (per SmartSDR for Mac UX).
class CatPort : public QObject {
    Q_OBJECT

public:
    static constexpr int kVfoNone = -1;  // VFO B = simplex
    static constexpr quint16 kOtrspDefaultPort = 60000;

    explicit CatPort(RadioModel* model, QObject* parent = nullptr);
    ~CatPort() override;

    bool start(quint16 port);
    void stop();

    bool    isRunning() const;
    quint16 port() const;
    int     clientCount() const;
    QString ptyPath() const;  // empty on Windows or if PTY not running
    QString errorString() const { return m_error; }

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
    void stateChanged();

private slots:
    void onNewConnection();
    // Rigctld TCP I/O
    void onRigctlData();
    void onRigctlDisconnected();
    void onOtrspData();
    void onOtrspDisconnected();
    // SmartCAT TCP
    void onCatSessionEnded(SmartCatSession* session);
    // PTY data
    void onPtyData();
    void flushOtrspPtyOutput();

private:
    void startPty();
    void stopPty();
    static void writeOtrspResponse(QTcpSocket* socket, const QByteArray& response);
    void writeOtrspPtyResponse(const QByteArray& response);

    // Rigctld client state (inline, mirrors former RigctlServer)
    struct RigctlClient {
        QTcpSocket*     socket{nullptr};
        RigctlProtocol* protocol{nullptr};
        QByteArray      buffer;
    };
    struct OtrspClient {
        QTcpSocket* socket{nullptr};
        std::shared_ptr<OtrspProtocol> protocol;
    };

    RadioModel*             m_model;
    CatDialect              m_dialect{CatDialect::Rigctld};
    int                     m_vfoA{0};
    int                     m_vfoB{kVfoNone};
    QString                 m_symlinkPath;
    QString                 m_error;

    QTcpServer*             m_tcpServer{nullptr};
    QList<RigctlClient>     m_rigctlClients;
    QList<OtrspClient>      m_otrspClients;
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
    std::shared_ptr<OtrspProtocol> m_ptyOtrspProtocol;
    QByteArray m_ptyOtrspOutput;
    QSocketNotifier* m_ptyWriteNotifier{nullptr};
#endif
};

} // namespace AetherSDR
