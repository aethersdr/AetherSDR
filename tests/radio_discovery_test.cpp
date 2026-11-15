#include "core/RadioDiscovery.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <cstdio>

namespace AetherSDR {
Q_LOGGING_CATEGORY(lcDiscovery, "aether.discovery.test", QtWarningMsg)

class RadioDiscoveryParserTest {
public:
    static RadioInfo parse(RadioDiscovery& discovery, const QByteArray& packet)
    {
        return discovery.parseDiscoveryPacket(packet);
    }
};
}

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    RadioDiscovery discovery;
    QByteArray packet("name=GeekJeep model=FLEX-6700 serial=1234-5678 "
                      "version=4.2.18 ip=192.0.2.10 port=4992 status=Available "
                      "nickname=GeekJeep callsign=KI4TTZ inuse=1 mf_enable=1 "
                      "gui_client_stations=AetherSDR");
    packet.append(char(0x7f));
    packet.append("Station gui_client_programs=AetherSDR");
    packet.append(char(0x7f));
    packet.append("Client gui_client_hosts=desk");
    packet.append(char(0x7f));
    packet.append("mac turf_region=USA");
    packet.append('\0');
    packet.append('\0');
    packet.append('\r');

    const RadioInfo info = RadioDiscoveryParserTest::parse(discovery, packet);

    report("trailing discovery padding stripped from turf region",
           info.turfRegion == QStringLiteral("USA"));
    report("DEL separator decoded in GUI client station",
           info.guiClientStations == QStringList{QStringLiteral("AetherSDR Station")});
    report("DEL separator decoded in GUI client program",
           info.guiClientPrograms == QStringList{QStringLiteral("AetherSDR Client")});
    report("DEL separator decoded in GUI client host",
           info.guiClientHosts == QStringList{QStringLiteral("desk mac")});
    report("ordinary discovery fields still parse",
           info.model == QStringLiteral("FLEX-6700")
               && info.nickname == QStringLiteral("GeekJeep")
               && info.callsign == QStringLiteral("KI4TTZ")
               && info.inUse
               && info.multiFlexEnabled);

    report("no bands key -> empty declaration (real Flex radios)",
           info.bands.isEmpty());

    // Radio-declared band set: a gateway presenting non-Flex hardware
    // declares what it can actually tune (validation/dedup against BandDefs
    // happens in RadioModel; discovery just carries the raw value).
    QByteArray gatewayPacket("model=FLEX-6700 serial=GATE9700 nickname=Icom-IC-9700 "
                             "ip=192.0.2.20 port=4992 status=Available "
                             "bands=2m,440,23cm callsign=SDRSIM");
    const RadioInfo gateway = RadioDiscoveryParserTest::parse(discovery, gatewayPacket);

    report("bands= discovery key captured verbatim",
           gateway.bands == QStringLiteral("2m,440,23cm"));
    report("bands= does not disturb neighbouring fields",
           gateway.model == QStringLiteral("FLEX-6700")
               && gateway.nickname == QStringLiteral("Icom-IC-9700")
               && gateway.callsign == QStringLiteral("SDRSIM"));

    // ---- #5594 item 3: capacity keys, and not the availability keys beside them ----
    //
    // A Flex states its capacity outright: observed on a FLEX-8600 running
    // 4.2.20.41343, max_panadapters=4 available_panadapters=4 max_slices=4
    // available_slices=4. `max_*` is hardware and licence and does not move;
    // `available_*` is the FREE count, which falls as any client opens objects.
    // FlexLib keeps all four apart (Discovery.cs:141/154/247/260). Reading the
    // wrong one of a pair is the specific mistake these cases guard against.
    {
        const RadioInfo caps = RadioDiscoveryParserTest::parse(discovery,
            QByteArray("model=FLEX-8600 serial=2125-1213-8600-8895 "
                       "version=4.2.20.41343 ip=192.168.50.100 port=4992 "
                       "status=Available max_panadapters=4 available_panadapters=4 "
                       "max_slices=4 available_slices=4"));
        report("max_panadapters is read", caps.maxPanadapters == 4);
        report("max_slices is read", caps.maxSlices == 4);

        // A busy radio: capacity unchanged, availability down to 1. A parser
        // reading the availability key would report a capacity of 1.
        const RadioInfo busy = RadioDiscoveryParserTest::parse(discovery,
            QByteArray("model=FLEX-6700 max_panadapters=8 available_panadapters=1 "
                       "max_slices=8 available_slices=1"));
        report("a busy radio still declares its full panadapter capacity",
               busy.maxPanadapters == 8);
        report("a busy radio still declares its full slice capacity",
               busy.maxSlices == 8);

        // Older firmware sends neither key.
        const RadioInfo silent = RadioDiscoveryParserTest::parse(discovery,
            QByteArray("model=FLEX-6400 serial=1234 ip=192.168.1.9 port=4992"));
        report("a packet without the capacity keys leaves both at 0",
               silent.maxPanadapters == 0 && silent.maxSlices == 0);
    }

    if (g_failed == 0) {
        std::printf("\nAll %d radio-discovery tests passed.\n", g_total);
        return 0;
    }

    std::printf("\n%d of %d radio-discovery tests failed.\n", g_failed, g_total);
    return 1;
}
