#pragma once

#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace AetherSDR {

class ShortcutManager;

// One declaration per operator action, published to every input surface.
//
// Keyboard shortcuts (ShortcutManager) and MIDI (MidiControlManager) keep
// their own persistence and dispatch. This registry is the single place an
// action is declared: add() once, and publishShortcuts()/publishMidi() register
// it on each surface under the id that surface persists. Actions declared
// before the registry existed are recorded with addPair(), which publishes
// nothing and only tells consumers (the Ulanzi mapper, the parity test) that a
// shortcut id and a MIDI id are the same action.
//
// Radio neutrality: an action's `available` predicate reads capabilities and
// model state, never a family string. An unavailable action refuses with the
// registry's refusal callback instead of silently doing nothing.
//
// TX keying is out of scope here: keying actions go through the TxController
// paths (registerTxShortcut, dispatchScopedMidiTx) that own Principle VI.
struct ControlAction {
    enum class Kind {
        Trigger,  // fire once: set(1)
        Toggle,   // keyboard flips get(); MIDI sets on/off
        Level,    // continuous; keyboard gets an Up/Down pair stepping by keyStep
    };

    QString shortcutId;    // [A-Za-z0-9_]+; empty = keyboard side declared elsewhere
    QString midiId;        // empty = MIDI side declared elsewhere
    QString name;          // operator-facing name
    QString category;      // ShortcutManager::categories() entry
    QString midiCategory;  // MIDI dialog group; empty = category
    Kind kind{Kind::Trigger};
    float rangeMin{0.0f};
    float rangeMax{1.0f};
    float keyStep{1.0f};   // Level only
    QKeySequence defaultKey;           // Trigger/Toggle; new actions ship unbound
    bool autoRepeat{false};
    std::function<void(float)> set;    // scaled to rangeMin..rangeMax
    std::function<float()> get;        // required for Toggle and Level
    std::function<bool()> available;   // null = always available

    static ControlAction trigger(QString shortcutId, QString midiId, QString name,
                                 QString category, std::function<void(float)> set)
    {
        ControlAction a;
        a.shortcutId = std::move(shortcutId);
        a.midiId = std::move(midiId);
        a.name = std::move(name);
        a.category = std::move(category);
        a.set = std::move(set);
        return a;
    }
    static ControlAction toggle(QString shortcutId, QString midiId, QString name,
                                QString category, std::function<void(float)> set,
                                std::function<float()> get)
    {
        ControlAction a = trigger(std::move(shortcutId), std::move(midiId), std::move(name),
                                  std::move(category), std::move(set));
        a.kind = Kind::Toggle;
        a.get = std::move(get);
        return a;
    }
    static ControlAction level(QString shortcutId, QString midiId, QString name,
                               QString category, float lo, float hi, float step,
                               std::function<void(float)> set, std::function<float()> get)
    {
        ControlAction a = toggle(std::move(shortcutId), std::move(midiId), std::move(name),
                                 std::move(category), std::move(set), std::move(get));
        a.kind = Kind::Level;
        a.rangeMin = lo;
        a.rangeMax = hi;
        a.keyStep = step;
        return a;
    }
    ControlAction&& inMidiCategory(QString c) && { midiCategory = std::move(c); return std::move(*this); }
    ControlAction&& repeating() && { autoRepeat = true; return std::move(*this); }
    ControlAction&& when(std::function<bool()> f) && { available = std::move(f); return std::move(*this); }
};

// What publishMidi() hands the MIDI side, kept free of MIDI types so this
// registry builds without HAVE_MIDI.
struct ControlActionMidiSpec {
    QString id;
    QString name;
    QString category;
    ControlAction::Kind kind;
    float rangeMin;
    float rangeMax;
    std::function<void(float)> set;
    std::function<float()> get;
};

class ControlActionRegistry {
public:
    // Called with the action name when an unavailable action is invoked.
    void setRefusalHandler(std::function<void(const QString&)> handler)
    {
        m_refused = std::move(handler);
    }

    // Returns false (and records nothing) for a malformed declaration:
    // a bad shortcut id, a duplicate id, or a Toggle/Level without get().
    bool add(ControlAction action);
    void addPair(const QString& shortcutId, const QString& midiId);

    void publishShortcuts(ShortcutManager& shortcuts) const;
    void publishMidi(const std::function<void(const ControlActionMidiSpec&)>& reg) const;

    // Shortcut ids a Level action publishes as its keyboard pair.
    static QString upId(const QString& shortcutId) { return shortcutId + QStringLiteral("_up"); }
    static QString downId(const QString& shortcutId) { return shortcutId + QStringLiteral("_down"); }
    static bool isValidShortcutId(const QString& id);

    // The other surface's id for the same action; empty when there is none.
    QString midiIdForShortcut(const QString& shortcutId) const;
    QString shortcutIdForMidi(const QString& midiId) const;

    const QVector<ControlAction>& actions() const { return m_actions; }
    QStringList errors() const { return m_errors; }

private:
    bool guard(const ControlAction& a) const;
    bool knownShortcutId(const QString& id) const;
    bool knownMidiId(const QString& id) const;

    struct Pair { QString shortcutId; QString midiId; };
    QVector<ControlAction> m_actions;
    QVector<Pair> m_pairs;
    QStringList m_errors;
    std::function<void(const QString&)> m_refused;
};

} // namespace AetherSDR
