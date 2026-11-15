#include "ControlPairing.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

namespace AetherSDR::control {

QString ControlPairing::checkDigits(const QString& six, const QString& certificateSha256)
{
    const QByteArray digest = QCryptographicHash::hash(
        (six + certificateSha256.toLower()).toLatin1(), QCryptographicHash::Sha256);
    const quint32 value = (quint32(quint8(digest[0])) << 24) | (quint32(quint8(digest[1])) << 16)
                        | (quint32(quint8(digest[2])) << 8) | quint32(quint8(digest[3]));
    return QStringLiteral("%1").arg(value % 10000u, 4, 10, QLatin1Char('0'));
}

std::optional<QString> ControlPairing::digitsOf(const QString& code)
{
    QString digits;
    for (const QChar c : code) {
        if (c.isDigit() && c.unicode() < 128) {
            digits.append(c);
        } else if (c != u'-' && c != u' ') {
            return std::nullopt;
        }
    }
    return digits.size() == 10 ? std::optional{digits} : std::nullopt;
}

QString ControlPairing::formatted(const QString& tenDigits)
{
    return tenDigits.left(3) + u'-' + tenDigits.mid(3, 3) + u'-' + tenDigits.mid(6);
}

ControlPairing::Opened ControlPairing::begin(const QString& certificateSha256, qint64 nowMs)
{
    if (randomSixForTest) {
        m_six = randomSixForTest();
    } else {
        m_six = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1000000u), 6, 10,
                                         QLatin1Char('0'));
    }
    m_fingerprint = certificateSha256.toLower();
    m_expiresAtMs = nowMs + kLifetimeMs;
    m_attemptsLeft = kMaxAttempts;
    m_open = true;
    return {formatted(m_six + checkDigits(m_six, m_fingerprint)), kLifetimeMs, m_fingerprint};
}

ControlPairing::Attempt ControlPairing::attempt(const QString& code, qint64 nowMs)
{
    if (!m_open) {
        return Attempt::NotOpen;
    }
    if (nowMs >= m_expiresAtMs) {
        m_open = false;
        return Attempt::Expired;
    }
    const std::optional<QString> digits = digitsOf(code);
    if (digits && *digits == m_six + checkDigits(m_six, m_fingerprint)) {
        m_open = false;   // single use
        return Attempt::Accepted;
    }
    if (--m_attemptsLeft <= 0) {
        m_open = false;
    }
    return Attempt::Wrong;
}

} // namespace AetherSDR::control
