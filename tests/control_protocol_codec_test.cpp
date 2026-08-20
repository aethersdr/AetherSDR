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
    return check(body.value(QStringLiteral("code")).toString()
                     == QStringLiteral("request.out_of_range")
                 && body.value(QStringLiteral("data")).toObject()
                        .value(QStringLiteral("field")).toString() == QStringLiteral("hz"),
                 "error response must preserve stable code and safe data");
}

} // namespace

int main()
{
    return testValidHello()
        && testValidSessionRequest()
        && testEnvelopeFailures()
        && testJsonSafetyFailures()
        && testResponseShapes() ? 0 : 1;
}
