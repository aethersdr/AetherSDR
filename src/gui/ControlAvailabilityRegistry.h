#pragma once

// Three-state controls (#5262 M3a). Controls are never shown or hidden per
// radio; each is unavailable (radio lacks it: dimmed, with a reason), inactive
// (supported, not engaged: greyed) or active. Hiding happens only per applet.
// See docs/style/theme-style-guide.md §"Three-state controls".
// Registration applies state immediately, so widgets built after
// capabilitiesChanged are correct from birth, and each control declares its
// predicate once. UI state is never the safety mechanism: TX refusal and
// rollback stay in the model.

#include "core/backends/RadioCapabilities.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QAction;

namespace AetherSDR {

class RadioModel;

// What a control is currently saying about itself.
enum class ControlAvailability {
    Unavailable,   // the radio cannot do this — dimmed, reason required
    Inactive,      // the radio can, but it is not engaged — greyed
    Active,        // engaged — normal
};

class ControlAvailabilityRegistry : public QObject {
    Q_OBJECT
public:
    // Answers "can THIS radio do it?" from the capability payload. Takes the
    // payload rather than re-polling: this runs as an event consumer, and the
    // one consumption rule is that event consumers use what the signal carried
    // (#5262 M1). Action-time guards re-poll; this is not one.
    using AvailabilityPredicate =
        std::function<bool(bool connected, const RadioCapabilities& caps)>;
    // Optional: "is it engaged right now?". Absent means the control has no
    // engaged state and renders Inactive whenever it is available.
    using EngagedPredicate = std::function<bool()>;

    explicit ControlAvailabilityRegistry(RadioModel& model, QObject* parent = nullptr);

    // Register a widget. `reason` is shown when unavailable and becomes both the
    // tooltip and the accessibleDescription — REQUIRED, because a dimmed control
    // with no reason is the accessibility defect this milestone exists to close
    // (#4896). Applied immediately, so a widget built after connect is correct
    // without a second push.
    void registerWidget(QWidget* widget,
                        QString reason,
                        AvailabilityPredicate available,
                        EngagedPredicate engaged = {});

    // Same contract for a menu entry or toolbar action.
    void registerAction(QAction* action,
                        QString reason,
                        AvailabilityPredicate available,
                        EngagedPredicate engaged = {});

    // Re-evaluate the engaged half for everything. Availability follows
    // capabilitiesChanged on its own; engagement is driven by whatever the
    // control reflects, so its owner says when that moved.
    void refreshEngaged();

    [[nodiscard]] int registrationCount() const { return m_entries.size(); }
    // The state a registered widget currently renders. For tests and the
    // automation bridge; returns Unavailable for anything unregistered.
    [[nodiscard]] ControlAvailability stateOf(const QWidget* widget) const;
    [[nodiscard]] ControlAvailability stateOf(const QAction* action) const;

private:
    struct Entry {
        QPointer<QWidget> widget;
        QPointer<QAction> action;
        QString reason;
        AvailabilityPredicate available;
        EngagedPredicate engaged;
        ControlAvailability state{ControlAvailability::Unavailable};
    };

    void applyAll(bool connected, const RadioCapabilities& caps);
    void applyOne(Entry& entry, bool connected, const RadioCapabilities& caps);
    void pruneDead();

    RadioModel& m_model;
    QVector<Entry> m_entries;
};

}  // namespace AetherSDR
