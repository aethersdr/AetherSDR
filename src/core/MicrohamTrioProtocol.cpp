#include "core/MicrohamTrioProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#include <algorithm>

namespace AetherSDR {
namespace MicrohamTrio {

namespace {

ParseResult fail(ParseError error, const QString& where)
{
    ParseResult out;
    out.error = error;
    out.where = where;
    return out;
}

// Exactly the keys in `expected`, no more and no fewer. Fills `error` and
// `where` and returns false otherwise.
bool hasExactlyKeys(const QJsonObject& object, const QStringList& expected,
                    const QString& path, ParseResult& failure)
{
    for (const QString& key : expected) {
        if (!object.contains(key)) {
            failure = fail(ParseError::MissingKey, path + QLatin1Char('.') + key);
            return false;
        }
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!expected.contains(it.key())) {
            failure = fail(ParseError::UnexpectedKey, path + QLatin1Char('.') + it.key());
            return false;
        }
    }
    return true;
}

bool parseButtons(const QJsonValue& value, const QString& path,
                  QVector<Button>& buttons, ParseResult& failure)
{
    if (!value.isObject()) {
        failure = fail(ParseError::WrongType, path);
        return false;
    }
    const QJsonObject object = value.toObject();
    if (object.isEmpty()) {
        failure = fail(ParseError::NoButtons, path);
        return false;
    }
    if (object.size() > kMaxButtonsPerGroup) {
        failure = fail(ParseError::TooManyButtons, path);
        return false;
    }
    static const QStringList kButtonKeys = {
        QStringLiteral("n"), QStringLiteral("a"), QStringLiteral("l"), QStringLiteral("e")};
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString buttonPath = path + QLatin1Char('.') + it.key();
        if (!it.value().isObject()) {
            failure = fail(ParseError::WrongType, buttonPath);
            return false;
        }
        const QJsonObject fields = it.value().toObject();
        if (!hasExactlyKeys(fields, kButtonKeys, buttonPath, failure)) {
            return false;
        }
        for (const QString& textKey : {QStringLiteral("n"), QStringLiteral("a"),
                                       QStringLiteral("l")}) {
            if (!fields.value(textKey).isString()) {
                failure = fail(ParseError::WrongType, buttonPath + QLatin1Char('.') + textKey);
                return false;
            }
        }
        if (!fields.value(QStringLiteral("e")).isBool()) {
            failure = fail(ParseError::WrongType, buttonPath + QStringLiteral(".e"));
            return false;
        }
        Button button;
        button.key          = it.key();
        button.name         = fields.value(QStringLiteral("n")).toString();
        button.abbreviation = fields.value(QStringLiteral("a")).toString();
        button.label        = fields.value(QStringLiteral("l")).toString();
        button.enabled      = fields.value(QStringLiteral("e")).toBool();
        buttons.append(button);
    }
    return true;
}

bool groupObject(const QJsonObject& groups, const QString& name,
                 QJsonObject& group, ParseResult& failure,
                 const QStringList& keys = {QStringLiteral("v"), QStringLiteral("b")})
{
    const QString path = QStringLiteral("s.") + name;
    if (!groups.value(name).isObject()) {
        failure = fail(ParseError::WrongType, path);
        return false;
    }
    group = groups.value(name).toObject();
    return hasExactlyKeys(group, keys, path, failure);
}

bool parseSelector(const QJsonObject& groups, const QString& name,
                   Selector& selector, ParseResult& failure);

// One split half: `prefix` + a button key, as in "tx_ant6".
bool parseHalf(const QJsonObject& group, const QString& field, const QString& prefix,
               const Selector& antenna, QString& key, ParseResult& failure)
{
    const QString path = QStringLiteral("s.antenna.") + field;
    if (!group.value(field).isString()) {
        failure = fail(ParseError::WrongType, path);
        return false;
    }
    const QString value = group.value(field).toString();
    key = value.startsWith(prefix) ? value.mid(prefix.size()) : QString();
    if (key.isEmpty() || antenna.find(key) == nullptr) {
        failure = fail(ParseError::SelectionNotAButton, path);
        return false;
    }
    return true;
}

// The antenna group's keys follow split: `v` without it, `txv` and `rxv` with it.
bool parseAntenna(const QJsonObject& groups, bool split, Snapshot& snapshot,
                  ParseResult& failure)
{
    if (!split) {
        if (!parseSelector(groups, QStringLiteral("antenna"), snapshot.antenna, failure)) {
            return false;
        }
        snapshot.antennaTx = snapshot.antenna.selected;
        snapshot.antennaRx = snapshot.antenna.selected;
        return true;
    }
    QJsonObject group;
    if (!groupObject(groups, QStringLiteral("antenna"), group, failure,
                     {QStringLiteral("txv"), QStringLiteral("rxv"), QStringLiteral("b")})) {
        return false;
    }
    return parseButtons(group.value(QStringLiteral("b")), QStringLiteral("s.antenna.b"),
                        snapshot.antenna.buttons, failure)
           && parseHalf(group, QStringLiteral("txv"), QStringLiteral("tx_"), snapshot.antenna,
                        snapshot.antennaTx, failure)
           && parseHalf(group, QStringLiteral("rxv"), QStringLiteral("rx_"), snapshot.antenna,
                        snapshot.antennaRx, failure);
}

bool parseSelector(const QJsonObject& groups, const QString& name,
                   Selector& selector, ParseResult& failure)
{
    const QString path = QStringLiteral("s.") + name;
    QJsonObject group;
    if (!groupObject(groups, name, group, failure)) {
        return false;
    }
    if (!group.value(QStringLiteral("v")).isString()) {
        failure = fail(ParseError::WrongType, path + QStringLiteral(".v"));
        return false;
    }
    selector.selected = group.value(QStringLiteral("v")).toString();
    if (!parseButtons(group.value(QStringLiteral("b")), path + QStringLiteral(".b"),
                      selector.buttons, failure)) {
        return false;
    }
    if (selector.selectedButton() == nullptr) {
        failure = fail(ParseError::SelectionNotAButton, path + QStringLiteral(".v"));
        return false;
    }
    return true;
}

bool parseToggle(const QJsonObject& groups, const QString& name,
                 Toggle& toggle, ParseResult& failure)
{
    const QString path = QStringLiteral("s.") + name;
    QJsonObject group;
    if (!groupObject(groups, name, group, failure)) {
        return false;
    }
    if (!group.value(QStringLiteral("v")).isBool()) {
        failure = fail(ParseError::WrongType, path + QStringLiteral(".v"));
        return false;
    }
    toggle.on = group.value(QStringLiteral("v")).toBool();
    return parseButtons(group.value(QStringLiteral("b")), path + QStringLiteral(".b"),
                        toggle.buttons, failure);
}

qint64 backoffMs(int consecutiveFailures, const Policy& policy)
{
    qint64 wait = std::max<qint64>(policy.failureBackoffMs, 0);
    for (int i = 1; i < consecutiveFailures && wait < policy.maxBackoffMs; ++i) {
        wait *= 2;
    }
    return std::min(wait, policy.maxBackoffMs);
}

SessionDecision act(SessionAction action, SessionReason reason, qint64 waitMs = 0)
{
    SessionDecision out;
    out.action = action;
    out.reason = reason;
    out.waitMs = waitMs;
    return out;
}

} // namespace
const Button* Selector::find(const QString& key) const
{
    for (const Button& button : buttons) {
        if (button.key == key) {
            return &button;
        }
    }
    return nullptr;
}

