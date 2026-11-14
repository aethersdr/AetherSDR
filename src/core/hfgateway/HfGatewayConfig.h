#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

// The HF Gateway's configuration (RFC #7296), as one value with a pure
// JSON round trip. HfGatewaySettings stores it as one nested object under
// one root key (Principle V), written whole (Principle XIV).
//
// fromJson() is the boundary for whatever is in the store: every field is
// checked and anything out of range falls back to its default, so a damaged
// or hand-edited document can't produce an unsafe configuration.
//
// Nothing here can arm the gateway or key the radio. The transmit arm is
// never stored, by design.

namespace AetherSDR::HfGateway {

// Which kind of external modem an engine is. The profile only adjusts
// defaults and labels; every profile speaks the same host interface.
enum class Profile {
    VaraCompatible,  // any modem speaking the VARA-style host interface, run by the operator
    Mercury,
    Data2gHost,
};

QString toString(Profile profile);
// Unknown text gives VaraCompatible.
Profile profileFromString(const QString& text);

struct EngineConfig {
    QString id;  // stable, unique within the config; [a-z0-9-], 1-32 chars
    Profile profile = Profile::VaraCompatible;
    QString label;  // operator's name for it, display only
    QString host = QStringLiteral("127.0.0.1");
    quint16 commandPort = 8300;
    quint16 dataPort = 8301;
};

struct BackendConfig {
    quint16 commandPort = 8500;  // the gateway's own VARA-style port pair (Stage 2b)
    quint16 dataPort = 8501;
    bool loopbackOnly = true;
};

struct Config {
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaxEngines = 4;

    QList<EngineConfig> engines;
    BackendConfig backend;
};

// Ports below 1024 need privileges on most systems and are never a modem's
// host port; accept 1024-65535 only.
bool isValidPort(int port);

// Engine ids: 1-32 characters of a-z, 0-9 and '-'.
bool isValidEngineId(const QString& id);

// Hosts: a non-empty name or address of at most 253 characters with no
// whitespace or control characters. Not resolved here.
bool isValidHost(const QString& host);

// True when `host` is a loopback name or address.
bool isLoopbackHost(const QString& host);

QJsonObject toJson(const Config& config);

// Reads a stored document. Engines with an invalid or duplicate id are
// dropped; other bad fields take their defaults; at most kMaxEngines are
// kept. A document from a newer schema version is read as far as it can be.
Config fromJson(const QJsonObject& json);

}  // namespace AetherSDR::HfGateway
