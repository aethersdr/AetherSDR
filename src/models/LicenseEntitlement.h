#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>

namespace AetherSDR {

// The radio's feature record is authoritative, including features retained
// after SmartSDR+ expires (FlexLib FeatureLicense.cs, v4.3.16.42034).

// Whether a feature gated on SmartSDR+ can be used. The radio's feature record
// decides once it has been seen. An active subscription is only the fallback for
// firmware that never reports the feature.
inline bool smartSdrPlusFeatureUsable(bool featureSeen, bool featureEnabled,
                                      bool subscriptionActive)
{
    return featureSeen ? featureEnabled : subscriptionActive;
}

// The subscription shown in Radio Setup → License Info. A radio can report
// several (SmartSDR+ and Early Access, current and lapsed), one status line
// each; the one with the latest expiration is held, so an active one always
// beats a lapsed one and the order the lines arrive in never matters. A lapsed
// one is still shown, marked expired, so a lapsed subscriber can see the
// subscription their retained features came from. Whether it is active is
// decided when read, so a subscription that lapses mid-session reads as lapsed.
struct LicenseSubscriptionState {
    QString   name;        // "SmartSDR", "SmartSDR+", "SmartSDR+ Early Access"
    QDateTime expiration;  // invalid for the base "SmartSDR"

    // FlexLib: active while (expiration - DateTime.Now).TotalDays > 0.
    bool isActive(const QDateTime& now) const
    {
        return expiration.isValid() && expiration > now;
    }

    QString label(const QDateTime& now) const
    {
        if (name.isEmpty() || !expiration.isValid() || isActive(now)) {
            return name;
        }
        return name + QStringLiteral(" (expired)");
    }
};

// The radio's `expiration=` value: an ISO-8601 date-time, or a bare date, which
// FlexLib's DateTime.TryParse reads as local midnight.
inline QDateTime parseLicenseExpiration(const QString& text)
{
    const QString trimmed = text.trimmed();
    QDateTime dateTime = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!dateTime.isValid()) {
        const QDate date = QDate::fromString(trimmed.left(10), Qt::ISODate);
        if (date.isValid()) {
            dateTime = date.startOfDay();
        }
    }
    return dateTime;
}

// Folds one `license subscription name=<n> expiration=<date>` status into the
// held state. Unknown names and unparseable dates leave it unchanged.
inline LicenseSubscriptionState mergeLicenseSubscription(
    const LicenseSubscriptionState& held, const QString& wireName,
    const QString& expiration)
{
    const QString name = wireName.trimmed().toLower();
    QString display;
    if (name == QLatin1String("smartsdr+_early_access")) {
        display = QStringLiteral("SmartSDR+ Early Access");
    } else if (name == QLatin1String("smartsdr+")) {
        display = QStringLiteral("SmartSDR+");
    } else {
        return held;
    }
    const QDateTime parsed = parseLicenseExpiration(expiration);
    if (!parsed.isValid()) {
        return held;
    }
    if (held.expiration.isValid() && held.expiration >= parsed) {
        return held;
    }
    return {display, parsed};
}

}  // namespace AetherSDR