ParseResult parse(const QByteArray& frame)
{
    if (frame.isEmpty()) {
        return fail(ParseError::Empty, QString{});
    }
    if (frame.size() > kMaxFrameBytes) {
        return fail(ParseError::TooLarge, QString{});
    }

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(frame, &jsonError);
    if (jsonError.error != QJsonParseError::NoError) {
        return fail(ParseError::NotJson, jsonError.errorString());
    }
    if (!document.isObject()) {
        return fail(ParseError::NotAnObject, QString{});
    }

    ParseResult failure;
    const QJsonObject root = document.object();
    static const QStringList kRootKeys = {QStringLiteral("s"), QStringLiteral("id")};
    if (!hasExactlyKeys(root, kRootKeys, QStringLiteral("$"), failure)) {
        return failure;
    }
    if (!root.value(QStringLiteral("id")).isString()) {
        return fail(ParseError::WrongType, QStringLiteral("$.id"));
    }
    if (!root.value(QStringLiteral("s")).isObject()) {
        return fail(ParseError::WrongType, QStringLiteral("$.s"));
    }

    const QJsonObject groups = root.value(QStringLiteral("s")).toObject();
    static const QStringList kGroups = {
        QStringLiteral("radio"), QStringLiteral("split"), QStringLiteral("pa"),
        QStringLiteral("antenna"), QStringLiteral("rxant"), QStringLiteral("rxext")};
    if (!hasExactlyKeys(groups, kGroups, QStringLiteral("s"), failure)) {
        return failure;
    }

    Snapshot snapshot;
    snapshot.id = root.value(QStringLiteral("id")).toString();
    if (!parseSelector(groups, QStringLiteral("radio"), snapshot.radio, failure)
        || !parseToggle(groups, QStringLiteral("split"), snapshot.split, failure)
        || !parseToggle(groups, QStringLiteral("pa"), snapshot.pa, failure)
        || !parseAntenna(groups, snapshot.split.on, snapshot, failure)
        || !parseSelector(groups, QStringLiteral("rxant"), snapshot.rxAntenna, failure)
        || !parseToggle(groups, QStringLiteral("rxext"), snapshot.rxExt, failure)) {
        return failure;
    }

    ParseResult out;
    out.snapshot = snapshot;
    return out;
}

