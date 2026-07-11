#include "ShortcutManager.h"
#include "AppSettings.h"
#include <QHash>
#include <QRegularExpression>
#include <QWidget>
#include <QDebug>

namespace AetherSDR {

ShortcutManager::ShortcutManager(QObject* parent)
    : QObject(parent)
{
}

void ShortcutManager::registerAction(const QString& id, const QString& displayName,
                                     const QString& category, const QKeySequence& defaultKey,
                                     std::function<void()> handler,
                                     bool autoRepeat, bool keysTx)
{
    // The id becomes part of the XML element name "Shortcut_<id>". AppSettings
    // silently drops keys that fail ^[A-Za-z_][A-Za-z0-9_]*$ on save (qWarning
    // only), so an id containing '.'/'-'/etc. would be contains()-true in-session
    // yet never written — a cleared binding would resurrect after restart (the
    // exact #3964 failure mode). Keep ids to [A-Za-z0-9_].
    Q_ASSERT_X(QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+$")).match(id).hasMatch(),
               "ShortcutManager::registerAction",
               "action id must match [A-Za-z0-9_]+ (it forms the XML key Shortcut_<id>)");

    // Prevent duplicate registration
    for (const auto& a : m_actions) {
        if (a.id == id) {
            qWarning() << "ShortcutManager: duplicate action registration:" << id;
            return;
        }
    }
    m_actions.append({id, displayName, category, defaultKey, defaultKey,
                      std::move(handler), autoRepeat, /*persisted=*/false, keysTx});
}

void ShortcutManager::setBinding(const QString& actionId, const QKeySequence& key)
{
    auto* a = action(actionId);
    if (!a) return;

    // Clear any existing binding on this key (prevent conflicts)
    if (!key.isEmpty()) {
        for (auto& other : m_actions) {
            if (other.id != actionId && other.currentKey == key)
                other.currentKey = QKeySequence();
        }
    }

    a->currentKey = key;
    a->persisted = true;
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::clearBinding(const QString& actionId)
{
    auto* a = action(actionId);
    if (!a) return;
    a->currentKey = QKeySequence();
    a->persisted = true;   // user intent — an explicit "" must survive restart (#3964)
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::resetToDefaults()
{
    for (auto& a : m_actions) {
        a.currentKey = a.defaultKey;
        a.persisted = false;   // back to default → drop from settings, track future defaults
    }
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::loadBindings()
{
    auto& s = AppSettings::instance();
    for (auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        // A present key (even "") is explicit user intent — apply it and mark the
        // action persisted; an absent key keeps the registered default. Presence,
        // not the null-vs-"" value (which the XML round-trip flattens), is the
        // correct sentinel (#3964). We need the presence bit itself to seed
        // `persisted`, so this stays a contains() gate rather than a
        // value(key, default) fallback.
        if (s.contains(key)) {
            a.currentKey = QKeySequence(s.value(key).toString());
            a.persisted = true;
        }
    }

    // Normalize duplicate key bindings for this session only. This clears the
    // losing action's currentKey in memory but leaves its `persisted` flag
    // untouched, so saveBindings() will NOT write the machine-initiated clear:
    // the loser is recomputed from (defaults + persisted customizations) on every
    // load. Persisting it would be indistinguishable from a user clear and would
    // pin the loser to "" forever — suppressing a future release's default once
    // the colliding registration is fixed (#3964 review).
    QHash<QString, int> ownerByKey;
    for (int i = 0; i < m_actions.size(); ++i) {
        auto& action = m_actions[i];
        if (action.currentKey.isEmpty()) continue;

        const QString keyText = action.currentKey.toString();
        auto it = ownerByKey.find(keyText);
        if (it == ownerByKey.end()) {
            ownerByKey.insert(keyText, i);
            continue;
        }

        auto& incumbent = m_actions[*it];
        const bool incumbentCustomized = incumbent.currentKey != incumbent.defaultKey;
        const bool actionCustomized = action.currentKey != action.defaultKey;

        if (actionCustomized && !incumbentCustomized) {
            qWarning() << "ShortcutManager: clearing duplicate default binding"
                       << incumbent.id << "for key" << keyText
                       << "in favor of customized binding" << action.id;
            incumbent.currentKey = QKeySequence();
            *it = i;
        } else {
            qWarning() << "ShortcutManager: clearing duplicate binding"
                       << action.id << "for key" << keyText
                       << "owned by" << incumbent.id;
            action.currentKey = QKeySequence();
        }
    }
}

void ShortcutManager::saveBindings()
{
    auto& s = AppSettings::instance();
    for (const auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        // Persist only actions the user explicitly touched. A set or cleared
        // binding is marked persisted and written back — a cleared one round-trips
        // as "" and loadBindings()'s presence gate honors it (#3964). Everything
        // else (fresh at default, reset to default, or cleared only by the
        // load-time duplicate-key normalization) is written absent: a future
        // release's changed default reaches it instead of being pinned, and a
        // machine-initiated normalization clear never sticks.
        if (a.persisted) {
            s.setValue(key, a.currentKey.toString());
        } else {
            s.remove(key);
        }
    }
    s.save();
}

void ShortcutManager::rebuildShortcuts(QWidget* parent,
                                       std::function<bool()> guardFn)
{
    // Destroy existing shortcuts
    qDeleteAll(m_shortcuts);
    m_shortcuts.clear();

    for (const auto& a : m_actions) {
        if (a.currentKey.isEmpty() || !a.handler) continue;

        auto* sc = new QShortcut(a.currentKey, parent);
        sc->setAutoRepeat(a.autoRepeat);
        auto handler = a.handler;
        connect(sc, &QShortcut::activated, this, [guardFn, handler]() {
            if (guardFn && !guardFn()) return;
            handler();
        });
        m_shortcuts.append(sc);
    }
}

void ShortcutManager::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts)
        sc->setEnabled(enabled);
}

ShortcutManager::Action* ShortcutManager::action(const QString& id)
{
    for (auto& a : m_actions) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

const ShortcutManager::Action* ShortcutManager::actionForKey(const QKeySequence& key) const
{
    if (key.isEmpty()) return nullptr;
    for (const auto& a : m_actions) {
        if (a.currentKey == key) return &a;
    }
    return nullptr;
}

QString ShortcutManager::conflictCheck(const QKeySequence& key,
                                       const QString& excludeId) const
{
    if (key.isEmpty()) return {};
    for (const auto& a : m_actions) {
        if (a.id != excludeId && a.currentKey == key)
            return a.displayName;
    }
    return {};
}

QStringList ShortcutManager::categories()
{
    return {"Frequency", "Band", "Mode", "TX", "CW", "Audio", "Slice",
            "Filter", "Tuning", "DSP", "AGC", "EQ", "Display", "RIT/XIT"};
}

} // namespace AetherSDR
