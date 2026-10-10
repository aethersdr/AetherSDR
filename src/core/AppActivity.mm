#include "AppActivity.h"
#include "LogManager.h"

#import <Foundation/Foundation.h>

namespace AetherSDR {

void keepAppActive()
{
    static id<NSObject> activity = nil;   // held for the life of the process
    if (activity) {
        return;
    }
    // No App Nap, but idle system sleep is still allowed: sleep is governed
    // only by "Prevent system sleep while connected" (SleepInhibitor).
    activity = [[[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                          reason:@"AetherSDR audio, DAX and TCI streaming"] retain];
    if (activity) {
        qCInfo(lcAudio) << "AppActivity: App Nap disabled (idle sleep still allowed)";
    } else {
        qCWarning(lcAudio) << "AppActivity: beginActivityWithOptions returned nil";
    }
}

} // namespace AetherSDR
