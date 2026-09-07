#include "core/control/ControlProtocolCodec.h"

#include <QJsonObject>

#include <cstdio>

using AetherSDR::control::ControlProtocolCodec;
using AetherSDR::control::ParseResult;
using AetherSDR::control::ProtocolError;
using AetherSDR::control::ProtocolLimits;

namespace {

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "%s\n", message);
    return false;
}

bool hasError(const ParseResult& result, const QString& code)
{
    return result.error.has_value() && result.error->code == code;
}

bool testValidHello()
{
    const ParseResult result = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"hello-1","method":"hello","params":{"versions":[1]}})");
    return check(result.ok(), "valid hello must parse")
        && check(result.request->isHello(), "hello method must be recognized")
        && check(result.request->sessionId.isEmpty(), "hello must have no sessionId");
}

bool testValidSessionRequest()
{
    const ParseResult result = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"req-1","sessionId":"session-1","method":"slice.setFrequency","params":{"hz":14225000}})");
    return check(result.ok(), "valid session request must parse")
        && check(result.request->sessionId == QStringLiteral("session-1"),
                 "sessionId must survive parsing");
}

bool testEnvelopeFailures()
{
    const ParseResult unknown = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"x","method":"hello","params":{},"typo":true})");
    if (!check(hasError(unknown, QStringLiteral("protocol.invalid_envelope")),
               "unknown envelope field must fail closed")) {
        return false;
    }

    const ParseResult missingSession = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"x","method":"resource.get","params":{}})");
    if (!check(hasError(missingSession, QStringLiteral("session.invalid")),
               "non-hello request must require a session")) {
        return false;
    }

    const ParseResult futureFraming = ControlProtocolCodec::parseRequest(
        R"({"v":2,"id":"x","method":"hello","params":{"versions":[1,2]}})");
    if (!check(hasError(futureFraming, QStringLiteral("protocol.invalid_envelope")),
               "the initial hello must use the baseline v1 envelope")) {
        return false;
    }

    const ParseResult helloSession = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"x","sessionId":"s","method":"hello","params":{}})");
    return check(hasError(helloSession, QStringLiteral("protocol.invalid_envelope")),
                 "hello must reject a supplied session");
}

bool testJsonSafetyFailures()
{
    const ParseResult duplicate = ControlProtocolCodec::parseRequest(
        R"({"v":1,"id":"first","id":"second","method":"hello","params":{}})");
    if (!check(hasError(duplicate, QStringLiteral("protocol.invalid_json")),
               "duplicate keys must be rejected before QJsonDocument last-wins parsing")) {
        return false;
    }

    QByteArray nested = R"({"v":1,"id":"x","method":"hello","params":)";
    nested.append(ProtocolLimits::kMaxNesting, '[');
    nested.append(ProtocolLimits::kMaxNesting, ']');
    nested.append('}');
    const ParseResult tooDeep = ControlProtocolCodec::parseRequest(nested);
    if (!check(hasError(tooDeep, QStringLiteral("protocol.invalid_json")),
               "excessive JSON nesting must be rejected")) {
        return false;
    }

    QByteArray tooLarge(ProtocolLimits::kMaxMessageBytes + 1, ' ');
    const ParseResult oversized = ControlProtocolCodec::parseRequest(tooLarge);
    return check(hasError(oversized, QStringLiteral("transport.limit_exceeded")),
                 "message byte limit must be checked before parsing");
}

QByteArray requestWithParams(const QByteArray& params, const QByteArray& id = "x",
                             const QByteArray& method = "hello",
                             const QByteArray& sessionField = {})
{
    return QByteArrayLiteral("{\"v\":1,\"id\":\"") + id
        + QByteArrayLiteral("\",") + sessionField
        + QByteArrayLiteral("\"method\":\"") + method
        + QByteArrayLiteral("\",\"params\":") + params + QByteArrayLiteral("}");
}

bool testStructuralBoundaries()
{
    const QByteArray maxString(ProtocolLimits::kMaxStringChars, 'a');
    const ParseResult maxStringResult = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{\"value\":\"") + maxString
                          + QByteArrayLiteral("\"}")));
    if (!check(maxStringResult.ok(), "a string at the character limit must parse")) {
        return false;
    }
    const ParseResult oversizedString = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{\"value\":\"") + maxString + 'a'
                          + QByteArrayLiteral("\"}")));
    if (!check(hasError(oversizedString, QStringLiteral("transport.limit_exceeded")),
               "a string over the character limit must fail")) {
        return false;
    }

    QByteArray maxArray = QByteArrayLiteral("{\"values\":[");
    for (qsizetype index = 0; index < ProtocolLimits::kMaxArrayEntries; ++index) {
        if (index != 0) {
            maxArray.append(',');
        }
        maxArray.append('0');
    }
    maxArray.append(QByteArrayLiteral("]}"));
    if (!check(ControlProtocolCodec::parseRequest(requestWithParams(maxArray)).ok(),
               "an array at the entry limit must parse")) {
        return false;
    }
    maxArray.insert(maxArray.size() - 2, QByteArrayLiteral(",0"));
    if (!check(hasError(ControlProtocolCodec::parseRequest(requestWithParams(maxArray)),
                        QStringLiteral("protocol.invalid_json")),
               "an array over the entry limit must fail")) {
        return false;
    }

    QByteArray maxDepth = QByteArrayLiteral("{\"nested\":");
    maxDepth.append(ProtocolLimits::kMaxNesting - 2, '[');
    maxDepth.append(ProtocolLimits::kMaxNesting - 2, ']');
    maxDepth.append('}');
    if (!check(ControlProtocolCodec::parseRequest(requestWithParams(maxDepth)).ok(),
               "JSON at the nesting limit must parse")) {
        return false;
    }
    maxDepth.insert(QByteArrayLiteral("{\"nested\":").size(), '[');
    maxDepth.insert(maxDepth.size() - 1, ']');
    if (!check(hasError(ControlProtocolCodec::parseRequest(requestWithParams(maxDepth)),
                        QStringLiteral("protocol.invalid_json")),
               "JSON over the nesting limit must fail")) {
        return false;
    }

    const ParseResult nonFinite = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{\"value\":1e9999}")));
    const ParseResult trailing = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{}")) + QByteArrayLiteral(" true"));
    return check(hasError(nonFinite, QStringLiteral("protocol.invalid_json")),
                 "a non-finite JSON number must fail")
        && check(hasError(trailing, QStringLiteral("protocol.invalid_json")),
                 "trailing data after the request must fail");
}

