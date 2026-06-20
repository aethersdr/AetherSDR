#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

namespace AetherSDR {

// In-app actionable reminder toast for an upcoming net. This is the *guaranteed*
// notification path: OS-native notifications (tray balloon / Toast / Notification
// Center) can be suppressed by Do-Not-Disturb, missing permissions, or platforms
// that don't render action buttons, and QSystemTrayIcon::showMessage() cannot
// carry a button at all. So the reliable "Tune Now" button lives here, in-app,
// where the action actually happens; the OS notification is only the
// attention-getter that raises the window.
//
// Rendered as a small frameless popup anchored to the bottom-right of its parent
// window, styled like the rest of AetherSDR's chrome.
class NetReminderBanner : public QFrame {
    Q_OBJECT

public:
    explicit NetReminderBanner(QWidget* parent = nullptr);

    // Show the toast for a net. `headline` is the bold line (e.g. "County ARES
    // Net starts in 10 min"), `detail` the dim line (e.g. "146.940 MHz · FM").
    // `canTune` disables the Tune Now button when no radio/slice is available.
    void showReminder(const QString& netId, const QString& headline,
                      const QString& detail, bool canTune);

Q_SIGNALS:
    void tuneRequested(const QString& netId);
    void dismissed(const QString& netId);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void anchorToParent();

    QLabel* m_headline{nullptr};
    QLabel* m_detail{nullptr};
    QPushButton* m_tuneButton{nullptr};
    QString m_netId;
};

} // namespace AetherSDR
