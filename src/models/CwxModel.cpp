#include "CwxModel.h"
#include <QDebug>
#include <QMap>

namespace AetherSDR {

CwxModel::CwxModel(QObject* parent)
    : QObject(parent)
{}

QString CwxModel::macro(int idx) const
{
    if (idx < 0 || idx >= 12) return {};
    return m_macros[idx];
}

QVector<CwxModel::SpeedSegment>
CwxModel::expandSpeedModifiers(const QString& text, int baseWpm, int step)
{
    QVector<SpeedSegment> segs;
    if (text.isEmpty())
        return segs;

    QString accumText;
    int     accumWpm    = baseWpm;
    bool    prevWasSpace = true;  // string start acts like after-space

    for (int i = 0; i < text.size(); ) {
        // Any whitespace (space, tab, newline) is a word boundary and keys as
        // a word gap. Macros come from multi-line QTextEdit widgets, so a raw
        // '\n'/'\t' must not be treated as a word char (it would bleed a
        // modifier across the line and, un-DEL-encoded, corrupt the cwx send
        // command). Normalize to a single space. (#3976 review)
        if (text[i].isSpace()) {
            accumText += ' ';
            prevWasSpace = true;
            ++i;
            continue;
        }

        // Count +/- modifier prefix — only valid at a word boundary
        int plus = 0, minus = 0;
        int j = i;
        if (prevWasSpace) {
            while (j < text.size() && (text[j] == '+' || text[j] == '-')) {
                if (text[j] == '+') ++plus; else ++minus;
                ++j;
            }
            // Treat as modifier only when immediately followed by a word char
            // (non-space, non-modifier).  Standalone +/- are prosigns/hyphens.
            if (j >= text.size() || text[j].isSpace()) {
                plus = minus = 0;
                j = i;
            }
        }

        // Scan word body to end-of-word
        const int wordStart = j;
        while (j < text.size() && !text[j].isSpace()) {
            ++j;
        }
        const QString wordBody = text.mid(wordStart, j - wordStart);

        const bool hasModifier = (plus > 0 || minus > 0);
        const int  delta   = (plus - minus) * step;
        const int  wordWpm = hasModifier
                           ? qBound(5, baseWpm + delta, 100)
                           : baseWpm;

        if (wordWpm != accumWpm) {
            if (!accumText.isEmpty())
                segs.append({accumText, accumWpm});
            accumText.clear();
            accumWpm = wordWpm;
        }

        accumText   += wordBody;
        prevWasSpace = false;

        if (hasModifier) {
            // Speed resets to base after each modifier word
            segs.append({accumText, accumWpm});
            accumText.clear();
            accumWpm = baseWpm;
        }

        i = j;
    }

    if (!accumText.isEmpty())
        segs.append({accumText, accumWpm});

    return segs;
}

void CwxModel::emitExpandedSend(const QVector<SpeedSegment>& segs)
{
    int cmdWpm = m_speed;
    for (const SpeedSegment& seg : segs) {
        if (seg.wpm != cmdWpm) {
            emit commandReady(QString("cwx wpm %1").arg(seg.wpm));
            ++m_pendingWpmEchoes;   // swallow this transient's echo (#3976)
            cmdWpm = seg.wpm;
        }
        QString encoded = seg.text;
        encoded.replace(' ', QChar(0x7f));
        if (!encoded.isEmpty())
            emit commandReady(
                QString("cwx send \"%1\" %2").arg(encoded).arg(m_nextBlock++));
        if (!seg.text.isEmpty())
            emit transmissionRequested(seg.text, seg.wpm);
    }
    // Restore authoritative WPM if transient changes were made
    if (cmdWpm != m_speed) {
        emit commandReady(QString("cwx wpm %1").arg(m_speed));
        ++m_pendingWpmEchoes;   // swallow the restore's echo too (#3976)
    }
}

void CwxModel::send(const QString& text)
{
    if (text.isEmpty()) return;
    emitExpandedSend(expandSpeedModifiers(text, m_speed, m_speedStep));
}

void CwxModel::sendChar(const QString& ch)
{
    if (ch.isEmpty()) return;
    QString encoded = ch;
    encoded.replace(' ', QChar(0x7f));
    emit commandReady(QString("cwx send \"%1\" %2").arg(encoded).arg(m_nextBlock++));
    emit transmissionRequested(ch, m_speed);
}

void CwxModel::sendMacro(int idx)
{
    if (idx < 1 || idx > 12) return;
    const QString text = m_macros[idx - 1];
    if (text.isEmpty()) return;
    // Expand speed modifiers client-side via cwx send blocks rather than
    // cwx macro send N.  When no modifiers are present the result is a
    // single cwx send identical in effect to the radio-side expansion, but
    // the unified path ensures + / - prefixes are never forwarded to the
    // radio where they would be misread as prosigns (AR / hyphen).
    emitExpandedSend(expandSpeedModifiers(text, m_speed, m_speedStep));
}

void CwxModel::saveMacro(int idx, const QString& text)
{
    if (idx < 0 || idx >= 12) return;
    m_macros[idx] = text;
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    emit commandReady(QString("cwx macro save %1 \"%2\"").arg(idx + 1).arg(encoded));
}

void CwxModel::erase(int numChars)
{
    emit commandReady(QString("cwx erase %1").arg(numChars));
    emit transmissionCancelled();
}

void CwxModel::clearBuffer()
{
    // Re-anchor WPM before clearing so ESC can't leave the radio parked at
    // a transient speed that was in-flight from an expandedSend sequence.
    m_pendingWpmEchoes = 0;   // abort — abandon any pending transient suppression (#3976)
    emit commandReady(QString("cwx wpm %1").arg(m_speed));
    emit commandReady("cwx clear");
    emit transmissionCancelled();
}

void CwxModel::setSpeed(int wpm)
{
    m_pendingWpmEchoes = 0;   // user set the base — abandon transient suppression (#3976)
    wpm = qBound(5, wpm, 100);
    if (wpm != m_speed) {
        m_speed = wpm;
        emit commandReady(QString("cwx wpm %1").arg(m_speed));
        emit speedChanged(m_speed);
    }
}

void CwxModel::setSpeedStep(int step)
{
    step = qBound(1, step, 20);
    if (step != m_speedStep) {
        m_speedStep = step;
        emit speedStepChanged(m_speedStep);
    }
}

void CwxModel::setDelay(int ms)
{
    ms = qBound(0, ms, 2000);
    if (ms != m_delay) {
        m_delay = ms;
        emit commandReady(QString("cwx delay %1").arg(m_delay));
        emit delayChanged(m_delay);
    }
}

void CwxModel::setQsk(bool on)
{
    if (on != m_qsk) {
        m_qsk = on;
        emit commandReady(QString("cwx qsk_enabled %1").arg(m_qsk ? 1 : 0));
        emit qskChanged(m_qsk);
    }
}

void CwxModel::setLive(bool on)
{
    if (on != m_live) {
        m_live = on;
        emit liveChanged(m_live);
    }
}

void CwxModel::applyStatus(const QMap<QString, QString>& kvs)
{
    for (auto it = kvs.cbegin(); it != kvs.cend(); ++it) {
        const QString& key = it.key();
        const QString& val = it.value();

        if (key == "sent") {
            bool ok;
            int idx = val.toInt(&ok);
            if (ok) {
                m_sentIndex = idx;
                emit charSent(idx);
            }
        } else if (key == "wpm") {
            bool ok;
            int v = val.toInt(&ok);
            if (ok) {
                if (m_pendingWpmEchoes > 0) {
                    // Echo of a transient `cwx wpm` we emitted during an
                    // expanded send — consume it, don't touch the base. (#3976)
                    --m_pendingWpmEchoes;
                } else if (v != m_speed) {
                    m_speed = v;
                    emit speedChanged(m_speed);
                }
            }
        } else if (key == "break_in_delay") {
            bool ok;
            int v = val.toInt(&ok);
            if (ok && v != m_delay) {
                m_delay = v;
                emit delayChanged(m_delay);
            }
        } else if (key == "qsk_enabled") {
            bool on = (val == "1");
            if (on != m_qsk) {
                m_qsk = on;
                emit qskChanged(m_qsk);
            }
        } else if (key == "erase") {
            QStringList parts = val.split(',');
            if (parts.size() == 2) {
                bool ok1, ok2;
                int start = parts[0].toInt(&ok1);
                int stop  = parts[1].toInt(&ok2);
                if (ok1 && ok2) emit erased(start, stop);
            }
        } else if (key == "queue") {
            // Empty queue= means the radio's CWX buffer has drained.
            // Signal RadioModel to release MOX (required when sync_cwx=1).
            if (val.isEmpty() || val == "0")
                emit queueEmpty();
        } else if (key.startsWith("macro") && key.length() > 5) {
            bool ok;
            int idx = key.mid(5).toInt(&ok);
            if (ok && idx >= 1 && idx <= 12) {
                // Decode: strip quotes, \u007f → space, * → =
                QString decoded = val;
                if (decoded.startsWith('"') && decoded.endsWith('"'))
                    decoded = decoded.mid(1, decoded.length() - 2);
                decoded.replace(QChar(0x7f), ' ');
                decoded.replace('*', '=');
                m_macros[idx - 1] = decoded;
                emit macroChanged(idx - 1, decoded);
            }
        }
    }
}

} // namespace AetherSDR
