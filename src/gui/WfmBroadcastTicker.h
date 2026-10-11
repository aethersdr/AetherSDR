#pragma once
#include "WfmBroadcastOverlay.h"
#include <QFont>
#include <QFontMetrics>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace AetherSDR {

// Presentation clock only: no metadata ownership, callbacks, or background timer.
// Incoming metadata is already bounded by the receiver/model normalization.
class WfmBroadcastTicker final {
public:
    bool setContent(const WfmBroadcastOverlayRecord& record)
    {
        if (m_haveContent && m_record.sliceId == record.sliceId
            && m_record.sessionId == record.sessionId && m_record.receiverEpoch == record.receiverEpoch
            && m_record.frequencyHz == record.frequencyHz && m_record.program == record.program
            && m_record.stationName == record.stationName && m_record.title == record.title
            && m_record.artist == record.artist) {
            return false; // An accepted control revision alone is not new content.
        }
        m_record = record;
        m_text = record.displayText().simplified();
        m_haveContent = true;
        m_haveLayout = false;
        m_pages.clear();
        m_page = 0;
        m_deadlineMs = -1;
        return true;
    }

    bool layout(const QFont& font, int availableWidth)
    {
        const int width = std::max(1, availableWidth);
        if (!m_haveContent || (m_haveLayout && m_font == font && m_width == width)) { return false; }
        m_font = font;
        m_width = width;
        m_haveLayout = true;
        m_pages.clear();
        const QFontMetrics metrics(font);
        m_labelWidth = std::min(width, metrics.horizontalAdvance(m_text));
        QTextBoundaryFinder lines(QTextBoundaryFinder::Line, m_text);
        QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, m_text);
        qsizetype offset = 0;
        while (offset < m_text.size()) {
            qsizetype end = offset;
            lines.setPosition(offset);
            for (qsizetype next = lines.toNextBoundary(); next >= 0; next = lines.toNextBoundary()) {
                if (metrics.horizontalAdvance(m_text.mid(offset, next - offset).trimmed()) > width) { break; }
                end = next;
            }
            // A single unbroken word may exceed the box. Split only at grapheme
            // boundaries, preserving surrogate pairs and combining sequences.
            if (end == offset) {
                graphemes.setPosition(offset);
                for (qsizetype next = graphemes.toNextBoundary(); next >= 0; next = graphemes.toNextBoundary()) {
                    if (metrics.horizontalAdvance(m_text.mid(offset, next - offset)) > width && end > offset) { break; }
                    end = next;
                    if (metrics.horizontalAdvance(m_text.mid(offset, end - offset)) > width) { break; }
                }
            }
            if (end <= offset) { break; }
            const QString page = m_text.mid(offset, end - offset).trimmed();
            if (!page.isEmpty()) { m_pages.append(page); }
            offset = end;
        }
        m_page = 0;
        m_deadlineMs = -1;
        return true;
    }

    bool advance(qint64 nowMs, bool visible)
    {
        if (!visible || m_pages.size() <= 1) {
            m_deadlineMs = -1;
            return false;
        }
        if (m_deadlineMs < 0) {
            m_deadlineMs = nowMs + holdMs();
            return false;
        }
        if (nowMs < m_deadlineMs) { return false; }
        m_page = (m_page + 1) % m_pages.size();
        // A delayed frame advances once, never races through unread pages.
        m_deadlineMs = nowMs + holdMs();
        return true;
    }

    const QString& fullText() const { return m_text; }
    QString pageText() const { return m_pages.isEmpty() ? m_text : m_pages.at(m_page); }
    const QStringList& pages() const { return m_pages; }
    int labelWidth() const { return m_labelWidth; }
    int pageIndex() const { return m_page; }

private:
    qint64 holdMs() const { return m_page == 0 || m_page == m_pages.size() - 1 ? 5000 : 3000; }
    WfmBroadcastOverlayRecord m_record;
    QString m_text;
    QStringList m_pages;
    QFont m_font;
    int m_width{0};
    int m_labelWidth{0};
    int m_page{0};
    qint64 m_deadlineMs{-1};
    bool m_haveContent{false};
    bool m_haveLayout{false};
};
}
