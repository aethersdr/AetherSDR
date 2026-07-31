#pragma once

#include "PersistentDialog.h"

class QLabel;
class QTableWidget;
class QTimer;

namespace AetherSDR {

class RadioModel;

// Live view of the connected radio's health/status registers.
//
// The values come from IRadioBackend::healthSnapshot(), so the dialog knows
// nothing about any particular radio family: the backend decides which
// registers exist, what they are called and how they group. A family that
// reports none gets an explanatory message instead of an empty table, because
// "this radio does not report health registers" and "every register reads zero"
// are completely different situations and the operator has to be able to tell
// them apart.
//
// This is a DIAGNOSTIC read-out, not a control surface — nothing here writes to
// the radio. The one thing it must never do is invent a value: a register the
// radio has not reported shows a dash.
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
