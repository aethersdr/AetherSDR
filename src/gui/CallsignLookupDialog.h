#pragma once

#include "PersistentDialog.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace AetherSDR {

class CallsignCard;

// View → Callsign Lookup: type a callsign, get the large contact card.
// Results ride CallsignLookupService (same QRZ client + 7-day cache the
// live CW contacts window uses), so repeated lookups cost nothing and the two
// surfaces always agree.
class CallsignLookupDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit CallsignLookupDialog(QWidget* parent = nullptr);

    // Pre-fill and immediately look up (context-menu entry points).
    void lookupCallsign(const QString& call);

private:
    void startLookup(bool forceRefresh);
    void setStatus(const QString& text);

    QLineEdit*    m_input{nullptr};
    QPushButton*  m_lookupBtn{nullptr};
    QPushButton*  m_refreshBtn{nullptr};
    CallsignCard* m_card{nullptr};
    QLabel*       m_status{nullptr};
    QString       m_pendingCall;
};

class LiveCwContactsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit LiveCwContactsDialog(int textFontPx, QWidget* parent = nullptr);
    CallsignCard* card() const { return m_card; }
    void showCallsign(const QString& call);
    void clearContact();

private:
    void applyTheme();

    CallsignCard* m_card{nullptr};
    QLabel* m_waiting{nullptr};
    QPushButton* m_closeBtn{nullptr};
    int m_textFontPx{18};
};

} // namespace AetherSDR
