#pragma once

#include "models/RadioModel.h"

#include <QObject>
#include <QString>

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
// Planned v2+ scope (see #3445):
//   • TciServer + CatPort[] ownership (today constructed and owned by
//     MainWindow in wireRadioModel()/wireCatPorts())
//   • the wireDiscovery/wireRadioModel/wirePanLifecycle bodies from
//     MainWindow_Session.cpp move here as session methods
//   • per-session settings facade (Principle V nested JSON)
//
// MainWindow currently binds `RadioModel& m_radioModel` to
// session->radioModel() so the ~900 existing call sites across the
// MainWindow TUs compile unchanged. New code SHOULD prefer going
// through the session.
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

private:
    RadioModel m_radioModel;
    int m_sessionId{0};
    QString m_label;
};

} // namespace AetherSDR
