#pragma once

#include "models/RadioModel.h"

#include <QObject>
#include <QString>

#include <array>

namespace AetherSDR {

// RadioSession — the aggregate that constitutes "a connected radio".
//
// Born from the #3351 monolith decomposition and the #3445 multi-radio
// brief (Camp B): MainWindow historically embedded one RadioModel by
// value plus the per-radio servers and lifecycle as loose members,
// which hard-coded the single-radio assumption into the application
// layer. RadioSession gives that bundle a name and an owner.
//
// v1 scope (this class today):
//   • owns the RadioModel (by value — identical construction semantics
//     to the old MainWindow member)
//   • carries session identity (id, label) for the future session
//     switcher UI
//
// v2 scope (this PR): TciServer + CatPort[] OWNERSHIP. Construction and
// signal wiring stay in MainWindow's wireRadioModel()/wireCatPorts()
// (they are UI-coupled); the session owns lifetime. Why this matters:
// both server types hold a raw RadioModel* — if they outlive the model,
// that's the #2385 crash-on-quit class. With the session owning both,
// teardown order is structural: servers are deleted in ~RadioSession's
// BODY, which the language guarantees runs before the m_radioModel
// member destructs. The old manual delete-before-members dance in
// MainWindow's shutdown path remains only for its *early* teardown
// requirement (DAX stream-remove commands must reach the radio while
// audio is already stopped) and now routes through shutdownTciServer().
//
// Planned v3+ scope (see #3445):
//   • the wireDiscovery/wireRadioModel/wirePanLifecycle bodies from
//     MainWindow_Session.cpp move here as session methods
//   • per-session settings facade (Principle V nested JSON)
//
// MainWindow currently binds `RadioModel& m_radioModel` to
// session->radioModel() so the ~900 existing call sites across the
// MainWindow TUs compile unchanged. New code SHOULD prefer going
// through the session.
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
