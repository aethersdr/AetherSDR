#pragma once

#include <QString>

namespace AetherSDR {

// The radio-reported address to try after an automatic connect to a saved
// manual address never reached the device. Empty when no retry applies:
// no saved manual address, the failed attempt was not to that address, the
// radio reports nothing or the same address, auth is blocked, or the one retry
// has been spent.
inline QString peripheralFallbackHost(const QString& attemptedHost, const QString& manualIp,
                                      const QString& reportedIp, bool authBlocked,
                                      bool alreadyTried)
{
    const QString manual = manualIp.trimmed();
    const QString reported = reportedIp.trimmed();
    if (authBlocked || alreadyTried || manual.isEmpty() || reported.isEmpty()) {
        return {};
    }
    if (attemptedHost.trimmed().compare(manual, Qt::CaseInsensitive) != 0
        || reported.compare(manual, Qt::CaseInsensitive) == 0) {
        return {};
    }
    return reported;
}

}  // namespace AetherSDR
