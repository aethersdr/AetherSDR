#include "ShortcutTooltips.h"

#include "ControlActionRegistry.h"
#include "core/ShortcutManager.h"

#include <QApplication>
#include <QEvent>
#include <QKeySequence>
#include <QTimer>
#include <QVariant>

#include <utility>

namespace AetherSDR {

namespace {

constexpr char kBaseToolTip[] = "aetherShortcutBaseToolTip";
constexpr char kAppliedToolTip[] = "aetherShortcutAppliedToolTip";
constexpr char kBaseDescription[] = "aetherShortcutBaseDescription";
constexpr char kAppliedDescription[] = "aetherShortcutAppliedDescription";

// The owner's own text: what the widget says now, unless that is still our
// last write, in which case the base stashed with it.
QString ownText(QWidget* w, const QString& current, const char* baseProp,
                const char* appliedProp)
{
    const QVariant applied = w->property(appliedProp);
    if (applied.isValid() && applied.toString() == current) {
        return w->property(baseProp).toString();
    }
    w->setProperty(baseProp, current);
    return current;
}

} // namespace

ShortcutTooltips::ShortcutTooltips(const ControlActionRegistry& registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
{
}

QString ShortcutTooltips::compose(const QString& base, const QString& keyText,
                                  bool shortcutsEnabled, bool available)
{
    if (keyText.isEmpty()) {
        return base;
    }
    // U+2014: the qualifier is its own clause, not part of the key name.
    const QString clause = shortcutsEnabled
        ? QStringLiteral("(%1)").arg(keyText)
        : QStringLiteral("(%1 — %2)").arg(keyText, tr("shortcuts disabled"));
    if (base.isEmpty()) {
        return clause;
    }
    return available ? QStringLiteral("%1 %2").arg(base, clause)
                     : QStringLiteral("%1 %2").arg(clause, base);
}

void ShortcutTooltips::setShortcutsEnabled(bool enabled)
{
    if (m_shortcutsEnabled == enabled) {
        return;
    }
    m_shortcutsEnabled = enabled;
    annotateWatched();
}

void ShortcutTooltips::refresh()
{
    collect();
    annotateWatched();
}

void ShortcutTooltips::annotateWatched()
{
    for (const QPointer<QWidget>& w : std::as_const(m_watched)) {
        if (w) {
            annotateOne(w);
        }
    }
}

void ShortcutTooltips::annotate(QWidget* root)
{
    if (!root) {
        return;
    }
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (QWidget* w : std::as_const(widgets)) {
        if (w->property(kShortcutActionProperty).isValid()) {
            watch(w);
            annotateOne(w);
        }
    }
}

void ShortcutTooltips::watch(QWidget* w)
{
    if (!m_watched.contains(w)) {
        m_watched.append(w);
        w->installEventFilter(this);
    }
}

void ShortcutTooltips::collect()
{
    // Every widget, not one window's children: an applet in a hidden or
    // floating container need not sit under the main window.
    m_watched.removeIf([](const QPointer<QWidget>& w) { return w.isNull(); });
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget* w : widgets) {
        if (w->property(kShortcutActionProperty).isValid()) {
            watch(w);
        }
    }
}

void ShortcutTooltips::annotateOne(QWidget* w)
{
    const QString id = w->property(kShortcutActionProperty).toString();
    // Not QKeySequence::toString: Qt cannot name a sided key (#7357).
    const QString keyText =
        shortcutKeyText(m_registry.boundKey(id), QKeySequence::NativeText);

    const QString tip = w->toolTip();
    const QString desc = w->accessibleDescription();
    const QString baseTip = ownText(w, tip, kBaseToolTip, kAppliedToolTip);
    const QString baseDesc = ownText(w, desc, kBaseDescription, kAppliedDescription);

    // Disabled by its owner = unavailable; its texts are then the reason.
    const bool available = !w->testAttribute(Qt::WA_ForceDisabled);
    // A control with no tooltip of its own names the key after its description.
    const QString tipBase = (keyText.isEmpty() || !baseTip.isEmpty()) ? baseTip : baseDesc;
    const QString wantedTip = compose(tipBase, keyText, m_shortcutsEnabled, available);
    const QString wantedDesc = compose(baseDesc, keyText, m_shortcutsEnabled, available);

    w->setProperty(kAppliedToolTip, wantedTip);
    w->setProperty(kAppliedDescription, wantedDesc);
    m_writing = true;
    if (wantedTip != tip) {
        w->setToolTip(wantedTip);
    }
    if (wantedDesc != desc) {
        w->setAccessibleDescription(wantedDesc);
    }
    m_writing = false;
}

void ShortcutTooltips::schedule(QWidget* w)
{
    if (!m_pending.contains(w)) {
        m_pending.append(w);
    }
    if (!m_flushQueued) {
        m_flushQueued = true;
        QTimer::singleShot(0, this, &ShortcutTooltips::flush);
    }
}

void ShortcutTooltips::flush()
{
    m_flushQueued = false;
    const QVector<QPointer<QWidget>> pending = std::exchange(m_pending, {});
    for (const QPointer<QWidget>& w : pending) {
        if (w) {
            annotateOne(w);
        }
    }
}

bool ShortcutTooltips::eventFilter(QObject* watched, QEvent* event)
{
    auto* w = qobject_cast<QWidget*>(watched);
    if (!w || m_writing) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::ToolTipChange:
    case QEvent::EnabledChange:
        // Deferred: owners write the tooltip, the description and the enabled
        // state as one block, and only the tooltip write is observable.
        schedule(w);
        break;
    case QEvent::ToolTip:
    case QEvent::FocusIn:
        // Right before the text is shown or announced, so it is never stale.
        annotateOne(w);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace AetherSDR
