#include "AppActivity.h"
#include "LogManager.h"

#import <Foundation/Foundation.h>

namespace AetherSDR {

void keepAppActive()
{
    static id<NSObject> activity = nil;   // held for the life of the process
    if (activity)
        return;
    // NSActivityUserInitiated = no App Nap + no idle system sleep. Shows in
    // `pmset -g assertions` as PreventUserIdleSystemSleep with this reason.
    activity = [[[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated
                          reason:@"AetherSDR audio, DAX and TCI streaming"] retain];
    qCInfo(lcAudio) << "AppActivity: App Nap and idle system sleep disabled";
}

} // namespace AetherSDR
