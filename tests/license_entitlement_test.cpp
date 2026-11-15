// SmartSDR+ entitlement rules (models/LicenseEntitlement.h): a lapsed
// subscription keeps the features released before it expired, so feature gates
// read the radio's feature record and the subscription only labels the panel.
#include "models/LicenseEntitlement.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void report(const char* name, bool passed)
{
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) {
        ++failures;
    }
}

}  // namespace

int main()
{
    // ── Feature gate: the radio's record decides once seen ──────────────────
    report("seen + enabled is usable with a lapsed subscription",
           smartSdrPlusFeatureUsable(true, true, false));
    report("seen + disabled stays locked with an active subscription",
           !smartSdrPlusFeatureUsable(true, false, true));
    report("unseen falls back to an active subscription",
           smartSdrPlusFeatureUsable(false, false, true));
    report("unseen with no active subscription is locked",
           !smartSdrPlusFeatureUsable(false, false, false));

    // ── Subscription merge ──────────────────────────────────────────────────
    const QDateTime now(QDate(2026, 10, 31), QTime(12, 0));
    const LicenseSubscriptionState base{QStringLiteral("SmartSDR"), {}};
    report("base SmartSDR has no expired marker", base.label(now) == QStringLiteral("SmartSDR"));

    const auto lapsed = mergeLicenseSubscription(base, QStringLiteral("smartsdr+"),
                                                 QStringLiteral("2026-03-01T00:00:00"));
    report("lapsed SmartSDR+ is held, inactive, labeled expired",
           !lapsed.isActive(now) && lapsed.expiration.date() == QDate(2026, 3, 1)
               && lapsed.label(now) == QStringLiteral("SmartSDR+ (expired)"));

    const auto current = mergeLicenseSubscription(lapsed, QStringLiteral("SMARTSDR+_EARLY_ACCESS"),
                                                  QStringLiteral("2027-01-15"));
    report("an active subscription replaces a lapsed one",
           current.isActive(now) && current.label(now) == QStringLiteral("SmartSDR+ Early Access")
               && current.expiration.date() == QDate(2027, 1, 15));

    const auto kept = mergeLicenseSubscription(current, QStringLiteral("smartsdr+"),
                                               QStringLiteral("2025-01-01"));
    report("a lapsed line never replaces an active subscription",
           kept.isActive(now) && kept.label(now) == QStringLiteral("SmartSDR+ Early Access"));

    const auto older = mergeLicenseSubscription(lapsed, QStringLiteral("smartsdr+_early_access"),
                                                QStringLiteral("2025-01-01"));
    report("between two lapsed lines the later expiration wins",
           older.label(now) == QStringLiteral("SmartSDR+ (expired)")
               && older.expiration.date() == QDate(2026, 3, 1));

    // Two active lines: the later expiration wins whichever arrives first.
    const auto plus = QStringLiteral("smartsdr+");
    const auto ea = QStringLiteral("smartsdr+_early_access");
    const auto plusFirst = mergeLicenseSubscription(
        mergeLicenseSubscription(base, plus, QStringLiteral("2027-06-01")), ea, QStringLiteral("2026-12-01"));
    const auto eaFirst = mergeLicenseSubscription(
        mergeLicenseSubscription(base, ea, QStringLiteral("2026-12-01")), plus, QStringLiteral("2027-06-01"));
    report("two active lines: the later expiration wins in either order",
           plusFirst.label(now) == QStringLiteral("SmartSDR+")
               && eaFirst.label(now) == QStringLiteral("SmartSDR+")
               && plusFirst.expiration == eaFirst.expiration);

    // FlexLib: active while (expiration - Now).TotalDays > 0, to the second.
    const auto midnight = mergeLicenseSubscription(base, plus, QStringLiteral("2026-10-31T00:00:00"));
    report("an expiration at midnight today has lapsed by noon",
           !midnight.isActive(now) && midnight.label(now) == QStringLiteral("SmartSDR+ (expired)"));
    const auto bareDate = mergeLicenseSubscription(base, plus, QStringLiteral("2026-10-31"));
    report("a bare date is local midnight, as FlexLib's DateTime.TryParse reads it",
           bareDate.expiration == QDateTime(QDate(2026, 10, 31), QTime(0, 0)) && !bareDate.isActive(now));
    const auto tonight = mergeLicenseSubscription(base, plus, QStringLiteral("2026-10-31T23:59:59"));
    report("active until the expiration instant, lapsed after it (read time)",
           tonight.isActive(now) && !tonight.isActive(QDateTime(QDate(2026, 11, 1), QTime(0, 0))));

    const auto unknown = mergeLicenseSubscription(current, QStringLiteral("smartsdr_pro"),
                                                  QStringLiteral("2030-01-01"));
    report("an unknown subscription name is ignored",
           unknown.label(now) == current.label(now) && unknown.expiration == current.expiration);
    const auto badDate = mergeLicenseSubscription(base, plus, QStringLiteral("soon"));
    report("an unparseable expiration is ignored", badDate.label(now) == QStringLiteral("SmartSDR"));
    const auto trailing = mergeLicenseSubscription(base, plus, QStringLiteral("2026-11-01garbage"));
    report("an expiration with trailing text is ignored", trailing.label(now) == QStringLiteral("SmartSDR"));

    return failures ? 1 : 0;
}
