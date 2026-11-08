#pragma once
#include <QtGlobal>

#include "CanonWindow.h"
#include "core/UlanziDialBackend.h"

#include <QList>
#include <QPoint>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QPaintEvent;
class QResizeEvent;

namespace AetherSDR {

class MidiControlManager;
class ShortcutManager;
class UlanziDialCanvas;

// Visual mapping editor for the Ulanzi Dial (#3232): the dial drawn centrally
// with callout pills per physical control, each showing its action and learned
// device-event signature. Click a pill to Learn, press the dial control, and the
// signature (e.g. "KEY_PLAYPAUSE", "Ctrl+V") is persisted to AppSettings.
// This dialog owns capture + persistence; MainWindow dispatches. A canon
// dialog (style guide, RFC #6226), opened from Settings → Controllers →
// Ulanzi Dial Mapping...
class UlanziDialMapperDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit UlanziDialMapperDialog(UlanziDialBackend* manager,
                                    ShortcutManager*     shortcuts,
                                    MidiControlManager*  midi,
                                    QWidget*             parent = nullptr);

    friend class UlanziDialCanvas;

protected:
    // Re-reads the backend's state, so a dialog opened after the scan
    // reported still shows the right status line.
    void showEvent(QShowEvent* event) override;

public:
    // Hardcoded signature → pill_id lookup.  Used by MainWindow's button
    // dispatcher to find which pill (and therefore which action) a fired
    // device event maps to.  Returns empty string if no pill claims it.
    static QString pillForSignature(const QString& signature);

    // Every pill id, in dial-layout order.
    static QStringList allPillIds();

    // One-shot claim of the pre-#4611 flat keys into the mappings document.
    // Call once at controller-wiring time, before any lookup.
    static void migrateLegacyMappings();

    // The action bound to a pill, falling back to defaultActionForPill() when
    // the mappings document has no entry for it.
    static QString actionForPill(const QString& pillId);

    // Bind an action to a pill and persist immediately.
    static void setActionForPill(const QString& pillId, const QString& actionId);

    // Re-derive the status line from the backend's current state and the
    // enable setting. MainWindow calls it after the setting changes, so an
    // open (or later reopened) dialog never keeps a stale line.
    void refreshStatus();

    // Built-in default action for a pill (e.g. "shortcut:rit_toggle"),
    // used by MainWindow's dispatcher as the AppSettings fallback so
    // bindings work on first launch even if the user has never opened
    // this dialog.  Returns "None" for unknown pillIds.
    static QString defaultActionForPill(const QString& pillId);

private slots:
    void onTuneSteps(int steps);
    void onButtonEvent(const QString& signature, int action);
    void onConnectionChanged(bool connected, const QString& name);
    // Status line for a dial turned off in Radio Setup: without it the dialog
    // reads "Disconnected" with nothing to say why.
    void showDisabledStatus();
    // Connected, disconnected, warning, error or off: the status line
    // restyles from the ulanziState property.
    void setStatusState(const QString& state);
#ifdef Q_OS_LINUX
    // Dial present but its evdev node isn't accessible — offer to install the
    // udev access rule via polkit.
    void onAccessRequired(const QString& deviceName);
    void onAccessCleared();
    void onGrantAccessClicked();
#endif

private:
    struct Pill {
        QString id;             // stable identifier for AppSettings key
        QString defaultLabel;   // physical-control label (e.g. "Top-Left")
        QString signature;      // immutable device-event signature this
                                // pill represents (e.g. "KEY_PREVIOUSSONG").
                                // Empty for the rotary (handled separately).
        QString defaultAction;  // default function on this pill (matches
                                // the original mockup labels).
        QPoint  anchor;         // dial-image anchor (normalized × body rect)
        QPoint  pillCenter;     // computed each layout, canvas coords
        QComboBox* combo{nullptr};
    };

    // Logical anchor point on the rendered dial body (0..1 coordinates so
    // it scales with the centre-image rect).
    static QPoint normalizedAnchor(double nx, double ny);

    void buildPills();
    void layoutPills();
    void loadActions();
    void saveAction(int pillIndex);
    void refreshPillLabel(int pillIndex);

    QRect dialBodyRect() const;
    QPoint anchorScreenPos(const QPoint& norm) const;

    UlanziDialBackend* m_manager{nullptr};
    ShortcutManager*     m_shortcuts{nullptr};
    MidiControlManager*  m_midi{nullptr};
    UlanziDialCanvas*    m_canvas{nullptr};
    QList<Pill> m_pills;
    bool m_isLoading{false};

    QLabel*    m_rotaryLabel{nullptr};
    QComboBox* m_rotaryCombo{nullptr};
    QLabel*    m_singleTapLabel{nullptr};

    QLabel* m_statusLabel{nullptr};
    QLabel* m_lastEventLabel{nullptr};
    QPushButton* m_resetBtn{nullptr};
    QPushButton* m_closeBtn{nullptr};
#ifdef Q_OS_LINUX
    QPushButton* m_grantAccessBtn{nullptr};  // shown only on accessRequired
#endif
};

} // namespace AetherSDR
