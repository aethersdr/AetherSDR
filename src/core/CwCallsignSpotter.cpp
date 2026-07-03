#include "CwCallsignSpotter.h"

#include "CallsignUtils.h"
#include "LogManager.h"

#include <QRegularExpression>

namespace AetherSDR {

namespace {
// "DE <call> <call>" with the two calls identical.  The callsign shape
// comes from Callsigns::regex(); \b keeps "MADE K1ABC..." from matching.
const QRegularExpression& deRegex()
{
    static const QRegularExpression re(
        QStringLiteral("\\bDE\\s+(%1)\\s+\\1\\b").arg(Callsigns::regex().pattern()));
    return re;
}
} // namespace

CwCallsignSpotter::CwCallsignSpotter(QObject* parent)
    : QObject(parent)
{
    // An ID is often the last thing a station sends ("... 73 DE K1AB K1AB"
    // then silence).  A match that touches the end of the window can't be
    // accepted immediately — the second call might still be streaming in —
    // so this timer re-scans once the stream has settled.
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(kSettleMs);
    connect(&m_settleTimer, &QTimer::timeout, this, [this] {
        scanWindow(/*streamSettled=*/true);
    });
}

void CwCallsignSpotter::clear()
{
    m_window.clear();
    m_settleTimer.stop();
}

void CwCallsignSpotter::feedText(const QString& text)
{
    if (text.isEmpty())
        return;

    // Collapse whitespace runs so multi-chunk arrival ("DE KI6", "BCJ ")
    // regexes the same as one string.
    m_window.append(text.toUpper());
    m_window.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (m_window.size() > kWindowChars)
        m_window.remove(0, m_window.size() - kWindowChars);

    scanWindow(/*streamSettled=*/false);
}

void CwCallsignSpotter::scanWindow(bool streamSettled)
{
    const QRegularExpressionMatch m = deRegex().match(m_window);
    if (!m.hasMatch()) {
        m_settleTimer.stop();
        return;
    }
    // A match that runs to the very end of the window may be incomplete
    // ("...DE K1AB K1AB" now, "C" in the next chunk).  Hold it until either
    // a following character arrives or the settle timer declares silence.
    if (m.capturedEnd() >= m_window.size() && !streamSettled) {
        m_settleTimer.start();
        return;
    }
    m_settleTimer.stop();

    const QString call = m.captured(1);
    // Consume through the match so the same "DE X X" run can't re-fire as
    // more text streams in behind it.
    m_window.remove(0, m.capturedEnd());
    emitSpot(call);

    // A long chunk could contain a second full ID; keep scanning.
    if (deRegex().match(m_window).hasMatch())
        scanWindow(streamSettled);
}

void CwCallsignSpotter::emitSpot(const QString& call)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 last = m_lastSpotUtc.value(call, 0);
    if (now - last < kReSpotSecs)
        return;
    m_lastSpotUtc.insert(call, now);

    qCDebug(lcQrz) << "CW spotted callsign" << call;
    emit callsignSpotted(call);
}

} // namespace AetherSDR
