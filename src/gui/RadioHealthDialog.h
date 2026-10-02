#pragma once

#include "PersistentDialog.h"

class QLabel;
class QTableWidget;
class QTimer;

namespace AetherSDR {

class RadioModel;

// Live view of the radio's health/status registers from
// IRadioBackend::healthSnapshot(); the backend decides which registers exist
// and how they group. A family reporting none gets an explanatory message, not
// an empty table. Read-only; an unreported register shows a dash, never a value.
class RadioHealthDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit RadioHealthDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    void refresh();
    void copyToClipboard();
    // Format one snapshot value for display. Booleans become Yes/No rather
    // than true/false, and an invalid variant becomes an em dash.
    static QString formatValue(const QVariant& v);

    RadioModel* m_model{nullptr};
    QTableWidget* m_table{nullptr};
    QLabel* m_statusLabel{nullptr};
    QTimer* m_refreshTimer{nullptr};
    // The row layout is rebuilt only when the KEY SET changes, not on every
    // tick: rebuilding a QTableWidget every refresh would drop the operator's
    // selection and scroll position twice a second while they were reading it.
    QStringList m_currentKeys;
};

} // namespace AetherSDR
