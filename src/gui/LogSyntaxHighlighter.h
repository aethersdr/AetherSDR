#pragma once

// Shared by the Network Diagnostics and Runtime Monitor log viewers.

#include "core/ThemeManager.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QWidget>

namespace AetherSDR {

class LogSyntaxHighlighter : public QSyntaxHighlighter {
public:
    LogSyntaxHighlighter(QTextDocument* parent, const QWidget* scope)
        : QSyntaxHighlighter(parent)
        , m_scope(scope)
    {
        applyThemeColors();
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, &LogSyntaxHighlighter::applyThemeColors);
    }

protected:
    void highlightBlock(const QString& text) override
    {
        static const QRegularExpression timeRe(QStringLiteral("^\\[[^\\]]+\\]"));
        static const QRegularExpression levelRe(QStringLiteral("\\]\\s+(DBG|INF|WRN|CRT|FTL)\\s+"));
        static const QRegularExpression categoryRe(QStringLiteral("\\]\\s+(?:DBG|INF|WRN|CRT|FTL)\\s+([^:]+):"));
        static const QRegularExpression numberRe(QStringLiteral("\\b(?:0x[0-9a-fA-F]+|\\d+(?:\\.\\d+)?)\\b"));
        static const QRegularExpression protocolRe(QStringLiteral("\\b(?:C\\d+|R\\d+|S[0-9a-fA-F]+|VITA-49|UDP|TCP|RX|TX)\\b"));

        QRegularExpressionMatch match = timeRe.match(text);
        if (match.hasMatch()) {
            setFormat(match.capturedStart(), match.capturedLength(), m_timeFormat);
        }

        match = levelRe.match(text);
        if (match.hasMatch()) {
            const QString level = match.captured(1);
            QTextCharFormat levelFormat = m_debugFormat;
            if (level == "INF") {
                levelFormat = m_infoFormat;
            } else if (level == "WRN") {
                levelFormat = m_warningFormat;
            } else if (level == "CRT" || level == "FTL") {
                levelFormat = m_criticalFormat;
            }
            setFormat(match.capturedStart(1), match.capturedLength(1), levelFormat);
        }

        match = categoryRe.match(text);
        if (match.hasMatch()) {
            setFormat(match.capturedStart(1), match.capturedLength(1), m_categoryFormat);
        }

        QRegularExpressionMatchIterator it = numberRe.globalMatch(text);
        while (it.hasNext()) {
            match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), m_numberFormat);
        }

        it = protocolRe.globalMatch(text);
        while (it.hasNext()) {
            match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), m_protocolFormat);
        }
    }

private:
    // color.log.* is the log viewers' own text group: every value clears
    // 4.5:1 on color.canon.nested, the ground both panes draw on, in both
    // bundled themes. color.canon.category.* stays marks-only (#7037).
    void applyThemeColors()
    {
        const auto& tm = ThemeManager::instance();
        const QColor time     = tm.color(m_scope, "color.log.time");
        const QColor info     = tm.color(m_scope, "color.log.info");
        const QColor warning  = tm.color(m_scope, "color.log.warning");
        const QColor critical = tm.color(m_scope, "color.log.critical");
        const QColor category = tm.color(m_scope, "color.log.category");
        const QColor number   = tm.color(m_scope, "color.log.number");
        const QColor protocol = tm.color(m_scope, "color.log.protocol");

        // themeChanged also fires on every Theme Editor token edit; only
        // re-run the document when a log color actually moved.
        const bool unchanged = m_timeFormat.foreground().color() == time
            && m_infoFormat.foreground().color() == info
            && m_warningFormat.foreground().color() == warning
            && m_criticalFormat.foreground().color() == critical
            && m_categoryFormat.foreground().color() == category
            && m_numberFormat.foreground().color() == number
            && m_protocolFormat.foreground().color() == protocol;
        if (unchanged && m_formatsResolved) {
            return;
        }

        m_timeFormat.setForeground(time);
        m_debugFormat.setForeground(time);
        m_infoFormat.setForeground(info);
        m_warningFormat.setForeground(warning);
        m_criticalFormat.setForeground(critical);
        m_categoryFormat.setForeground(category);
        m_categoryFormat.setFontWeight(QFont::Bold);
        m_numberFormat.setForeground(number);
        m_protocolFormat.setForeground(protocol);
        m_formatsResolved = true;
        rehighlight();
    }

    QPointer<const QWidget> m_scope;
    bool m_formatsResolved = false;
    QTextCharFormat m_timeFormat;
    QTextCharFormat m_debugFormat;
    QTextCharFormat m_infoFormat;
    QTextCharFormat m_warningFormat;
    QTextCharFormat m_criticalFormat;
    QTextCharFormat m_categoryFormat;
    QTextCharFormat m_numberFormat;
    QTextCharFormat m_protocolFormat;
};

} // namespace AetherSDR
