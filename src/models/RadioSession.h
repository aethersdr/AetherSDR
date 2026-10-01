#pragma once

#include "models/RadioModel.h"

#include <QObject>
#include <QString>

#include <array>

class QThread;

namespace AetherSDR {

// RadioSession — the aggregate that constitutes "a connected radio" (#3351,
// #3445). Owns the RadioModel (by value), session identity, and the TciServer
// and CatPort[] lifetimes (MainWindow still constructs and wires them). Both
// hold a raw RadioModel*, so ~RadioSession's body deletes them before
// m_radioModel destructs (#2385). shutdownTciServer() stops TCI earlier, while
// the model and AudioEngine are alive, and joins its TciIo worker. Model->UI
// wiring stays in the GUI layer so models/ never depends on widgets.
// MainWindow::m_radioModel aliases radioModel(); new code should use the session.
class TciServer;
class CatPort;

class RadioSession : public QObject {
    Q_OBJECT

public:
    explicit RadioSession(QObject* parent = nullptr);
    ~RadioSession() override;

    RadioModel& radioModel() { return m_radioModel; }
    const RadioModel& radioModel() const { return m_radioModel; }

    // Session identity — stable across reconnects to the same radio.
    int sessionId() const { return m_sessionId; }
    void setSessionId(int id) { m_sessionId = id; }

    // Operator-facing label (radio nickname once connected, else model).
    QString label() const { return m_label; }
    void setLabel(const QString& label) { m_label = label; }

    // ── Owned servers (v2) ───────────────────────────────────────────────
    // The session takes ownership on set; servers must NOT have a QObject
    // parent (parent-based deletion would run after member destruction and
    // recreate #2385).

    static constexpr int kCatPorts = 8;

#ifdef HAVE_WEBSOCKETS
    TciServer* tciServer() const { return m_tciServer; }
    void setTciServer(TciServer* server);   // takes ownership
    // Early teardown for the shutdown path (#2385): delete while the
    // RadioModel is alive and audio is already stopped. Idempotent.
    void shutdownTciServer();
#endif

    CatPort* catPort(int i) const;
    void setCatPort(int i, CatPort* port);  // takes ownership
    // Raw array view for CatControlApplet::setPorts(CatPort**, int).
    CatPort** catPortsArray() { return m_catPorts.data(); }

private:
    RadioModel m_radioModel;
    // Owned; deleted in ~RadioSession's body (and shutdownTciServer()),
    // i.e. strictly before m_radioModel destructs — both hold RadioModel*.
#ifdef HAVE_WEBSOCKETS
    TciServer* m_tciServer{nullptr};
#endif
    std::array<CatPort*, kCatPorts> m_catPorts{};
    int m_sessionId{0};
    QString m_label;
};

} // namespace AetherSDR
