#pragma once

#include "core/backends/SetupStatusObservation.h"

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

// Folds one `license subscription` report into the held state: the later
// expiration wins. Unknown names and invalid dates leave it unchanged.
inline LicenseSubscriptionState mergeLicenseSubscription(
    const LicenseSubscriptionState& held, const QString& wireName, const QDateTime& expiration)
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
    if (!expiration.isValid() || (held.expiration.isValid() && held.expiration >= expiration)) {
        return held;
    }
    return {display, expiration};
}

inline LicenseSubscriptionState mergeLicenseSubscription(
    const LicenseSubscriptionState& held, const QString& wireName, const QString& expiration)
{
    return mergeLicenseSubscription(held, wireName, parseLicenseExpiration(expiration));
}

}  // namespace AetherSDR
