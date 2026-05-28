#pragma once
#include <QtGlobal>
#ifdef Q_OS_LINUX

#include "PersistentDialog.h"

#include <QList>
#include <QPoint>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QPaintEvent;
class QResizeEvent;

namespace AetherSDR {

class EvdevEncoderManager;
class MidiControlManager;
class ShortcutManager;
class UlanziDialCanvas;

// Visual mapping editor for the Ulanzi Dial.  Renders a stylized
// representation of the dial in the centre and "callout" pills radiating
// outward to each physical control.  Each pill shows the AetherSDR
// action it triggers and (after the user runs Learn mode) the captured
// device-event signature that fires it.  See #3232 for the cross-platform
// design.
//
// Workflow:
//   1. Click a pill → enters Learn mode for that control.
//   2. Press the corresponding physical button on the dial.
//   3. The captured signature (e.g. "KEY_PLAYPAUSE", "Ctrl+V") is bound
//      to that pill and persisted to AppSettings.
//   4. From then on, when the dial fires that signature, the pill's
//      AetherSDR action is dispatched.  (Dispatch wiring lands in a
//      follow-up PR; this dialog only owns capture + persistence.)
class UlanziDialMapperDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit UlanziDialMapperDialog(EvdevEncoderManager* manager,
                                    ShortcutManager*     shortcuts,
                                    MidiControlManager*  midi,
                                    QWidget*             parent = nullptr);

    friend class UlanziDialCanvas;

protected:
    // Re-run layoutPills after the dialog is fully shown.  Until that
    // point, m_canvas->mapTo/mapFrom return values relative to an
    // uninitialised position, so any "centre on window" calc has to
    // wait for the layout to settle.
    void showEvent(QShowEvent* event) override;

public:
    // Hardcoded signature → pill_id lookup.  Used by MainWindow's button
    // dispatcher to find which pill (and therefore which action) a fired
    // device event maps to.  Returns empty string if no pill claims it.
    static QString pillForSignature(const QString& signature);

    // AppSettings key for the user-chosen action on a given pill.  Used
    // both by the dialog (to load/save the combo selection) and by
    // MainWindow's dispatcher (to look up what to do on press).
    static QString actionSettingsKey(const QString& pillId);

    // Built-in default action for a pill (e.g. "shortcut:mox_toggle"),
    // used by MainWindow's dispatcher as the AppSettings fallback so
    // bindings work on first launch even if the user has never opened
    // this dialog.  Returns "None" for unknown pillIds.
    static QString defaultActionForPill(const QString& pillId);

private slots:
    void onTuneSteps(int steps);
    void onButtonEvent(const QString& signature, int action);
    void onConnectionChanged(bool connected, const QString& name);

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
        QPoint  pillCenter;     // computed each resize, screen coords
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

    EvdevEncoderManager* m_manager{nullptr};
    ShortcutManager*     m_shortcuts{nullptr};
    MidiControlManager*  m_midi{nullptr};
    UlanziDialCanvas*    m_canvas{nullptr};
    QList<Pill> m_pills;

    QLabel* m_statusLabel{nullptr};
    QLabel* m_lastEventLabel{nullptr};
    QPushButton* m_resetBtn{nullptr};
    QPushButton* m_closeBtn{nullptr};
};

} // namespace AetherSDR

#endif // Q_OS_LINUX
