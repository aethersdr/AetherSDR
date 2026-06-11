#pragma once

#include "PersistentDialog.h"

class QShowEvent;
class QTableWidget;

namespace AetherSDR {

class AprsMessenger;

// The envelope window: every APRS message sent or received, newest last,
// with delivery state for outgoing traffic (sent / acked / failed). Opening
// the dialog marks everything read, which clears the unread badge on the
// envelope button in AetherModem's APRS tab. Double-clicking a row asks the
// owner to prefill a reply to that station.
class AprsMessagesDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit AprsMessagesDialog(AprsMessenger* messenger, QWidget* parent = nullptr);

signals:
    // Emitted on row double-click; the AetherModem dialog prefills the
    // message "To" field with this callsign and focuses the text entry.
    void replyRequested(const QString& callsign);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void rebuild();

    AprsMessenger* m_messenger{nullptr};
    QTableWidget* m_table{nullptr};
};

} // namespace AetherSDR
