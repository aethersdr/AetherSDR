#include "ControlProtocolCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace AetherSDR::control {
namespace {

ProtocolError error(QString code, QString message, QJsonObject data = {})
{
    return {std::move(code), std::move(message), std::move(data), false};
}

class JsonStructureScanner final {
public:
    explicit JsonStructureScanner(const QByteArray& bytes) : m_bytes(bytes) {}

    bool scan(QString* why)
    {
        skipWhitespace();
        if (!scanValue(1, why)) {
            return false;
        }
        skipWhitespace();
        if (m_pos != m_bytes.size()) {
            *why = QStringLiteral("trailing data after JSON value");
            return false;
        }
        return true;
    }

private:
    bool scanValue(int depth, QString* why)
    {
        if (depth > ProtocolLimits::kMaxNesting) {
            *why = QStringLiteral("JSON nesting exceeds the protocol limit");
            return false;
        }
        skipWhitespace();
        if (m_pos >= m_bytes.size()) {
            *why = QStringLiteral("unexpected end of JSON");
            return false;
        }

        switch (m_bytes.at(m_pos)) {
        case '{': return scanObject(depth, why);
        case '[': return scanArray(depth, why);
        case '"': return scanString(nullptr, why);
        case 't': return scanLiteral("true", why);
        case 'f': return scanLiteral("false", why);
        case 'n': return scanLiteral("null", why);
        default: return scanNumber(why);
        }
    }

    bool scanObject(int depth, QString* why)
    {
        ++m_pos;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        QSet<QString> keys;
        while (m_pos < m_bytes.size()) {
            QByteArray rawKey;
            if (!scanString(&rawKey, why)) {
                return false;
            }
            QJsonParseError keyError{};
            const QJsonDocument keyDoc = QJsonDocument::fromJson(
                QByteArrayLiteral("[") + rawKey + QByteArrayLiteral("]"), &keyError);
            if (keyError.error != QJsonParseError::NoError
                || !keyDoc.isArray() || keyDoc.array().isEmpty()
                || !keyDoc.array().first().isString()) {
                *why = QStringLiteral("invalid JSON object key");
                return false;
            }
            const QString key = keyDoc.array().first().toString();
            if (keys.contains(key)) {
                *why = QStringLiteral("duplicate JSON object key: %1").arg(key);
                return false;
            }
            keys.insert(key);

            skipWhitespace();
            if (!consume(':')) {
                *why = QStringLiteral("expected ':' after JSON object key");
                return false;
            }
            if (!scanValue(depth + 1, why)) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                *why = QStringLiteral("expected ',' or '}' in JSON object");
                return false;
            }
            skipWhitespace();
        }
        *why = QStringLiteral("unterminated JSON object");
        return false;
    }

    bool scanArray(int depth, QString* why)
    {
        ++m_pos;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }

        qsizetype count = 0;
        while (m_pos < m_bytes.size()) {
            ++count;
            if (count > ProtocolLimits::kMaxArrayEntries) {
                *why = QStringLiteral("JSON array exceeds the protocol limit");
                return false;
            }
            if (!scanValue(depth + 1, why)) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                *why = QStringLiteral("expected ',' or ']' in JSON array");
                return false;
            }
        }
        *why = QStringLiteral("unterminated JSON array");
        return false;
    }

    bool scanString(QByteArray* raw, QString* why)
    {
        if (!consume('"')) {
            *why = QStringLiteral("expected JSON string");
            return false;
        }
        const qsizetype start = m_pos - 1;
        bool escaped = false;
        while (m_pos < m_bytes.size()) {
            const char ch = m_bytes.at(m_pos++);
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                if (raw) {
                    *raw = m_bytes.mid(start, m_pos - start);
                }
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20U) {
                *why = QStringLiteral("control character in JSON string");
                return false;
            }
        }
        *why = QStringLiteral("unterminated JSON string");
        return false;
    }

    bool scanLiteral(const char* literal, QString* why)
    {
        const QByteArray expected(literal);
        if (m_bytes.mid(m_pos, expected.size()) != expected) {
            *why = QStringLiteral("invalid JSON token");
            return false;
        }
        m_pos += expected.size();
        return true;
    }

    bool scanNumber(QString* why)
    {
        const qsizetype start = m_pos;
        while (m_pos < m_bytes.size()) {
            const char ch = m_bytes.at(m_pos);
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+'
                || ch == '.' || ch == 'e' || ch == 'E') {
                ++m_pos;
                continue;
            }
            break;
        }
        if (m_pos == start) {
            *why = QStringLiteral("invalid JSON token");
            return false;
        }
        return true;
    }

    void skipWhitespace()
    {
        while (m_pos < m_bytes.size()) {
            const char ch = m_bytes.at(m_pos);
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                return;
            }
            ++m_pos;
        }
    }

    bool consume(char expected)
    {
        if (m_pos >= m_bytes.size() || m_bytes.at(m_pos) != expected) {
            return false;
        }
        ++m_pos;
        return true;
    }

    const QByteArray& m_bytes;
    qsizetype m_pos{0};
};

bool validateValueLimits(const QJsonValue& value, QString* why)
{
    if (value.isString()) {
        if (value.toString().size() > ProtocolLimits::kMaxStringChars) {
            *why = QStringLiteral("JSON string exceeds the protocol limit");
            return false;
        }
        return true;
    }
    if (value.isDouble()) {
        if (!std::isfinite(value.toDouble())) {
            *why = QStringLiteral("non-finite JSON number");
            return false;
        }
        return true;
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() > ProtocolLimits::kMaxArrayEntries) {
            *why = QStringLiteral("JSON array exceeds the protocol limit");
            return false;
        }
        for (const QJsonValue& child : array) {
            if (!validateValueLimits(child, why)) {
                return false;
            }
        }
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key().size() > ProtocolLimits::kMaxStringChars
                || !validateValueLimits(it.value(), why)) {
                if (why->isEmpty()) {
                    *why = QStringLiteral("JSON object key exceeds the protocol limit");
                }
                return false;
            }
        }
    }
    return true;
}

