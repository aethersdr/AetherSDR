#include "ControlActionRegistry.h"

#include <QRegularExpression>
#include <algorithm>

namespace AetherSDR {

bool ControlActionRegistry::isValidShortcutId(const QString& id)
{
    // ShortcutManager stores bindings as Shortcut_<id>; AppSettings drops a key
    // outside this alphabet on save, so a bad id would bind but never persist.
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_]+$"));
    return re.match(id).hasMatch();
}

bool ControlActionRegistry::knownShortcutId(const QString& id) const
{
    if (id.isEmpty())
        return false;
    for (const auto& a : m_actions) {
        if (a.shortcutId.isEmpty())
            continue;
        if (a.kind == ControlAction::Kind::Level) {
            if (upId(a.shortcutId) == id || downId(a.shortcutId) == id)
                return true;
        } else if (a.shortcutId == id) {
            return true;
        }
    }
    return std::any_of(m_pairs.cbegin(), m_pairs.cend(),
                       [&](const Pair& p) { return p.shortcutId == id; });
}

bool ControlActionRegistry::knownMidiId(const QString& id) const
{
    if (id.isEmpty())
        return false;
    return std::any_of(m_actions.cbegin(), m_actions.cend(),
                       [&](const ControlAction& a) { return a.midiId == id; })
        || std::any_of(m_pairs.cbegin(), m_pairs.cend(),
                       [&](const Pair& p) { return p.midiId == id; })
        || std::any_of(m_keyingMidi.cbegin(), m_keyingMidi.cend(),
                       [&](const KeyingMidi& k) { return k.midiId == id; });
}

bool ControlActionRegistry::add(ControlAction a)
{
    const QString label = a.name.isEmpty() ? a.shortcutId + a.midiId : a.name;
    auto fail = [&](const QString& why) {
        m_errors << QStringLiteral("%1: %2").arg(label, why);
        return false;
    };
    if (a.shortcutId.isEmpty() && a.midiId.isEmpty())
        return fail(QStringLiteral("no surface id"));
    if (!a.set)
        return fail(QStringLiteral("no setter"));
    if (a.kind != ControlAction::Kind::Trigger && !a.get)
        return fail(QStringLiteral("toggle/level needs a getter"));
    if (!a.shortcutId.isEmpty() && !isValidShortcutId(a.shortcutId))
        return fail(QStringLiteral("shortcut id must match [A-Za-z0-9_]+"));
    if (a.kind == ControlAction::Kind::Level && a.keyStep <= 0.0f)
        return fail(QStringLiteral("level needs a positive keyStep"));
    if (a.kind == ControlAction::Kind::Level && !a.shortcutId.isEmpty()
        && (knownShortcutId(upId(a.shortcutId)) || knownShortcutId(downId(a.shortcutId))))
        return fail(QStringLiteral("duplicate shortcut id"));
    if (a.kind != ControlAction::Kind::Level && knownShortcutId(a.shortcutId))
        return fail(QStringLiteral("duplicate shortcut id"));
    if (knownMidiId(a.midiId))
        return fail(QStringLiteral("duplicate MIDI id"));
    if (a.midiCategory.isEmpty())
        a.midiCategory = a.category;
    m_actions.append(std::move(a));
    return true;
}

void ControlActionRegistry::addPair(const QString& shortcutId, const QString& midiId)
{
    m_pairs.append({shortcutId, midiId});
}

void ControlActionRegistry::addKeyingMidi(const QString& midiId, const QString& name,
                                          const QString& category)
{
    if (midiId.isEmpty() || std::any_of(m_keyingMidi.cbegin(), m_keyingMidi.cend(),
                                        [&](const KeyingMidi& k) { return k.midiId == midiId; })
        || std::any_of(m_actions.cbegin(), m_actions.cend(),
                       [&](const ControlAction& a) { return a.midiId == midiId; })) {
        m_errors << QStringLiteral("%1: duplicate MIDI id").arg(midiId);
        return;
    }
    m_keyingMidi.append({midiId, name, category});
}

