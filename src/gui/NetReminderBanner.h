#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;

namespace AetherSDR {

// In-app reminder toast for an upcoming net, the guaranteed path with the
// "Tune Now" button: OS notifications may be suppressed or lack buttons
// (QSystemTrayIcon::showMessage() has none) and only raise the window.
// Frameless popup anchored bottom-right of its parent.
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
