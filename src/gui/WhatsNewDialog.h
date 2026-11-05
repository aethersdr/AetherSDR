#pragma once

#include "CanonWindow.h"

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
// The release notes for this version, fetched from GitHub, in a short-lived
// canon window (RFC #6226, #7045). The notes' colours come from canon tokens
// in this window's scope and are rebuilt on a theme change.
class WhatsNewDialog : public CanonWindow {
    Q_OBJECT
    friend class WhatsNewDialogTestAccess;

public:
    // Show GitHub release notes for currentVersion.
    // If lastSeen is empty (first install), uses a welcome heading.
    // currentVersionOnly is used by Help -> What's New so it is not treated
    // as a first-run welcome.
    explicit WhatsNewDialog(const QString& lastSeenVersion,
                            const QString& currentVersion,
                            QWidget* parent = nullptr,
                            bool showUpgrade = false,
                            bool currentVersionOnly = false);

    // Show all entries for the current version (for Help menu).
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
    // Re-render whatever the browser shows (loading, error or notes) with
    // the current theme's colours.
    void rerender();
    bool findInNotes(const QString& text);
    void setStatusText(const QString& text);
    QString releaseTag() const;

    QString m_currentVersion;
    QString m_lastFindText;
    enum class Content { Loading, Error, Notes } m_content{Content::Loading};
    QString m_errorMessage;
    QString m_notesMarkdown;
    // Tests build the window without reaching GitHub (socket-free).
    static inline bool s_fetchReleaseNotes = true;
    bool m_isWelcome{false};

    QPointer<QTextBrowser> m_browser;
    QPointer<QLabel> m_statusLabel;
};

} // namespace AetherSDR
