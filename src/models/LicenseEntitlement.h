#pragma once

#include <QDate>
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
// each. An active one always wins; a lapsed one (the most recently expired of
// several) is shown, marked expired, only while nothing active is held, so a
// lapsed subscriber can still see the subscription their retained features
// came from.
struct LicenseSubscriptionState {
    QString name;        // "SmartSDR", "SmartSDR+", "SmartSDR+ Early Access"
    QDate   expiration;  // invalid for the base "SmartSDR"
    bool    active{false};

    QString label() const
    {
        if (name.isEmpty() || active || !expiration.isValid()) {
            return name;
        }
        return name + QStringLiteral(" (expired)");
    }
};

// Folds one `license subscription name=<n> expiration=<date>` status into the
// held state. Unknown names and unparseable dates leave it unchanged.
inline LicenseSubscriptionState mergeLicenseSubscription(
    const LicenseSubscriptionState& held, const QString& wireName,
    const QString& expiration, const QDate& today)
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
    const QDate date = QDate::fromString(expiration.left(10), Qt::ISODate);
    if (!date.isValid()) {
        return held;
    }
    const bool active = date >= today;
    // Active beats lapsed; between two lapsed lines the later expiration wins.
    if (!active && (held.active
                    || (held.expiration.isValid() && held.expiration > date))) {
        return held;
    }
    return {display, date, active};
}

}  // namespace AetherSDR
