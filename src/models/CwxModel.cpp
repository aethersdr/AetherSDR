#include "CwxModel.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QMap>
#include <QScopeGuard>
#include <QPointer>
#include <limits>

namespace AetherSDR {

namespace {
// CWX keyer speed is clamped to the FlexLib-supported 5–100 wpm range at every
// entry point (user setSpeed and +/- modifier expansion) — one definition so
// the two clamp sites can't drift. (#3949 review)
inline int clampWpm(int wpm) { return qBound(5, wpm, 100); }
}

CwxModel::CwxModel(QObject* parent)
    : QObject(parent)
{}

CwxModel::~CwxModel()
{
    m_queueValid->store(false, std::memory_order_release);
}

CwxModel::TransmissionPermit CwxModel::queuedTransmissionPermit() const
{
    const std::shared_ptr<std::atomic<bool>> valid = m_queueValid;
    return [valid] { return valid->load(std::memory_order_acquire); };
}

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
        // Whitespace (space/tab/newline) is a word boundary and keys as a word
        // gap. Macros come from multi-line QTextEdit widgets, so a raw '\n'/'\t'
        // must not be treated as a word char — it would bleed a modifier across
        // the line and, un-DEL-encoded, corrupt the cwx send command. Normalize
        // any whitespace run to a single space. (#272)
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
        while (j < text.size() && !text[j].isSpace()) ++j;
        const QString wordBody = text.mid(wordStart, j - wordStart);

