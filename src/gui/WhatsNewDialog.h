#pragma once

#include "PersistentDialog.h"

#include <QPointer>
#include <QString>

class QLabel;
class QPushButton;
class QTextBrowser;

namespace AetherSDR {

// "What's New" dialog shown on first launch after a version change.
// Displays release notes between lastSeenVersion and currentVersion,
// rendered as styled HTML in a scrollable text browser.
//
// Also accessible via Help → What's New (#483).
class WhatsNewDialog : public PersistentDialog {
    Q_OBJECT

public:
    // Show GitHub release notes for currentVersion.
    // If lastSeen is empty (first install), uses a welcome heading.
    // currentVersionOnly is used by Help -> What's New so it is not treated
    // as a first-run welcome.
    // useLatestRelease fetches the repo's newest published release instead of
    // the release tagged for currentVersion — used by Help -> What's New, where
    // the running build's exact version may not have a published GitHub release
    // yet (dev builds, point releases, release lag) and the tag lookup 404s.
    explicit WhatsNewDialog(const QString& lastSeenVersion,
                            const QString& currentVersion,
                            QWidget* parent = nullptr,
                            bool showUpgrade = false,
                            bool currentVersionOnly = false,
                            bool useLatestRelease = false);

    // Show the newest published release notes (for Help menu).
    static WhatsNewDialog* showAll(QWidget* parent);

private:
    void buildUI(const QString& lastSeenVersion, const QString& currentVersion,
                  bool showUpgrade, bool currentVersionOnly);
    void fetchLiveReleaseNotes();
    void showLoadingState();
    void showReleaseLoadError(const QString& message);
    void showLiveReleaseNotes(const QString& title,
                              const QString& tagName,
                              const QString& publishedAt,
                              const QString& bodyMarkdown);
    void promptFind();
    bool findInNotes(const QString& text);
    void setStatusText(const QString& text);
    QString releaseTag() const;

    QString m_currentVersion;
    QString m_lastFindText;
    bool m_isWelcome{false};
    bool m_useLatestRelease{false};

    QPointer<QTextBrowser> m_browser;
    QPointer<QLabel> m_statusLabel;
};

} // namespace AetherSDR
