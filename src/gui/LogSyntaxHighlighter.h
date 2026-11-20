#pragma once

// Shared by the Network Diagnostics and Runtime Monitor log viewers.

#include "core/ThemeManager.h"

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
    void applyThemeColors()
    {
        const auto& tm = ThemeManager::instance();
        m_timeFormat.setForeground(tm.color(m_scope, "color.canon.muted"));
        m_debugFormat.setForeground(tm.color(m_scope, "color.canon.muted"));
        m_infoFormat.setForeground(tm.color(m_scope, "color.canon.category.cyan"));
        m_warningFormat.setForeground(tm.color(m_scope, "color.canon.category.amber"));
        m_criticalFormat.setForeground(tm.color(m_scope, "color.canon.category.rose"));
        m_categoryFormat.setForeground(tm.color(m_scope, "color.canon.ink"));
        m_categoryFormat.setFontWeight(QFont::Bold);
        m_numberFormat.setForeground(tm.color(m_scope, "color.canon.category.teal"));
        m_protocolFormat.setForeground(tm.color(m_scope, "color.canon.category.violet"));
        rehighlight();
    }

    QPointer<const QWidget> m_scope;
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
