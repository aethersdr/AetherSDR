#include "gui/ControlAvailabilityRegistry.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QAction>
#include <QStringList>
#include <QVariant>

#include <functional>

namespace AetherSDR {

namespace {

// The doctrine's two treatments, read from the theme rather than hard-coded so
// a user theme can restate them. Both tokens ship in default-dark and
// default-light; see docs/style/theme-style-guide.md §"Three-state controls"
// for the measured contrast and the honest note about the light theme.
QString treatmentToken(ControlAvailability state)
{
    switch (state) {
        case ControlAvailability::Unavailable:
            return QStringLiteral("color.control.unavailable");
        case ControlAvailability::Inactive:
            return QStringLiteral("color.control.inactive");
        case ControlAvailability::Active:
            return {};
    }
    return {};
}

// The registry holds a text channel only while it shows text of its own there,
// as {own, shown} on the control, so a re-created registry finds it too. Text
// the owner wrote since the last apply is its own text (read via `ownOf` if
// given) and comes back on release. An empty `wanted` releases, so an empty
// reason cannot blank the own text.
template <typename Set>
void applyText(QObject* control, const char* key, const QString& current,
               const std::function<QString(const QString& own)>& wanted, Set set,
               const std::function<QString()>& ownOf = {})
{
    const QStringList hold = control->property(key).toStringList();
    const bool held = hold.size() == 2;
    const bool fromHold = held && current == hold.at(1);
    QString own = fromHold ? hold.at(0) : current;
    const QString want = wanted(own);
    if (!fromHold && ownOf && !want.isEmpty()) {
        own = ownOf();
    }
    if (want.isEmpty()) {
        if (held) {
            set(own);
            control->setProperty(key, QVariant());
        }
        return;
    }
    if (current != want) {
        set(want);
    }
    const QStringList next{own, want};
    if (hold != next) {
        control->setProperty(key, next);
    }
}

// The inactive announcement is appended to the control's own description, so
// the state is announced without losing what the control is.
QString withInactivePhrase(const QString& own)
{
    const QString phrase = QObject::tr("Available, not currently active");
    const QString base = own.trimmed();
    if (base.isEmpty()) {
        return phrase;
    }
    const QChar last = base.back();
    const bool closed = last == u'.' || last == u'!' || last == u'?';
    return base + (closed ? QStringLiteral(" ") : QStringLiteral(". ")) + phrase;
}

// The tooltip an action set itself; null when it reports its text instead, so
// that releasing the hold makes it follow its text again.
QString explicitToolTip(QAction* action)
{
    const QString shown = action->toolTip();
    action->setToolTip(QString());
    if (action->toolTip() == shown) {
        return QString();
    }
    action->setToolTip(shown);
    return shown;
}

constexpr char kToolTipHold[] = "aetherAvailabilityToolTip";
constexpr char kDescriptionHold[] = "aetherAvailabilityDescription";

}  // namespace

ControlAvailabilityRegistry::ControlAvailabilityRegistry(RadioModel& model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    // ONE subscription for every registered control, which is the point: the
    // per-site lambdas this replaces had undefined ordering between them.
    connect(&m_model, &RadioModel::capabilitiesChanged, this,
            [this](bool connected, const RadioCapabilities& caps) {
                applyAll(connected, caps);
            });
}

void ControlAvailabilityRegistry::registerWidget(QWidget* widget,
                                                 QString reason,
                                                 AvailabilityPredicate available,
                                                 EngagedPredicate engaged,
                                                 bool availableWhenDisconnected)
{
    if (!widget || !available) {
        return;
    }
    Entry entry;
    entry.widget = widget;
    entry.availableWhenDisconnected = availableWhenDisconnected;
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
    if (!action || !available) {
        return;
    }
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
    applyAll(m_model.isConnected(), m_model.backendCapabilities());
}

void ControlAvailabilityRegistry::applyAll(bool connected, const RadioCapabilities& caps)
{
    pruneDead();
    for (Entry& entry : m_entries) {
        applyOne(entry, connected, caps);
    }
}

void ControlAvailabilityRegistry::applyOne(Entry& entry,
                                           bool connected,
                                           const RadioCapabilities& caps)
{
    // Preserve the historical disconnected treatment unless a control explicitly
    // requires a live receiver (for example observed broadcast-FM settings).
    const bool available = connected ? entry.available(connected, caps)
                                     : entry.availableWhenDisconnected;
    const bool engaged = connected && available && entry.engaged && entry.engaged();
    entry.state = !available   ? ControlAvailability::Unavailable
                : engaged      ? ControlAvailability::Active
                               : ControlAvailability::Inactive;

    const bool unavailable = entry.state == ControlAvailability::Unavailable;
    const auto reasonOnly = [&](const QString&) {
        return unavailable ? entry.reason : QString();
    };

    if (QWidget* w = entry.widget) {
        // ENABLED STATE, NOT VISIBILITY. The doctrine's whole point: the control
        // keeps its place in the layout so the operator can see the radio cannot
        // do this, rather than wondering where the control went.
        w->setEnabled(!unavailable);
        // The reason rides on BOTH channels: a tooltip is help text, not the
        // description a screen reader announces (#4896). Inactive is appended to
        // the widget's own description.
        applyText(w, kToolTipHold, w->toolTip(), reasonOnly,
                  [w](const QString& text) { w->setToolTip(text); });
        applyText(w, kDescriptionHold, w->accessibleDescription(),
            [&](const QString& own) {
                if (unavailable) {
                    return entry.reason;
                }
                return entry.state == ControlAvailability::Inactive
                    ? withInactivePhrase(own) : QString();
            },
            [w](const QString& text) { w->setAccessibleDescription(text); });
        ThemeManager::instance().setWidgetForegroundToken(w, treatmentToken(entry.state));
    }
    if (QAction* a = entry.action) {
        a->setEnabled(!unavailable);
        // An action with no tooltip of its own is held as null and released to
        // follow its text, also when the text changed meanwhile.
        applyText(a, kToolTipHold, a->toolTip(), reasonOnly,
                  [a](const QString& text) { a->setToolTip(text); },
                  [a] { return explicitToolTip(a); });
        // QAction has no accessibleDescription; Qt exposes the status tip to
        // accessibility clients. Only the reason goes there: it also shows in
        // the status bar on hover, and describing every entry would flood it.
        applyText(a, kDescriptionHold, a->statusTip(), reasonOnly,
                  [a](const QString& text) { a->setStatusTip(text); });
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
    for (const Entry& e : m_entries) {
        if (e.widget == widget) {
            return e.state;
        }
    }
    return ControlAvailability::Unavailable;
}

ControlAvailability ControlAvailabilityRegistry::stateOf(const QAction* action) const
{
    for (const Entry& e : m_entries) {
        if (e.action == action) {
            return e.state;
        }
    }
    return ControlAvailability::Unavailable;
}

}  // namespace AetherSDR