bool ControlActionRegistry::guard(const ControlAction& a) const
{
    if (!a.available || a.available())
        return true;
    if (m_refused)
        m_refused(a.name);
    return false;
}

void ControlActionRegistry::publishShortcuts(
    const std::function<void(const ControlActionShortcutSpec&)>& reg) const
{
    for (const ControlAction& a : m_actions) {
        if (a.shortcutId.isEmpty())
            continue;
        // Copies, not references: the lambdas outlive any reallocation of
        // m_actions and the registry entry they were built from.
        const ControlAction act = a;
        const auto self = this;
        switch (a.kind) {
        case ControlAction::Kind::Trigger:
            reg({a.shortcutId, a.name, a.category, a.defaultKey,
                 [self, act] { if (self->guard(act)) act.set(act.rangeMax); },
                 a.autoRepeat});
            break;
        case ControlAction::Kind::Toggle:
            reg({a.shortcutId, a.name, a.category, a.defaultKey,
                 [self, act] {
                     if (!self->guard(act))
                         return;
                     const float mid = (act.rangeMin + act.rangeMax) / 2.0f;
                     act.set(act.get() > mid ? act.rangeMin : act.rangeMax);
                 },
                 a.autoRepeat});
            break;
        case ControlAction::Kind::Level: {
            const auto step = [self, act](float sign) {
                if (!self->guard(act))
                    return;
                const float next = act.get() + sign * act.keyStep;
                act.set(std::clamp(next, act.rangeMin, act.rangeMax));
            };
            reg({upId(a.shortcutId), QStringLiteral("%1 Up").arg(a.name), a.category,
                 QKeySequence(), [step] { step(1.0f); }, /*autoRepeat=*/true});
            reg({downId(a.shortcutId), QStringLiteral("%1 Down").arg(a.name), a.category,
                 QKeySequence(), [step] { step(-1.0f); }, /*autoRepeat=*/true});
            break;
        }
        }
    }
}

void ControlActionRegistry::publishMidi(
    const std::function<void(const ControlActionMidiSpec&)>& reg) const
{
    for (const ControlAction& a : m_actions) {
        if (a.midiId.isEmpty())
            continue;
        const ControlAction act = a;
        const auto self = this;
        ControlActionMidiSpec spec{a.midiId, a.name, a.midiCategory, a.kind,
                                   a.rangeMin, a.rangeMax,
                                   [self, act](float v) { if (self->guard(act)) act.set(v); },
                                   a.get};
        reg(spec);
    }
    for (const KeyingMidi& k : m_keyingMidi) {
        reg({k.midiId, k.name, k.category, ControlAction::Kind::Trigger, 0.0f, 1.0f,
             [](float) {}, {}});
    }
}

bool ControlActionRegistry::stepLevel(const QString& midiId, int steps) const
{
    for (const ControlAction& a : m_actions) {
        if (a.midiId != midiId || a.kind != ControlAction::Kind::Level)
            continue;
        if (guard(a))
            a.set(std::clamp(a.get() + static_cast<float>(steps) * a.keyStep,
                             a.rangeMin, a.rangeMax));
        return true;
    }
    return false;
}

QString ControlActionRegistry::midiIdForShortcut(const QString& shortcutId) const
{
    for (const auto& a : m_actions) {
        if (a.shortcutId == shortcutId && a.kind != ControlAction::Kind::Level)
            return a.midiId;
    }
    for (const auto& p : m_pairs) {
        if (p.shortcutId == shortcutId)
            return p.midiId;
    }
    return {};
}

QString ControlActionRegistry::shortcutIdForMidi(const QString& midiId) const
{
    for (const auto& a : m_actions) {
        if (a.midiId == midiId && a.kind != ControlAction::Kind::Level)
            return a.shortcutId;
    }
    for (const auto& p : m_pairs) {
        if (p.midiId == midiId)
            return p.shortcutId;
    }
    return {};
}

} // namespace AetherSDR
