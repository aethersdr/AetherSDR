#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace AetherSDR::control {

struct ProtocolLimits {
    static constexpr qsizetype kMaxMessageBytes = 256 * 1024;
    static constexpr int kMaxNesting = 32;
    static constexpr qsizetype kMaxStringChars = 64 * 1024;
    static constexpr qsizetype kMaxArrayEntries = 4096;
    static constexpr qsizetype kMaxRequestIdChars = 64;
    static constexpr qsizetype kMaxMethodChars = 128;
};

struct ProtocolError {
    QString code;
    QString message;
    QJsonObject data;
    bool retryable{false};
};

struct ProtocolRequest {
    int version{0};
    QString id;
    QString sessionId;
    QString method;
    QJsonObject params;

    [[nodiscard]] bool isHello() const { return method == QStringLiteral("hello"); }
};

struct ParseResult {
    std::optional<ProtocolRequest> request;
    std::optional<ProtocolError> error;
    QString requestId;

    [[nodiscard]] bool ok() const { return request.has_value(); }
};

// Transport-neutral codec for the AetherD control plane. It owns only framing-
// independent JSON validation and envelope serialization; authentication,
// method dispatch and model access live above it.
class ControlProtocolCodec final {
public:
    [[nodiscard]] static ParseResult parseRequest(const QByteArray& bytes);

    [[nodiscard]] static QJsonObject successResponse(
        const QString& id, const QJsonObject& result);
    [[nodiscard]] static QJsonObject errorResponse(
        const QString& id, const ProtocolError& error);

private:
    ControlProtocolCodec() = delete;
};

} // namespace AetherSDR::control
