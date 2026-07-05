#pragma once

#include "PersistentDialog.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace AetherSDR {

class CallsignCard;

// View → Callsign Lookup: type a callsign, get the large contact card.
// Results ride CallsignLookupService (same QRZ client + 7-day cache the
// CW decoder card uses), so repeated lookups cost nothing and the two
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

} // namespace AetherSDR
