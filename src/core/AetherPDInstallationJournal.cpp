#include "AetherPDInstallationJournal.h"
#include "AetherPDProtocol.h"
#include "RadioSettingsScope.h"

#include <QJsonObject>
#include <QString>
#include <optional>

namespace AetherSDR {
namespace {
const QString kFeature = QStringLiteral("AetherPDInstallation");
constexpr int kSchema = 1;
QJsonObject document(const QByteArray& plan, const QByteArray& key, bool completed)
{
    return {{QStringLiteral("plan"), QString::fromLatin1(plan.toHex())},
        {QStringLiteral("publisherKey"), QString::fromLatin1(key.toHex())},
        {QStringLiteral("credentialSaved"), completed}};
}
bool target(const RadioSettingsScope& scope, const QByteArray& plan, const QByteArray& key)
{
    const auto parsed = aetherpd::parseFirstInstallationPlan(plan, key);
    return scope.isValid() && !scope.radioId().isEmpty() && parsed && parsed->serial == scope.radioId();
}
} // namespace

AetherPDInstallationJournal::Read AetherPDInstallationJournal::read(const RadioSettingsScope& scope, const QByteArray& publisherKey)
{
    if (!scope.isValid() || scope.radioId().isEmpty()) { return {}; }
    int schema = 0;
    AppSettings::FeatureReadStatus status;
    const QJsonObject stored = scope.featureExact(kFeature, &schema, &status);
    if (status == AppSettings::FeatureReadStatus::Missing) { return {State::Missing, {}}; }
    if (status == AppSettings::FeatureReadStatus::Unavailable) { return {}; }
    if (status != AppSettings::FeatureReadStatus::Present || schema != kSchema || stored.size() != 3
        || !stored.value(QStringLiteral("credentialSaved")).isBool()) { return {State::Invalid, {}}; }
    const QString encoded = stored.value(QStringLiteral("plan")).toString();
    if (encoded.size() != 616 || stored.value(QStringLiteral("publisherKey")).toString()
        != QString::fromLatin1(publisherKey.toHex())) { return {State::Invalid, {}}; }
    const QByteArray plan = QByteArray::fromHex(encoded.toLatin1());
    if (QString::fromLatin1(plan.toHex()) != encoded || !target(scope, plan, publisherKey)) { return {State::Invalid, {}}; }
    return {stored.value(QStringLiteral("credentialSaved")).toBool() ? State::CredentialSaved : State::Pending, plan};
}
bool AetherPDInstallationJournal::record(const RadioSettingsScope& scope, const QByteArray& plan, const QByteArray& publisherKey)
{
    if (!target(scope, plan, publisherKey)) { return false; }
    const Read previous = read(scope, publisherKey);
    if (previous.state == State::Pending && previous.plan != plan) { return false; }
    if (previous.state != State::Missing && previous.state != State::Pending && previous.state != State::CredentialSaved) { return false; }
    const std::optional<QJsonObject> expected = previous.state == State::Missing ? std::nullopt
        : std::optional<QJsonObject>(document(previous.plan, publisherKey, previous.state == State::CredentialSaved));
    return scope.compareAndSetFeature(kFeature, kSchema, expected, kSchema, document(plan, publisherKey, false));
}
bool AetherPDInstallationJournal::complete(const RadioSettingsScope& scope, const QByteArray& plan, const QByteArray& publisherKey)
{
    const Read previous = read(scope, publisherKey);
    if ((previous.state != State::Pending && previous.state != State::CredentialSaved) || previous.plan != plan) { return false; }
    return scope.compareAndSetFeature(kFeature, kSchema, document(previous.plan, publisherKey, previous.state == State::CredentialSaved),
        kSchema, document(plan, publisherKey, true));
}
} // namespace AetherSDR
