#include <cstdio>
#include "core/Kat500Protocol.h"
#include <cstdlib>
using namespace AetherSDR::Kat500;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main()
{
    FrameParser parser;
    QList<QByteArray> frames;
    parser.setCallback([&](const QByteArray& frame) { frames.append(frame); });
    parser.feed("KA"); parser.feed("T500;kat500;PS1;PSI0;TP0;MDA;BYPB;AMPI1;");
    CHECK(frames.size() == 8 && frames[0] == "KAT500" && frames[1] == "kat500");
    Status status;
    for (const QByteArray& frame : frames) { applyStatus(frame, status); }
    CHECK(status.powerOn == true && status.mode == Mode::Auto && status.bypassed == true);
    CHECK(status.ampKeyInterrupted == true && status.tuning == false);
    CHECK(!applyStatus("PS2", status) && status.powerOn == true);
    CHECK(!applyStatus("VFWD4096", status));
    CHECK(applyStatus("VFWD 4095", status) && status.forwardAdc == 4095);
    CHECK(applyStatus("SN 222", status) && status.serial == "00222");
    frames.clear(); parser.feed(QByteArray(500, 'A') + ";PS0;");
    CHECK(frames.size() == 1 && frames[0] == "PS0");
    CHECK(relayTotal(true, 255) == 2701 && relayTotal(false, 255) == 17370);
    CHECK(relayMaskForValue(true, 30) == 3 && relayMaskForValue(true, 29) == 2);
    const SettingKey key{Setting::SlotsPerBin, 5};
    CHECK(configurationQuery(key) == "AB05;");
    CHECK(configurationSet(key, 6) == "AB056;" && configurationSet(key, 7).isEmpty());
    CHECK(configurationSet({Setting::AutoSwr, 5}, 1.49).isEmpty());
    CHECK(configurationSet({Setting::AutoSwr, 5}, 1.75) == "ST05A1.75;");
    CHECK(configurationValue({Setting::AmpKeyPower}, "AKIP 1500W VFWD 4095") == 1500);
    CHECK(!configurationValue({Setting::EnabledAntenna, 5, 2}, "AE0511"));
    CHECK(buildFrequency(14074) == "F 14074;" && buildFrequency(-1).isEmpty());
    for (const int invalid : {0, 1, 1799, 54001, 99999}) { CHECK(buildFrequency(invalid).isEmpty()); }
    CHECK(buildFrequency(1800) == "F 1800;" && buildFrequency(54000) == "F 54000;");
    const QList<QByteArray> memory{"DM 21040-21059","AN3","SIDEA","C06","L00","VSWRB 2.11","AN2","SIDET","C20","L01","VSWRB 2.11","AN1","BYP","VSWRB 1.00","3 UNUSED"};
    const auto bin = decodeMemoryBin(memory);
    CHECK(bin && bin->matches.size() == 3 && bin->matches[0].capacitorMask == 6 && bin->unused == 3);
    QList<QByteArray> duplicated = memory; duplicated.insert(4,"C07");
    CHECK(!decodeMemoryBin(duplicated));
    QList<QByteArray> incomplete = memory; incomplete.removeLast(); CHECK(!decodeMemoryBin(incomplete));
    CHECK(!matchesStatusQuery("PS;","PSI1") && !matchesStatusQuery("VSWR;","VSWRB 1.50"));
    CHECK(configurationSet({Setting::AmpKeyPower},-1).isEmpty());
    CHECK(configurationQuery({Setting::SlotsPerBin,11}).isEmpty());
    CHECK(configurationValue({Setting::AutoSwr,5},"ST05A 1.50") == 1.5);
    CHECK(buildErase(10,3) == "EM103;" && buildErase(99,0).isEmpty());
    frames.clear(); parser.feed(QByteArray("PS1\0GARBAGE;",12) + "TP0;");
    CHECK(frames.size() == 1 && frames[0] == "TP0");
    return 0;
}
