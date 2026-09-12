#pragma once

// The three-state control doctrine, as a mechanism (#5262 M3a).
//
// THE RULE, maintainer-ruled and binding: individual controls are never shown
// or hidden per radio. Every control renders in one of three states —
//
//   unavailable  the radio lacks the capability            dimmed, with a reason
//   inactive     supported by this radio, not engaged now  greyed
//   active       engaged                                   normal
//
// — and hiding exists only at APPLET granularity, where a radio-specific
// cluster moves to its own applet that hides wholesale. See
// docs/style/theme-style-guide.md §"Three-state controls".
//
// WHY A REGISTRY RATHER THAN MORE setVisible() CALLS. The per-site plumbing it
// replaces failed in two ways that are properties of the plumbing, not of any
// one site:
//
//   * A WIDGET BUILT AFTER THE SIGNAL never learns its state. MainWindow's
//     applyCapabilitiesToUi() runs on capabilitiesChanged; a pane added later
//     (Add Panadapter, a layout change, a lazily-built dialog page) missed it
//     and rendered in whatever state its constructor left it. The Calibration
//     page and DemoApplet both carried hand-written second pushes to paper over
//     this. Registration here APPLIES IMMEDIATELY, so a control is correct from
//     the moment it exists and the second push is unnecessary.
//   * ONE SUBSCRIPTION, NOT N. Each site connecting its own lambda to
//     capabilitiesChanged makes ordering between them undefined and makes "what
//     does this control depend on?" unanswerable without reading every lambda.
//     A control declares its predicate once, here.
//
// WHAT IT IS NOT. It does not decide whether a control is SAFE to use — that
// stays with the model-level TX refusal and rollback (Principle VI). UI state is
// never the safety mechanism; a dimmed control that somehow receives a click
// must still be refused below the seam.

#include "core/backends/RadioCapabilities.h"

#include <QHash>
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
    // engaged state and renders Active whenever it is available.
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

    void applyAll();
    void applyOne(Entry& entry, bool connected, const RadioCapabilities& caps);
    void pruneDead();

    RadioModel& m_model;
    QVector<Entry> m_entries;
};

}  // namespace AetherSDR
