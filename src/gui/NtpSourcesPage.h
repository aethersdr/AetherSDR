#pragma once

#include "core/backends/NtpSourceRequest.h"
#include "core/backends/RadioCapabilities.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace AetherSDR {

class ControlAvailabilityRegistry;
class RadioModel;

// Radio Setup → Time: the radio's own NTP time sources (SmartSDR 4.3.16 on the
// BigBend platform: FLEX-8400/8600, Aurora, ML-9600/MLS-9601, CL-9300/CLS-9301).
// Every row is what the radio last reported; Add, Edit, Remove and the Enabled
// box send a request and the table follows the radio's status (Principle II). Unavailable radios keep the page, dimmed with a reason.
class NtpSourcesPage final : public QWidget {
    Q_OBJECT
public:
    explicit NtpSourcesPage(RadioModel& model, QWidget* parent = nullptr);

    // Why the page cannot be used with this radio, or empty.
    static QString unavailableReason(bool connected, const RadioCapabilities& caps);
    // Why a definition cannot be sent, or empty. `adding` also checks that the id
    // is new and that the radio has room for another source, counting creates
    // already sent but not yet reported (pendingCreates).
    static QString definitionIssue(const NtpSourceDefinition& definition, bool adding,
                                   const QList<NtpSourceState>& sources, int maxSources,
                                   const QStringList& pendingCreates = {});
    // FlexLib NtpSource.cs Status and ReachabilityDisplay wording.
    static QString statusText(const NtpSourceState& source);
    static QString reachText(const NtpSourceState& source);

    // Asks the operator to confirm; returns true to go ahead. Replaceable so the
    // page test can answer without a modal box.
    using Confirm = std::function<bool(const QString& title, const QString& text, const QString& accept)>;
    void setConfirm(Confirm confirm) { m_confirm = std::move(confirm); }

private:
    void rebuild();
    void updateButtons();
    void setMessage(const QString& text, const QString& token);
    QString selectedId() const;
    const NtpSourceState* source(const QString& id) const;
    // Enabled sources once the requests still in flight land.
    int enabledCount() const;
    bool pending(const QString& id) const { return m_pending.contains(id); }
    void track(const QString& id, NtpSourceRequest::Action action);
    void untrack(const QString& id, NtpSourceRequest::Action action);
    int maxSources() const;
    void openEditor(const NtpSourceState* existing);
    void closeEditor();
    void validateEditor();
    NtpSourceDefinition editorDefinition() const;
    void saveEditor();
    void removeSelected();
    void requestEnabled(const QString& id, bool enabled);

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QWidget* m_body;
    QTableWidget* m_table;
    QLabel* m_count;
    QPushButton* m_add;
    QPushButton* m_edit;
    QPushButton* m_remove;
    QGroupBox* m_editor;
    QLineEdit* m_id;
    QLineEdit* m_address;
    QComboBox* m_directive;
    QLineEdit* m_description;
    QCheckBox* m_prefer;
    QLabel* m_editorIssue;
    QPushButton* m_save;
    QPushButton* m_cancel;
    QLabel* m_message;
    QLabel* m_reason;
    Confirm m_confirm;
    // Requests sent and not yet answered, by id (Create, Remove, Enable, Disable).
    QHash<QString, NtpSourceRequest::Action> m_pending;
    // Still the session the open editor was filled from.
    std::function<bool()> m_editorSessionCurrent;
    bool m_adding{false};
    bool m_rebuilding{false};
    QString m_pendingSelection;
};

} // namespace AetherSDR
