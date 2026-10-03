// RadioModel hands the power amplifier's handle to MeterModel, so the PGXL's
// AMP meters are the ones whose source index is that handle (FlexLib
// Radio.FindMetersByAmplifier) and a second amplifier's meters stay off it.

#include "models/MeterModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

static MeterDef ampIdMeter(int index, int handle)
{
    MeterDef def;
    def.index = index;
    def.source = "AMP";
    def.sourceIndex = handle;
    def.name = "ID";
    def.unit = "Amps";
    def.low = 0.0;
    def.high = 70.0;
    def.description = "External Meter";
    return def;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    RadioModel model;   // default family is Flex, which decodes amplifier status

    constexpr int kPgxlHandle = 0x16C58EE4;
    constexpr int kOtherAmpHandle = 0x2A7D11C3;
    MeterModel& meters = model.meterModel();
    meters.defineMeter(ampIdMeter(15, kPgxlHandle));
    meters.defineMeter(ampIdMeter(22, kOtherAmpHandle));   // defined last

    model.handleStatusForTest(QStringLiteral("amplifier 0x16C58EE4"),
                              {{"model", "PowerGeniusXL"}, {"ip", "192.168.1.50"},
                               {"state", "IDLE"}});
    check(model.amplifier().handle() == QStringLiteral("0x16C58EE4"),
          "the PGXL status gives the amplifier its handle");

    float amps = -1.0f;
    QObject::connect(&meters, &MeterModel::ampVitalsChanged,
                     [&](float a, bool valid, bool, float, bool, bool) {
                         if (valid) amps = a;
                     });
    // 19.2 A on the PGXL, 5 A on the other amplifier, in one packet.
    meters.updateValues({15, 22}, {qint16(19.2f * 256.0f), qint16(5.0f * 256.0f)});
    check(std::fabs(amps - 19.2f) < 0.01f,
          "the amplifier's drain current comes from its own ID meter");

    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
