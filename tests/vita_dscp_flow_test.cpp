#include "core/backends/flex/VitaDscpFlow.h"

#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {
// This fake pins the requested DSCP value and rollback sequence. It does not
// exercise the native QOSTrafficTypeBestEffort constant or OS policy.
struct FlowApi {
    using FlowId = unsigned long;
    bool allowAdd{true};
    bool allowEf{true};
    std::vector<int> calls;
    int requestedDscp{-1};
    FlowId removed{0};

    bool addBestEffortFlow(FlowId& flow)
    {
        calls.push_back(1);
        if (allowAdd) {
            flow = 73;
        }
        return allowAdd;
    }
    bool setDscp(FlowId flow, int dscp)
    {
        calls.push_back(flow == 73 ? 2 : -2);
        requestedDscp = dscp;
        return allowEf;
    }
    void removeFlow(FlowId flow)
    {
        calls.push_back(3);
        removed = flow;
    }
};

int g_failed = 0;
void check(bool ok, const char* description)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", description);
    if (!ok) {
        ++g_failed;
    }
}
} // namespace

int main()
{
    check(kVitaDscpEf == 46 && kVitaIpTosEf == 0xB8, "EF uses DSCP 46 and IPv4 TOS 0xB8");
    FlowApi success;
    check(createVitaEfFlow(success) == 73, "accepted EF flow remains owned by the caller");
    check(success.requestedDscp == 46, "flow helper requests the six-bit DSCP value, not TOS or the voice default");
    check(success.calls == std::vector<int>({1, 2}), "add the initial flow before requesting exact EF");

    FlowApi denied;
    denied.allowEf = false;
    check(!createVitaEfFlow(denied), "policy/privilege denial is a best-effort failure");
    check(denied.calls == std::vector<int>({1, 2, 3}) && denied.removed == 73,
          "a denied EF request removes its flow instead of retaining another priority");

    FlowApi unavailable;
    unavailable.allowAdd = false;
    check(!createVitaEfFlow(unavailable), "unavailable QoS does not produce a usable flow");
    check(unavailable.calls == std::vector<int>({1}), "a failed add does not touch an unowned flow");
    return g_failed ? 1 : 0;
}