        const bool hasModifier = (plus > 0 || minus > 0);
        const int  delta   = (plus - minus) * step;
        const int  wordWpm = hasModifier
                           ? clampWpm(baseWpm + delta)
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

CwxModel::TransmissionPermit CwxModel::admitTransmission(const TransmissionRoute& route)
{
    const QPointer<CwxModel> self(this);
    if (m_clearing) {
        return {};
    }
    const SendAvailability available = m_sendAvailability;
    if (available && !available()) {
        qCWarning(lcCw) << "CWX send refused: TUNE is active (#5422)";
        return {};
    }
    if (!self) { return {}; }
    const int epoch = m_drainEpoch;
    const TransmissionAdmission admission = route.admit ? route.admit : m_transmissionAdmission;
    const TransmissionPermit permit = admission ? admission() : TransmissionPermit{};
    if (!self) { return {}; }
    const bool permitted = !admission || (permit && permit());
    if (!self || !permitted) {
        return {};
    }
    return [self, permit, epoch] {
        return self && epoch == self->m_drainEpoch && (!permit || permit()) && self;
    };
}

void CwxModel::dispatchCommand(const QString& command, int epoch, int nChars,
                               const TransmissionRoute& route)
{
    if (route.command) {
        route.command(command, epoch, nChars);
    } else if (nChars >= 0) {
        emit replyCommandReady(command, epoch, nChars);
    } else {
        emit commandReady(command);
    }
}

void CwxModel::emitExpandedSend(const QVector<SpeedSegment>& segs, const TransmissionPermit& permit,
                                const TransmissionRoute& route)
{
    // Find last segment with non-empty text — that block's cwx send goes via
    // replyCommandReady so RadioModel can capture the radio_index and detect
    // queue drain via sent= status instead of the broken queue= path. (#3949)
    int lastNonEmpty = -1;
    for (int i = segs.size() - 1; i >= 0; --i) {
        if (!segs[i].text.isEmpty()) { lastNonEmpty = i; break; }
    }

    int cmdWpm = m_speed;
    const int epoch = m_drainEpoch;
    const QPointer<CwxModel> self(this);
    const auto restoreSpeed = qScopeGuard([self, &cmdWpm, epoch, &route] {
        // This is cleanup, not a fresh keying intent. Every early return must
        // restore the base speed too. An epoch change means clearBuffer already
        // restored it, or the original session has gone away; don't write into
        // a replacement batch/session from this old stack frame.
        if (self && epoch == self->m_drainEpoch && cmdWpm != self->m_speed) {
            ++self->m_pendingWpmEchoes;
            self->dispatchCommand(QString("cwx wpm %1").arg(self->m_speed), epoch, -1, route);
        }
    });
    for (int i = 0; i < segs.size(); ++i) {
        if (!permit()) {
            return;
        }
        const SpeedSegment& seg = segs[i];
        if (seg.wpm != cmdWpm) {
            ++m_pendingWpmEchoes;   // swallow this transient's echo (#272)
            cmdWpm = seg.wpm;
            dispatchCommand(QString("cwx wpm %1").arg(seg.wpm), epoch, -1, route);
        }
        if (!permit()) {
            return;
        }
        QString encoded = seg.text;
        encoded.replace(' ', QChar(0x7f));
        if (!encoded.isEmpty()) {
            const QString cmd =
                QString("cwx send \"%1\" %2").arg(encoded).arg(m_nextBlock++);
            if (i == lastNonEmpty) {
                // Segments queue contiguously, so the last segment's start
                // (radio_index) + its length - 1 is the last char of the whole
                // message — the correct batch-end index to watch. (#3949)
                dispatchCommand(cmd, m_drainEpoch, seg.text.length(), route);
            } else {
                dispatchCommand(cmd, m_drainEpoch, -1, route);
            }
        }
        if (!permit()) {
            return;
        }
        if (!seg.text.isEmpty() && !notifyTransmission(seg.text, seg.wpm, permit, route)) {
            return;
        }
    }
}

bool CwxModel::notifyTransmission(const QString& text, int wpm, const TransmissionPermit& permit,
                                  const TransmissionRoute& route)
{
    if (!permit()) {
        return false;
    }
    const TextSender sender = route.text ? route.text : m_textSender;
    if (sender && !sender(text, wpm)) {
        if (permit()) {
            clearBuffer();
        }
        return false;
    }
    if (!permit()) {
        return false;
    }
    emit transmissionRequested(text, wpm);
    return permit();
}

void CwxModel::send(const QString& text)
{
    send(text, {});
}

void CwxModel::send(const QString& text, const TransmissionRoute& route)
{
    if (text.isEmpty()) {
        return;
    }
    const TransmissionPermit permit = admitTransmission(route);
    if (!permit) {
        return;
    }
    if (!m_speedModifiersEnabled) {
        notifyTransmission(text, m_speed, permit, route);
    } else {
        emitExpandedSend(expandSpeedModifiers(text, m_speed, m_speedStep), permit, route);
    }
    if (permit()) {
        if (route.dispatched) {
            route.dispatched(m_drainEpoch, false);
        } else {
            emit transmissionDispatched(m_drainEpoch, false);
        }
    }
}

void CwxModel::sendChar(const QString& ch)
{
    sendChar(ch, {});
}

void CwxModel::sendChar(const QString& ch, const TransmissionRoute& route)
{
    if (ch.isEmpty()) return;
    const TransmissionPermit permit = admitTransmission(route);
    if (!permit) {
        return;
    }
    QString encoded = ch;
    encoded.replace(' ', QChar(0x7f));
    // Live-mode chars go via the reply path (not fire-and-forget commandReady)
    // so each char's radio_index advances the drain watch. Without this a
    // live-typing-only session arms no watch and the ~60 s stuck-TX survives,
    // and chars typed after a macro would be truncated by a stale watch. (#3949)
    const QString command = QString("cwx send \"%1\" %2").arg(encoded).arg(m_nextBlock++);
    if (route.command) { route.command(command, m_drainEpoch, ch.length()); }
    else { emit replyCommandReady(command, m_drainEpoch, ch.length()); }
    if (permit() && notifyTransmission(ch, m_speed, permit, route)) {
        if (route.dispatched) { route.dispatched(m_drainEpoch, false); }
        else { emit transmissionDispatched(m_drainEpoch, false); }
    }
}

void CwxModel::sendMacro(int idx)
{
    sendMacro(idx, {});
}

void CwxModel::sendMacro(int idx, const TransmissionRoute& route)
{
    if (idx < 1 || idx > 12) return;
    const TransmissionPermit permit = admitTransmission(route);
    if (!permit) {
        return;
    }
    const QString text = m_macros[idx - 1];
    if (text.isEmpty()) {
        // Local copy not yet synced (the macroN= status is still pending in the
        // first seconds after connect) — fall back to radio-side expansion so an
        // early F-key press still transmits instead of silently keying nothing.
        // The radio is authoritative for stored macros (Principle II).
        // No client-side character count exists, so this can only complete
        // local dispatch, never claim that the radio has finished transmitting.
        const QString command = QString("cwx macro send %1").arg(idx);
        if (route.command) { route.command(command, m_drainEpoch, -1); }
        else { emit commandReady(command); }
        if (permit()) {
            if (route.dispatched) { route.dispatched(m_drainEpoch, true); }
            else { emit transmissionDispatched(m_drainEpoch, true); }
        }
        return;
    }
    // Expand speed modifiers client-side via cwx send blocks rather than
    // cwx macro send N.  When no modifiers are present the result is a
    // single cwx send identical in effect to the radio-side expansion, but
    // the unified path ensures + / - prefixes are never forwarded to the
    // radio where they would be misread as prosigns (AR / hyphen).
    emitExpandedSend(expandSpeedModifiers(text, m_speed, m_speedStep), permit, route);
    if (permit()) {
        if (route.dispatched) { route.dispatched(m_drainEpoch, false); }
        else { emit transmissionDispatched(m_drainEpoch, false); }
    }
}

void CwxModel::saveMacro(int idx, const QString& text)
{
    if (idx < 0 || idx >= 12) return;
    m_macros[idx] = text;   // keep the raw +/- text for our own client-side expansion
    // Strip the client-only +/- speed-modifier prefixes before writing to the
    // radio's shared macro store. The +/- syntax is an AetherSDR extension; a
    // Multi-Flex peer (SmartSDR/Maestro) that plays this stored slot would key a
    // leading '+' as the AR prosign (CWX.cs HandleSpecialStrings maps " + " ↔
    // " AR "). Store the plain keyed text so shared slots stay portable.
    // (Principle VII: sanitise at the boundary where the bytes leave.) The strip
    // reuses expandSpeedModifiers — its segment text has the modifier prefixes
    // already removed; joining the segments reconstructs the plain message.
    QString plain;
    for (const auto& seg : expandSpeedModifiers(text, m_speed, m_speedStep))
        plain += seg.text;
    QString encoded = plain;
    encoded.replace(' ', QChar(0x7f));
    emit commandReady(QString("cwx macro save %1 \"%2\"").arg(idx + 1).arg(encoded));
}

void CwxModel::erase(int numChars)
{
    // KNOWN GAP (needs hardware / #3949 item 4): erase removes queued chars, so
    // an armed drain watch at m_cwxEndIndex may point past the new tail — sent=
    // could then never reach it and TX would stick for the full interlock
    // timeout. The correct adjustment depends on the radio's post-erase sent=
    // semantics, which aren't yet confirmed on the 8600. Left unchanged (bare
    // commandReady) pending that observation rather than guessing an offset.
    emit commandReady(QString("cwx erase %1").arg(numChars));
    emit transmissionCancelled();
}

void CwxModel::clearBuffer()
{
    if (m_clearing) {
        return;
    }
    m_clearing = true;
    const QPointer<CwxModel> self(this);
    const auto clearing = qScopeGuard([self] { if (self) { self->m_clearing = false; } });
    resetDrainWatch();    // abort pending watch + bump epoch (#3949)
    m_pendingWpmEchoes = 0;   // abort — abandon any pending transient suppression (#272)
    // Re-anchor WPM before clearing so ESC can't leave the radio parked at
    // a transient speed that was in-flight from an expandedSend sequence.
    emit commandReady(QString("cwx wpm %1").arg(m_speed));
    if (!self) { return; }
    emit commandReady("cwx clear");
    if (!self) { return; }
    emit transmissionCancelled();
}

void CwxModel::resetDrainWatch()
{
    m_queueValid->store(false, std::memory_order_release);
    m_queueValid = std::make_shared<std::atomic<bool>>(true);
    abandonDrainWatch();
}

void CwxModel::abandonDrainWatch()
{
    // Bumping the epoch invalidates any in-flight cwx-send reply so it can't
    // re-arm the watch for a batch the radio has discarded. Clearing the end
    // index also releases the monotonic guard in handleSendReply(), so a fresh
    // session after reconnect can arm at a smaller radio_index. (#3949)
    ++m_drainEpoch;
    m_cwxEndIndex = -1;
}

void CwxModel::handleSendReply(int resultCode, const QString& body, int epoch, int nChars)
{
    // Stale-reply guard: a reply for a batch that was aborted (ESC/clear/
    // disconnect) after the command was sent carries the pre-abort epoch and
    // must not re-arm the watch. Without this, a late reply arriving after ESC
    // — easy over SmartLink latency — would arm at an index the radio already
    // discarded, causing a spurious mid-message xmit 0 later. (#3949)
    if (epoch != m_drainEpoch)
        return;
    if (resultCode != 0) {
        qCWarning(lcCw) << "CwxModel: cwx send failed, result=" << resultCode;
        clearBuffer();
        return;
    }
    // Body format is "<radio_index>,<block>" per FlexLib CWX.cs:54-83.
    const QString indexStr = body.split(',').first().trimmed();
    bool ok = false;
    const int radioIndex = indexStr.toInt(&ok);
    if (!ok || radioIndex < 0) {
        qCWarning(lcCw) << "CwxModel: failed to parse radio_index from reply body:" << body;
        clearBuffer();
        return;
    }
    // radio_index is the FIRST-char (insertion-start) queue position of this
    // batch, so the last char — the value cwx sent= must reach to release TX —
    // is radio_index + nChars - 1. Verified on FLEX-6500 fw 4.2.20.41343 (see
    // handleSendReply() doc in the header). nChars is always >= 1 for a real
    // send; guard defensively so an empty batch can't retract the index. (#3949)
    if (nChars < 1 || radioIndex > std::numeric_limits<int>::max() - (nChars - 1)) {
        qCWarning(lcCw) << "CwxModel: invalid character count in send reply";
        clearBuffer();
        return;
    }
    const int endIndex = radioIndex + nChars - 1;
    // Only ever advance the watch, never retract it. cwx send replies on the
    // single command socket are normally ordered, but if two back-to-back
    // macros' replies arrive out of order, a smaller end index must not
    // overwrite a larger one — that would release TX while the later batch
    // still has chars queued. m_cwxEndIndex is -1 while idle, so the first
    // reply of a batch always takes. (#3949)
    if (endIndex > m_cwxEndIndex) {
        m_cwxEndIndex = endIndex;
    }
}

void CwxModel::setSpeed(int wpm)
{
    m_pendingWpmEchoes = 0;   // user set the base — abandon transient suppression (#272)
    wpm = clampWpm(wpm);
    if (wpm != m_speed) {
        m_speed = wpm;
        emit commandReady(QString("cwx wpm %1").arg(m_speed));
        emit speedChanged(m_speed);
        emit speedCommandIssued(m_speed);
    }
}

void CwxModel::adoptSpeed(int wpm)
{
    wpm = clampWpm(wpm);
    if (wpm != m_speed) {
        m_speed = wpm;
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
                // Queue drain detection: radio_index captured from cwx send
                // reply tells us the last char position of the queued batch.
                // Fire queueEmpty() so RadioModel can release MOX. (#3949)
                if (m_cwxEndIndex >= 0 && idx >= m_cwxEndIndex) {
                    m_cwxEndIndex = -1;
                    emit queueEmpty();
                }
            }
        } else if (key == "wpm") {
            bool ok;
            int v = val.toInt(&ok);
            if (m_pendingWpmEchoes > 0) {
                // Echo of a transient `cwx wpm` we emitted for a per-word speed
                // modifier — swallow it so it can't clobber the authoritative
                // base speed or flicker the Speed spinbox. (#272)
                --m_pendingWpmEchoes;
            } else if (ok && v != m_speed) {
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
            // Legacy path: an empty queue= would mean the radio's CWX buffer has
            // drained. Firmware doesn't actually emit this (confirmed on FLEX-6500
            // fw 4.2.20.41343 — the reply-radio_index watch exists precisely
            // because queue= never arrives), so this is a belt-and-suspenders
            // fallback should a future firmware start sending it. If it does, the
            // reply-radio_index machinery can be retired in favour of this path —
            // tracked in #4028 (protocol/upstream). (#3949)
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
