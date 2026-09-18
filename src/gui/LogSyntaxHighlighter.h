#pragma once

// Extracted verbatim from NetworkDiagnosticsDialog.cpp (#2554). The System Info
// dialog tails the same log file and wants the same colouring; a file-private
// class cannot be shared. Nothing about the highlighter changed in the move.

#include <QColor>
#include <QFont>
#include <QRegularExpression>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>

namespace AetherSDR {

class LogSyntaxHighlighter : public QSyntaxHighlighter {
public:
    explicit LogSyntaxHighlighter(QTextDocument* parent)
        : QSyntaxHighlighter(parent)
    {
        m_timeFormat.setForeground(QColor("#8d99ad"));
        m_debugFormat.setForeground(QColor("#8d99ad"));
        m_infoFormat.setForeground(QColor("#77d8ff"));
        m_warningFormat.setForeground(QColor("#e8b977"));
        m_criticalFormat.setForeground(QColor("#ff6b6b"));
        m_categoryFormat.setForeground(QColor("#d4deea"));
        m_categoryFormat.setFontWeight(QFont::Bold);
        m_numberFormat.setForeground(QColor("#80ed91"));
        m_protocolFormat.setForeground(QColor("#d8b4ff"));
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
