#include "gui/ControlAvailabilityRegistry.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QAction>
#include <QPalette>

namespace AetherSDR {

namespace {

// The doctrine's two treatments, read from the theme rather than hard-coded so
// a user theme can restate them. Both tokens ship in default-dark and
// default-light; see docs/style/theme-style-guide.md §"Three-state controls"
// for the measured contrast and the honest note about the light theme.
QColor treatmentColor(ControlAvailability state, const QWidget* context)
{
    switch (state) {
        case ControlAvailability::Unavailable:
            return ThemeManager::instance().color(context,
                                                  QStringLiteral("color.control.unavailable"));
        case ControlAvailability::Inactive:
            return ThemeManager::instance().color(context,
                                                  QStringLiteral("color.control.inactive"));
        case ControlAvailability::Active:
            break;
    }
    return {};
}

// The accessible description is the half a screen reader actually conveys, and
// in the light theme it is close to the ONLY half that separates the two muted
// states — their colours are within 1.4:1 of each other there. So it is built
// from the state and the reason together, never from colour alone (#4896).
QString describe(ControlAvailability state, const QString& reason)
{
    switch (state) {
        case ControlAvailability::Unavailable:
            return reason;
        case ControlAvailability::Inactive:
            return QObject::tr("Available, not currently active");
        case ControlAvailability::Active:
            return QString();
    }
    return QString();
}

}  // namespace

ControlAvailabilityRegistry::ControlAvailabilityRegistry(RadioModel& model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    // ONE subscription for every registered control, which is the point: the
    // per-site lambdas this replaces had undefined ordering between them.
    connect(&m_model, &RadioModel::capabilitiesChanged, this,
            [this](bool, const RadioCapabilities&) { applyAll(); });
}

void ControlAvailabilityRegistry::registerWidget(QWidget* widget,
                                                 QString reason,
                                                 AvailabilityPredicate available,
                                                 EngagedPredicate engaged)
{
    if (!widget || !available)
        return;
    Entry entry;
    entry.widget = widget;
    entry.reason = std::move(reason);
    entry.available = std::move(available);
    entry.engaged = std::move(engaged);
    m_entries.push_back(entry);
    // APPLIED IMMEDIATELY. A widget built after the connect edge would otherwise
    // sit in its constructor's state until the next capabilitiesChanged, which
    // on a settled session may never come — the lazy-widget bug this replaces.
    applyOne(m_entries.last(), m_model.isConnected(), m_model.backendCapabilities());
}

void ControlAvailabilityRegistry::registerAction(QAction* action,
                                                 QString reason,
                                                 AvailabilityPredicate available,
                                                 EngagedPredicate engaged)
{
    if (!action || !available)
        return;
    Entry entry;
    entry.action = action;
    entry.reason = std::move(reason);
    entry.available = std::move(available);
    entry.engaged = std::move(engaged);
    m_entries.push_back(entry);
    applyOne(m_entries.last(), m_model.isConnected(), m_model.backendCapabilities());
}

void ControlAvailabilityRegistry::refreshEngaged()
{
    applyAll();
}

void ControlAvailabilityRegistry::applyAll()
{
    pruneDead();
    const bool connected = m_model.isConnected();
    const RadioCapabilities caps = m_model.backendCapabilities();
    for (Entry& entry : m_entries)
        applyOne(entry, connected, caps);
}

void ControlAvailabilityRegistry::applyOne(Entry& entry,
                                           bool connected,
                                           const RadioCapabilities& caps)
{
    // PERMISSIVE ON DISCONNECT, matching every gate in applyCapabilitiesToUi:
    // with no radio attached there is nothing to be honest about, and leaving a
    // control dimmed after unplugging reads as a fault rather than as an absent
    // capability.
    const bool available = !connected || entry.available(connected, caps);
    const bool engaged = available && entry.engaged && entry.engaged();
    entry.state = !available   ? ControlAvailability::Unavailable
                : engaged      ? ControlAvailability::Active
                               : ControlAvailability::Inactive;

    const QString description = describe(entry.state, entry.reason);
    // The reason rides on BOTH the tooltip and the accessible description. A
    // tooltip is a mouse affordance; a screen reader never sees it, which is
    // exactly the gap #5266 shipped and #5299 then deleted.
    const QString tip = entry.state == ControlAvailability::Unavailable ? entry.reason
                                                                        : QString();

    if (QWidget* w = entry.widget) {
        // ENABLED STATE, NOT VISIBILITY. The doctrine's whole point: the control
        // keeps its place in the layout so the operator can see the radio cannot
        // do this, rather than wondering where the control went.
        w->setEnabled(entry.state != ControlAvailability::Unavailable);
        w->setToolTip(tip);
        w->setAccessibleDescription(description);
        QPalette pal = w->palette();
        const QColor colour = treatmentColor(entry.state, w);
        if (colour.isValid())
            pal.setColor(QPalette::Disabled, QPalette::WindowText, colour);
        w->setPalette(pal);
    }
    if (QAction* a = entry.action) {
        a->setEnabled(entry.state != ControlAvailability::Unavailable);
        a->setToolTip(tip);
        // QAction has no accessibleDescription, so the reason rides on the
        // status tip — that is the channel Qt exposes to accessibility clients
        // for an action, and the one TX Band Settings was missing.
        //
        // ONLY WHEN UNAVAILABLE, unlike a widget's accessibleDescription. A
        // status tip is also shown in the STATUS BAR on hover, so describing
        // every merely-inactive entry there would push "Available, not
        // currently active" into the status bar for most of the menu. A widget
        // has a private accessible channel and can afford the fuller wording;
        // an action shares one with the UI and must not.
        a->setStatusTip(entry.state == ControlAvailability::Unavailable ? entry.reason
                                                                        : QString());
    }
}

void ControlAvailabilityRegistry::pruneDead()
{
    m_entries.removeIf([](const Entry& e) {
        return e.widget.isNull() && e.action.isNull();
    });
}

ControlAvailability ControlAvailabilityRegistry::stateOf(const QWidget* widget) const
{
    for (const Entry& e : m_entries)
        if (e.widget == widget)
            return e.state;
    return ControlAvailability::Unavailable;
}

ControlAvailability ControlAvailabilityRegistry::stateOf(const QAction* action) const
{
    for (const Entry& e : m_entries)
        if (e.action == action)
            return e.state;
    return ControlAvailability::Unavailable;
}

}  // namespace AetherSDR
