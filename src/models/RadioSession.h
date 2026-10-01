#pragma once

#include "models/RadioModel.h"

#include <QObject>
#include <QString>

#include <array>

class QThread;

namespace AetherSDR {

// RadioSession — the aggregate that constitutes "a connected radio" (#3351,
// #3445).
//
// It owns:
//   • the RadioModel (by value);
//   • session identity (id, label) for a future session switcher;
//   • the TciServer and CatPort[] LIFETIMES. Construction and wiring stay in
//     MainWindow's wireRadioModel()/wireCatPorts() (UI-coupled). Both servers
//     hold a raw RadioModel*, so they are deleted in ~RadioSession's body,
//     which runs before m_radioModel destructs (the #2385 crash-on-quit
//     class). MainWindow's shutdown still stops TCI early, via
//     shutdownTciServer(), while the model and AudioEngine are alive; that
//     destroys the controller and joins its TciIo worker.
//
// Deliberately not here (#3445):
//   • the wireDiscovery/wireRadioModel/wirePanLifecycle bodies — model->UI glue
//     referencing MainWindow's widgets; moving them into models/ would invert
//     the dependency. They stay in MainWindow_Session.cpp or a future GUI-layer
//     per-session controller.
//   • a per-session settings facade — per-radio state today is global,
//     radio-side, or already namespaced (BandStackSettings keys
//     "Radio_<serial>", the template to follow). Build it when a second
//     session has a real consumer.
//
// MainWindow binds `RadioModel& m_radioModel` to session->radioModel() so
// existing call sites compile unchanged. New code should go through the session.
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
