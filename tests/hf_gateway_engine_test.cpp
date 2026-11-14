#include "core/hfgateway/HostPortEngine.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::HfGateway;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

}  // namespace

// Stage 1 is receive-only by construction (RFC #7296). These checks pin the
// structural guarantees without opening a socket: an engine starts without
// transmit permission, has no API that grants it, and Monitor is offered for
// Mercury only.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    for (const Profile p : {Profile::Mercury, Profile::Data2gHost, Profile::VaraCompatible}) {
        EngineConfig c;
        c.id = toString(p);
        c.profile = p;
        HostPortEngine engine(c);
        report("engine starts without transmit permission", !engine.transmitPermitted());
        report("engine starts disconnected", engine.state() == AetherSDR::ModemHost::Client::State::Disconnected);
        engine.setMonitor(true);
        report("monitor toggling never grants permission", !engine.transmitPermitted());
        report("monitor only for Mercury", engine.monitorSupported() == (p == Profile::Mercury)
                                               && engine.monitor() == (p == Profile::Mercury));
        engine.disconnectFromModem();
        report("disconnect leaves permission off", !engine.transmitPermitted());
    }

    if (g_failed != 0) {
        std::printf("\n%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
