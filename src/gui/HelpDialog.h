#pragma once

#include "CanonWindow.h"
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace AetherSDR {

// One bundled offline guide, in a canon workspace window (RFC #6226, #7049),
// with a find row over the rendered Markdown.
class HelpDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit HelpDialog(const QString& windowTitle,
                        const QString& resourcePath,
                        QWidget* parent = nullptr);

private:
    void buildUI(const QString& resourcePath);
    void applyLinkColour();
    void focusFindField();
    void findNext();
    void findPrevious();
    void runFind(bool backward);
    QString loadMarkdown(const QString& resourcePath) const;
    void resetFindState();
    void updateFindFeedback(const QString& message, bool noMatch);

    QTextBrowser* m_browser{nullptr};
    QLineEdit* m_findEdit{nullptr};
    QPushButton* m_findButton{nullptr};
    QPushButton* m_findPrevButton{nullptr};
    QLabel* m_findStatus{nullptr};
};

} // namespace AetherSDR
