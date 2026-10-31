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
    const QDate today(2026, 10, 31);
    const LicenseSubscriptionState base{QStringLiteral("SmartSDR"), {}, false};
    report("base SmartSDR has no expired marker", base.label() == QStringLiteral("SmartSDR"));

    const auto lapsed = mergeLicenseSubscription(base, QStringLiteral("smartsdr+"),
                                                 QStringLiteral("2026-03-01T00:00:00"), today);
    report("lapsed SmartSDR+ is held, inactive, labeled expired",
           !lapsed.active && lapsed.expiration == QDate(2026, 3, 1)
               && lapsed.label() == QStringLiteral("SmartSDR+ (expired)"));

    const auto current = mergeLicenseSubscription(lapsed, QStringLiteral("SMARTSDR+_EARLY_ACCESS"),
                                                  QStringLiteral("2027-01-15"), today);
    report("an active subscription replaces a lapsed one",
           current.active && current.label() == QStringLiteral("SmartSDR+ Early Access")
               && current.expiration == QDate(2027, 1, 15));

    const auto kept = mergeLicenseSubscription(current, QStringLiteral("smartsdr+"),
                                               QStringLiteral("2025-01-01"), today);
    report("a lapsed line never replaces an active subscription",
           kept.active && kept.label() == QStringLiteral("SmartSDR+ Early Access"));

    const auto older = mergeLicenseSubscription(lapsed, QStringLiteral("smartsdr+_early_access"),
                                                QStringLiteral("2025-01-01"), today);
    report("between two lapsed lines the later expiration wins",
           older.label() == QStringLiteral("SmartSDR+ (expired)")
               && older.expiration == QDate(2026, 3, 1));

    report("expiring today still counts as active",
           mergeLicenseSubscription(base, QStringLiteral("smartsdr+"),
                                    QStringLiteral("2026-10-31"), today).active);

    const auto unknown = mergeLicenseSubscription(current, QStringLiteral("smartsdr_pro"),
                                                  QStringLiteral("2030-01-01"), today);
    report("an unknown subscription name is ignored",
           unknown.label() == current.label() && unknown.expiration == current.expiration);
    const auto badDate = mergeLicenseSubscription(base, QStringLiteral("smartsdr+"),
                                                  QStringLiteral("soon"), today);
    report("an unparseable expiration is ignored", badDate.label() == QStringLiteral("SmartSDR"));

    return failures ? 1 : 0;
}
