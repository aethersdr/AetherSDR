#include "SettingsSanitizer.h"

#include "AppSettings.h"
#include "SettingsCredentialPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace AetherSDR {
namespace SettingsSanitizer {

namespace {

const QString kRedacted = QStringLiteral("[REDACTED]");

QJsonValue redactJsonValue(const QJsonValue& value);

QJsonObject redactJsonObject(const QJsonObject& obj)
{
    QJsonObject out;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (isSecretKey(it.key())) {
            out.insert(it.key(), kRedacted);
        } else {
            out.insert(it.key(), redactJsonValue(it.value()));
        }
    }
    return out;
}

QJsonValue redactJsonValue(const QJsonValue& value)
{
    if (value.isObject()) {
        return redactJsonObject(value.toObject());
    }
    if (value.isArray()) {
        QJsonArray out;
        const QJsonArray in = value.toArray();
        for (const QJsonValue& element : in) {
            out.append(redactJsonValue(element));
        }
        return out;
    }
    return value;
}

} // namespace

bool isSecretKey(const QString& key)
{
    // Exact names first, derived from THE credential list — the shape regex
    // alone missed "MqttPass", the literal legacy name the exodus list itself
    // declared (PR #4612 review, K5PTB). Deriving from one table means a new
    // credential added to the policy is redacted here for free.
    if (SettingsCredentialPolicy::isFlatCredentialKey(key)) {
        return true;
    }
    for (const auto& entry : SettingsCredentialPolicy::kDocFieldCredentials) {
        if (key == QLatin1String(entry.field)) {
            return true;
        }
    }
    static const QRegularExpression secretPattern(
        QStringLiteral("token|password|passphrase|secret|auth0|refresh|"
                       "api_?key|credential|\\bpass\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return secretPattern.match(key).hasMatch();
}

QString redactedValue(const QString& key, const QString& value)
{
    if (isSecretKey(key)) {
        return kRedacted;
    }
    // Recursive redaction inside JSON document values: a secret-shaped FIELD
    // inside an innocuously-named row must not survive (e.g. an api key field
    // inside a feature config object).
    const QByteArray raw = value.toUtf8().trimmed();
    if (raw.startsWith('{') || raw.startsWith('[')) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error == QJsonParseError::NoError && !doc.isNull()) {
            const QJsonDocument redacted =
                doc.isArray()
                    ? QJsonDocument(redactJsonValue(doc.array()).toArray())
                    : QJsonDocument(redactJsonObject(doc.object()));
            return QString::fromUtf8(redacted.toJson(QJsonDocument::Compact));
        }
    }
    return value;
}

QString dump()
{
    auto& s = AppSettings::instance();
    QString out;
    out += QStringLiteral("## app_settings\n");
    // Deterministic order for diffing between two bundles.
    const QStringList keys = [&s] {
        QStringList all;
        // AppSettings has no key-enumeration API by design; the dump goes
        // through the sanitizer-only enumeration hook below.
        all = s.allKeysForDiagnostics();
        all.sort();
        return all;
    }();
    for (const QString& key : keys) {
        out += key + QLatin1Char('\t')
               + redactedValue(key, s.value(key).toString()) + QLatin1Char('\n');
    }

    const QStringList stationKeys = [&s] {
        QStringList all = s.stationKeysForDiagnostics();
        all.sort();
        return all;
    }();
    if (!stationKeys.isEmpty()) {
        out += QStringLiteral("## station_settings (%1)\n").arg(s.stationName());
        for (const QString& key : stationKeys) {
            out += key + QLatin1Char('\t')
                   + redactedValue(key, s.stationValue(key).toString())
                   + QLatin1Char('\n');
        }
    }
    return out;
}

} // namespace SettingsSanitizer
} // namespace AetherSDR
