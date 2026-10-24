#pragma once

#include "gui/CanonWindow.h"

#include <QPointer>
#include <QString>
#include <QStringList>

class QCheckBox;
class QFrame;
class QScrollArea;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace AetherSDR {

class FirmwareUpdateCard;
class FlowLayout;
class RadioModel;
class UpdateChecker;

// File → Update...: what AetherSDR release is newest, what is in it, and the
// download built for this computer (RFC #5304); below it, the connected
// radio's firmware (FirmwareUpdateCard). Made to make an update worth
// installing, after aethersdr.com/experience. Everything it shows comes from
// UpdateChecker: the parsed version, and the release's tagline and chapters
// as plain text. Opening it without the release's details starts a check.
class UpdatesWindow : public CanonWindow {
    Q_OBJECT

public:
    // `radio` may be null (no radio section).
    UpdatesWindow(UpdateChecker* checker, RadioModel* radio, QWidget* parent = nullptr);

    // Scroll the radio firmware section into view (the status bar's
    // out-of-date firmware version opens the window here).
    void showFirmware();

    // Ask GitHub again now (Check Now, and Help → Check for Updates...).
    void checkNow();

    // Mirrors Help → Check for Updates Automatically.
    void setAutomaticCheck(bool on);

signals:
    // The badge the title bar should show now (empty clears it).
    void badgeChanged(const QString& version);
    // The window's Check daily box changed; MainWindow keeps the Help menu
    // toggle and the settings in step.
    void automaticCheckToggled(bool on);

protected:
    // Not while firmware is being written: every close path asks first.
    void done(int result) override;

private:
    friend class UpdatesWindowTestAccess;

    enum class View { Checking, Available, Current, Failed };

    void refresh();
    View currentView() const;
    void rebuildRelease();
    void openDownload();

    QPointer<UpdateChecker> m_checker;
    bool m_failed{false};
    bool m_firmwareBusy{false};
    FirmwareUpdateCard* m_firmware{nullptr};
    QScrollArea* m_scroll{nullptr};

    QWidget*     m_header{nullptr};
    QLabel*      m_eyebrow{nullptr};
    QWidget*     m_version{nullptr};
    QLabel*      m_tagline{nullptr};
    QLabel*      m_note{nullptr};
    QLabel*      m_chaptersTitle{nullptr};
    QWidget*     m_chips{nullptr};
    FlowLayout*  m_chipsLayout{nullptr};
    QPushButton* m_get{nullptr};
    QPushButton* m_notes{nullptr};
    QPushButton* m_skip{nullptr};
    QPushButton* m_checkNow{nullptr};
    QLabel*      m_footnote{nullptr};
    QCheckBox*   m_autoCheck{nullptr};
    QStringList  m_builtChapters;   // the chapters the chips were built from
};

} // namespace AetherSDR