QByteArray encodeKeepAlive()
{
    return QByteArrayLiteral("ping");
}

Frame classify(quint8 firstByte, const QByteArray& payload)
{
    Frame out;
    if (firstByte == kPongFrame) {
        if (payload.isEmpty()) {
            out.kind = FrameKind::ServerPong;
        }
        return out;
    }
    if (firstByte != kTextFrame) {
        return out;
    }
    if (payload == QByteArrayLiteral("pong")) {
        out.kind = FrameKind::KeepAliveReply;
        return out;
    }
    out.state = parse(payload);
    if (out.state.snapshot) {
        out.kind = FrameKind::State;
    }
    return out;
}

bool sessionDead(const State& state, const Policy& policy)
{
    if (state.link != Link::Held) {
        return false;
    }
    if (state.lastMessageAgeMs < 0 || state.lastMessageAgeMs > policy.sessionDeadAfterMs) {
        return true;
    }
    return state.unansweredKeepAliveAgeMs
           && (*state.unansweredKeepAliveAgeMs < 0
               || *state.unansweredKeepAliveAgeMs > policy.sessionDeadAfterMs);
}

SessionDecision decideSession(const State& state, const Policy& policy)
{
    if (state.releasedForBrowser) {
        return act(state.link == Link::Held ? SessionAction::Disconnect : SessionAction::Wait,
                   SessionReason::Released);
    }

    switch (state.link) {
    case Link::NeverConnected:
        return act(SessionAction::Connect, SessionReason::FirstConnect);

    case Link::Evicted:
        // Another client holds the session now, most often the operator's
        // browser on the Trio's own page. Connecting again would evict it in
        // turn; only the operator's Connect takes the session back.
        return act(SessionAction::Wait, SessionReason::Evicted);

    case Link::Failed: {
        const qint64 pause = backoffMs(state.consecutiveFailures, policy);
        if (state.lastConnectAgeMs && *state.lastConnectAgeMs >= pause) {
            return act(SessionAction::Connect, SessionReason::RetryAfterFailure);
        }
        const qint64 age = state.lastConnectAgeMs ? std::max<qint64>(*state.lastConnectAgeMs, 0) : 0;
        return act(SessionAction::Wait, SessionReason::BackoffAfterFailure, pause - age);
    }

    case Link::Held:
        break;
    }

    if (sessionDead(state, policy)) {
        return act(SessionAction::Connect, SessionReason::SessionDead);
    }
    if (!state.hasSnapshot) {
        return act(SessionAction::Wait, SessionReason::AwaitingState);
    }
    if (!state.lastKeepAliveAgeMs || *state.lastKeepAliveAgeMs < 0
        || *state.lastKeepAliveAgeMs >= policy.keepAliveIntervalMs) {
        return act(SessionAction::SendKeepAlive, SessionReason::KeepAliveDue);
    }
    return act(SessionAction::Wait, SessionReason::KeepAliveNotDue,
               policy.keepAliveIntervalMs - *state.lastKeepAliveAgeMs);
}

} // namespace MicrohamTrio
} // namespace AetherSDR