bool validRequestId(const QString& id)
{
    if (id.isEmpty() || id.size() > ProtocolLimits::kMaxRequestIdChars) {
        return false;
    }
    for (const QChar ch : id) {
        if (ch.unicode() < 0x21U || ch.unicode() > 0x7eU) {
            return false;
        }
    }
    return true;
}

bool validMethod(const QString& method)
{
    if (method.isEmpty() || method.size() > ProtocolLimits::kMaxMethodChars
        || method.startsWith('.') || method.endsWith('.')) {
        return false;
    }
    bool segmentStart = true;
    for (const QChar ch : method) {
        if (ch == '.') {
            if (segmentStart) {
                return false;
            }
            segmentStart = true;
            continue;
        }
        if (segmentStart) {
            if (!ch.isLetter()) {
                return false;
            }
            segmentStart = false;
        } else if (!ch.isLetterOrNumber()) {
            return false;
        }
    }
    return !segmentStart;
}

} // namespace

ParseResult ControlProtocolCodec::parseRequest(const QByteArray& bytes)
{
    ParseResult out;
    if (bytes.size() > ProtocolLimits::kMaxMessageBytes) {
        out.error = error(QStringLiteral("transport.limit_exceeded"),
                          QStringLiteral("message exceeds maxMessageBytes"));
        return out;
    }

    QString structureError;
    JsonStructureScanner scanner(bytes);
    if (!scanner.scan(&structureError)) {
        out.error = error(QStringLiteral("protocol.invalid_json"), structureError);
        return out;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        out.error = error(QStringLiteral("protocol.invalid_json"),
                          parseError.errorString());
        return out;
    }

    const QJsonObject object = document.object();
    const QJsonValue rawIdValue = object.value(QStringLiteral("id"));
    if (rawIdValue.isString() && validRequestId(rawIdValue.toString())) {
        out.requestId = rawIdValue.toString();
    }

    QString limitError;
    if (!validateValueLimits(object, &limitError)) {
        out.error = error(QStringLiteral("transport.limit_exceeded"), limitError);
        return out;
    }

    const QSet<QString> commonKeys = {
        QStringLiteral("v"), QStringLiteral("id"), QStringLiteral("method"),
        QStringLiteral("params"), QStringLiteral("sessionId")};
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!commonKeys.contains(it.key())) {
            out.error = error(QStringLiteral("protocol.invalid_envelope"),
                              QStringLiteral("unknown envelope field"),
                              {{QStringLiteral("field"), it.key()}});
            return out;
        }
    }

    const QJsonValue versionValue = object.value(QStringLiteral("v"));
    const QJsonValue idValue = object.value(QStringLiteral("id"));
    const QJsonValue methodValue = object.value(QStringLiteral("method"));
    const QJsonValue paramsValue = object.value(QStringLiteral("params"));
    if (!versionValue.isDouble() || versionValue.toDouble() != 1.0
        || !idValue.isString() || !methodValue.isString()
        || !paramsValue.isObject()) {
        out.error = error(QStringLiteral("protocol.invalid_envelope"),
                          QStringLiteral("v, id, method, and params have invalid types"));
        return out;
    }

    ProtocolRequest request;
    request.version = 1;
    request.id = idValue.toString();
    request.method = methodValue.toString();
    request.params = paramsValue.toObject();
    if (!validRequestId(request.id) || !validMethod(request.method)) {
        out.error = error(QStringLiteral("protocol.invalid_envelope"),
                          QStringLiteral("id or method has invalid syntax"));
        return out;
    }

    const QJsonValue sessionValue = object.value(QStringLiteral("sessionId"));
    if (request.isHello()) {
        if (!sessionValue.isUndefined()) {
            out.error = error(QStringLiteral("protocol.invalid_envelope"),
                              QStringLiteral("hello must not carry a sessionId"));
            return out;
        }
    } else {
        if (!sessionValue.isString() || sessionValue.toString().isEmpty()
            || sessionValue.toString().size() > ProtocolLimits::kMaxRequestIdChars) {
            out.error = error(QStringLiteral("session.invalid"),
                              QStringLiteral("request requires a valid sessionId"));
            return out;
        }
        request.sessionId = sessionValue.toString();
    }

    out.request = std::move(request);
    return out;
}

QJsonObject ControlProtocolCodec::successResponse(
    const QString& id, const QJsonObject& result)
{
    return {{QStringLiteral("v"), 1},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), result}};
}

QJsonObject ControlProtocolCodec::errorResponse(
    const QString& id, const ProtocolError& protocolError)
{
    QJsonObject body{{QStringLiteral("code"), protocolError.code},
                     {QStringLiteral("message"), protocolError.message},
                     {QStringLiteral("retryable"), protocolError.retryable}};
    if (!protocolError.data.isEmpty()) {
        body.insert(QStringLiteral("data"), protocolError.data);
    }
    QJsonObject response{{QStringLiteral("v"), 1},
                         {QStringLiteral("error"), body}};
    if (!id.isEmpty()) {
        response.insert(QStringLiteral("id"), id);
    }
    return response;
}

} // namespace AetherSDR::control
