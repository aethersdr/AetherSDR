#pragma once

#include "ShortcutActionMarker.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QEvent;

namespace AetherSDR {

class ControlActionRegistry;

// Appends a marked control's bound key, read from ControlActionRegistry, to its
// tooltip and accessible description (RFC #5484). An unbound key changes
// nothing. A control the owner disabled carries its availability reason, which
// stays and follows the key. The owner's own texts are kept as the base and
// re-read whenever the owner rewrites them, so the suffix never stacks.
class ShortcutTooltips : public QObject {
    Q_OBJECT
public:
    explicit ShortcutTooltips(const ControlActionRegistry& registry,
                              QObject* parent = nullptr);

    // `keyText` empty returns `base` unchanged. `available` false means `base`
    // is the control's availability reason.
    static QString compose(const QString& base, const QString& keyText,
                           bool shortcutsEnabled, bool available);

    // The Settings → Keyboard Shortcuts switch: off qualifies the key, never
    // hides it.
    void setShortcutsEnabled(bool enabled);
    bool shortcutsEnabled() const { return m_shortcutsEnabled; }

    // Re-read `root` and every marked control under it now, and watch any not
    // yet watched. For an owner that rewrites a description, which sends no
    // event, or builds a marked control after refresh().
    void annotate(QWidget* root);

public slots:
    // Watch every marked control in the application and re-read every
    // binding. Call once the controls exist; connect to bindingsChanged.
    void refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void collect();
    void watch(QWidget* w);
    void annotateOne(QWidget* w);
    void annotateWatched();
    void schedule(QWidget* w);
    void flush();

    const ControlActionRegistry& m_registry;
    QVector<QPointer<QWidget>> m_watched;
    QVector<QPointer<QWidget>> m_pending;
    bool m_shortcutsEnabled{false};
    bool m_flushQueued{false};
    bool m_writing{false};
};

} // namespace AetherSDR