bool testIdentifierAndMethodBoundaries()
{
    const QByteArray maxId(ProtocolLimits::kMaxRequestIdChars, 'a');
    if (!check(ControlProtocolCodec::parseRequest(
                   requestWithParams(QByteArrayLiteral("{\"versions\":[1]}"), maxId)).ok(),
               "a request id at the length limit must parse")) {
        return false;
    }
    const ParseResult oversizedId = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{\"versions\":[1]}"), maxId + 'a'));
    if (!check(hasError(oversizedId, QStringLiteral("protocol.invalid_envelope"))
                   && oversizedId.requestId.isEmpty(),
               "an oversized request id must fail without being reflected")) {
        return false;
    }
    const ParseResult controlId = ControlProtocolCodec::parseRequest(
        QByteArrayLiteral("{\"v\":1,\"id\":\"\\u0000\",\"method\":\"hello\","
                          "\"params\":{\"versions\":[1]}}"));
    if (!check(hasError(controlId, QStringLiteral("protocol.invalid_envelope"))
                   && controlId.requestId.isEmpty(),
               "a non-printable request id must fail without being reflected")) {
        return false;
    }

    const QByteArray maxMethod(ProtocolLimits::kMaxMethodChars, 'a');
    const QByteArray session = QByteArrayLiteral("\"sessionId\":\"session-1\",");
    if (!check(ControlProtocolCodec::parseRequest(
                   requestWithParams(QByteArrayLiteral("{}"), QByteArrayLiteral("x"),
                                     maxMethod, session)).ok(),
               "a method at the length limit must parse")) {
        return false;
    }
    const ParseResult oversizedMethod = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{}"), QByteArrayLiteral("x"),
                          maxMethod + 'a', session));
    const ParseResult invalidMethod = ControlProtocolCodec::parseRequest(
        requestWithParams(QByteArrayLiteral("{}"), QByteArrayLiteral("x"),
                          QByteArrayLiteral("slice..get"), session));
    return check(hasError(oversizedMethod, QStringLiteral("protocol.invalid_envelope")),
                 "a method over the length limit must fail")
        && check(hasError(invalidMethod, QStringLiteral("protocol.invalid_envelope")),
                 "a method with an empty segment must fail");
}

bool testResponseShapes()
{
    const QJsonObject success = ControlProtocolCodec::successResponse(
        QStringLiteral("r1"), {{QStringLiteral("accepted"), true}});
    if (!check(success.value(QStringLiteral("v")).toInt() == 1
               && success.value(QStringLiteral("id")).toString() == QStringLiteral("r1")
               && success.value(QStringLiteral("result")).isObject(),
               "success response must have v/id/result")) {
        return false;
    }

    const ProtocolError source{QStringLiteral("request.out_of_range"),
                               QStringLiteral("bad value"),
                               {{QStringLiteral("field"), QStringLiteral("hz")}}, false};
    const QJsonObject failure = ControlProtocolCodec::errorResponse(QStringLiteral("r2"), source);
    const QJsonObject body = failure.value(QStringLiteral("error")).toObject();
    if (!check(failure.value(QStringLiteral("id")).toString() == QStringLiteral("r2"),
               "a correlated error response must preserve its request id")
        || !check(body.value(QStringLiteral("code")).toString()
                     == QStringLiteral("request.out_of_range")
                 && body.value(QStringLiteral("data")).toObject()
                        .value(QStringLiteral("field")).toString() == QStringLiteral("hz"),
                 "error response must preserve stable code and safe data")) {
        return false;
    }

    const QJsonObject uncorrelated = ControlProtocolCodec::errorResponse({}, source);
    return check(!uncorrelated.contains(QStringLiteral("id")),
                 "an uncorrelated error response must omit the request id");
}

} // namespace

int main()
{
    return testValidHello()
        && testValidSessionRequest()
        && testEnvelopeFailures()
        && testJsonSafetyFailures()
        && testStructuralBoundaries()
        && testIdentifierAndMethodBoundaries()
        && testResponseShapes() ? 0 : 1;
}
