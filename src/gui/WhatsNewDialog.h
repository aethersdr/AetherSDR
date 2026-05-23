#pragma once

#include <QDialog>

namespace MasterSDR {

struct ReleaseEntry;

// "What's New" dialog shown on first launch after a version change.
// Displays release notes between lastSeenVersion and currentVersion,
// rendered as styled HTML in a scrollable text browser.
//
// Also accessible via Help → What's New (#483).
class WhatsNewDialog : public QDialog {
    Q_OBJECT

public:
    // Show changes between lastSeen and current version.
    // If lastSeen is empty (first install), shows only the current version.
    explicit WhatsNewDialog(const QString& lastSeenVersion,
                            const QString& currentVersion,
                            QWidget* parent = nullptr,
                            bool showUpgrade = false);

    // Show all entries for the current version (for Help menu).
    static WhatsNewDialog* showAll(QWidget* parent);

private:
    void buildUI(const QString& lastSeenVersion, const QString& currentVersion,
                  bool showUpgrade);
    QString renderHtml(const std::vector<ReleaseEntry>& entries, bool isWelcome) const;
};

} // namespace MasterSDR
