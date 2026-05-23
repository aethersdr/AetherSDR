#include "CwxModel.h"
#include <QDebug>
#include <QMap>

namespace MasterSDR {

CwxModel::CwxModel(QObject* parent)
    : QObject(parent)
{}

QString CwxModel::macro(int idx) const
{
    if (idx < 0 || idx >= 12) return {};
    return m_macros[idx];
}

void CwxModel::send(const QString& text)
{
    if (text.isEmpty()) return;
    // Encode spaces as DEL (0x7f) per FlexLib protocol
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    emit commandReady(QString("cwx send \"%1\" %2").arg(encoded).arg(m_nextBlock++));
    emit transmissionRequested(text, m_speed);
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
    emit commandReady(QString("cwx macro send %1").arg(idx));
    // Macro text lives in m_macros (0-based); fire local keyer with it
    // so sidetone matches the radio's expansion.
    const QString text = m_macros[idx - 1];
    if (!text.isEmpty())
        emit transmissionRequested(text, m_speed);
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
    emit commandReady("cwx clear");
    emit transmissionCancelled();
}

void CwxModel::setSpeed(int wpm)
{
    wpm = qBound(5, wpm, 100);
    if (wpm != m_speed) {
        m_speed = wpm;
        emit commandReady(QString("cwx wpm %1").arg(m_speed));
        emit speedChanged(m_speed);
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
            if (ok && v != m_speed) {
                m_speed = v;
                emit speedChanged(m_speed);
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

} // namespace MasterSDR
