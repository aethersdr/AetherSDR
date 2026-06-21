#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QByteArray>
#include <QString>

class QLocalServer;
class QLocalSocket;
class QWidget;
class QJsonObject;

namespace AetherSDR {

// In-app, agent-first automation bridge (issue #3646, Phase 0).
//
// Exposes a tiny line/JSON command channel over a QLocalServer so an external
// agent can introspect and capture the GUI without driving OS accessibility
// APIs or pixel-hunting through VNC. It is *off* in production and only starts
// when the AETHER_AUTOMATION environment variable is set, so it adds no attack
// surface or overhead to normal runs.
//
// Phase 0 implements two read-only verbs:
//
//   dumpTree                       -> ARIA-style JSON snapshot of every
//                                     top-level QWidget hierarchy (objectName,
//                                     class, accessibleName, role/value,
//                                     enabled, visible, global geometry).
//   grab <target> [path]           -> PNG capture of a single widget, resolved
//                                     by objectName, class name, or
//                                     accessibleName. Reads back the GPU
//                                     framebuffer for the QRhi panadapter so
//                                     the live spectrum is captured correctly.
//
// Requests are newline-delimited. Each line is either a bare command
// ("ping", "dumpTree", "grab SpectrumWidget /tmp/pan.png") or a JSON object
// ({"cmd":"grab","target":"SpectrumWidget","path":"/tmp/pan.png"}). Each
// request yields exactly one compact-JSON response line.
//
// Later phases (invoke/get/replay) layer onto this same channel; keeping it
// separate from TciServer is deliberate — TCI has external protocol-compat
// constraints (eesdr-tci aborts on unknown commands) and test verbs must never
// leak into a radio-control protocol.
class AutomationServer : public QObject {
    Q_OBJECT

public:
    explicit AutomationServer(QObject* parent = nullptr);
    ~AutomationServer() override;

    // Start listening on the given QLocalServer name. Returns false if the
    // server could not bind (e.g. a stale socket that could not be removed).
    // On success the resolved socket path is written to a discovery file
    // (<temp>/aethersdr-automation.json) so a driver can find it without
    // guessing the platform-specific endpoint.
    bool start(const QString& serverName);
    void stop();

    bool isRunning() const;
    QString serverName() const { return m_serverName; }
    QString fullServerName() const;  // resolved socket path / pipe name

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    // Dispatch a single request line and return the response object.
    QJsonObject handleLine(const QByteArray& line);

    QJsonObject doDumpTree() const;
    QJsonObject doGrab(const QString& target, const QString& path) const;

    // Resolve a target string to a widget: exact objectName first, then
    // class name (with or without namespace) or accessibleName.
    static QWidget* resolveWidget(const QString& target);

    QLocalServer* m_server{nullptr};
    QString       m_serverName;
    QString       m_discoveryFile;
    QHash<QLocalSocket*, QByteArray> m_buffers;  // per-client read buffer
};

} // namespace AetherSDR
