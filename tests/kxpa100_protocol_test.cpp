#include "core/Kxpa100Protocol.h"
#include <cstdio>
#include <cmath>
using namespace AetherSDR::Kxpa100;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main()
{
    FrameParser parser;
    QList<QByteArray> frames;
    parser.setCallback([&](const QByteArray& frame) { frames.append(frame); });
    parser.feed("\r\n^IKXP"); parser.feed("A100;^OP1;^TU1;^MDB;^BYB;^AN2;^AT2;^TP0;");
    CHECK(frames.size() == 8 && frames.front() == "^IKXPA100");
    Status s;
    for (const QByteArray& frame : frames) { CHECK(applyStatus(frame, s)); }
    CHECK(s.identity == "KXPA100" && s.operate == true && s.tunerPresent == true);
    CHECK(s.mode == Mode::Bypass && s.bypassed == true && s.antenna == 2 && s.attenuator == 2 && s.tuning == false);
    CHECK(applyStatus("^PI0012", s) && s.inputWatts == 1.2);
    CHECK(applyStatus("^PF0500", s) && s.forwardWatts == 50.0);
    CHECK(applyStatus("^PV0011", s) && s.reflectedWatts == 1.1);
    CHECK(applyStatus("^PD0123", s) && s.dissipatedWatts == 12.3);
    CHECK(applyStatus("^PC0023", s) && s.drainAmps == 2.3);
    CHECK(applyStatus("^SV13287", s) && s.supplyVolts == 13.287);
    CHECK(applyStatus("^TM0315", s) && s.temperatureC == 31.5);
    CHECK(applyStatus("^SW010", s) && s.swr == 1.0);
    CHECK(applyStatus("^SW013", s) && s.swr == 1.3);
    CHECK(applyStatus("^SW01.0", s) && s.swr == 1.0);
    CHECK(applyStatus("^SW01.3", s) && s.swr == 1.3);
    CHECK(applyStatus("^SW00.0", s) && s.swr == 0.0);
    CHECK(applyStatus("^SW00.1", s) && s.swr == 0.1);
    CHECK(applyStatus("^SW99.9", s) && s.swr == 99.9);
    CHECK(applyStatus("^SW999", s) && s.swr == 99.9);
    for (const QByteArray& invalid : {"^SW009", "^SW1.3", "^SW001.3", "^SW01.30", "^SW01,3",
         "^SW-1.3", "^SW+1.3", "^SW 1.3", "^SW01.-", "^SW01.a", "^SW01.3extra", "^SW1e3"}) {
        CHECK(!applyStatus(invalid, s) && s.swr == 99.9);
        CHECK(!matchesStatusQuery("^SW;", invalid));
    }
    CHECK(matchesStatusQuery("^SW;", "^SW01.3") && matchesStatusQuery("^SW;", "^SW00.0"));
    CHECK(matchesStatusQuery("^SW;", "^SW013"));
    CHECK(applyStatus("^SB012", s) && s.bypassSwr == 1.2);
    CHECK(applyStatus("^F14074", s) && s.frequencyKhz == 14074);
    CHECK(applyStatus("^FLP01234", s) && s.faultCode == 'P' && s.faultDetail == 123.4);
    CHECK(applyStatus("^FLN", s) && s.faultCode == 'N');
    CHECK(!applyStatus("^PF5", s) && s.forwardWatts == 50.0);
    CHECK(!applyStatus("^PF-001", s) && !applyStatus("^AN3", s) && !applyStatus("^OP2", s));
    CHECK(!applyStatus("^SWnan", s) && !applyStatus("^RV01.39extra", s));
    CHECK(matchesStatusQuery("^OP;", "^OP1") && !matchesStatusQuery("^F;", "^FLN"));
    frames.clear(); parser.feed(QByteArray(5000, 'A') + ";^OP0;");
    CHECK(frames.size() == 1 && frames[0] == "^OP0");
    CHECK(relayTotal(true, 255) == 2624 && relayTotal(false, 255) == 17370);
    CHECK(relayMaskForValue(true, 32) == 3 && relayTotal(true, relayMaskForValue(true, 31)) == 32);
    CHECK(buildFrequencyCommand("^SM", 14074) == "^SM14074;");
    CHECK(buildFrequencyCommand("^MT", 0).isEmpty() && buildFrequencyCommand("^FT", 14074).isEmpty());
    CHECK(buildErase(15, 0) == "^EM150;" && buildErase(5, 2) == "^EM052;");
    CHECK(buildErase(11, 2).isEmpty() && buildErase(5, 3).isEmpty());
    const auto config = parseConfiguration("^AEA33333333333");
    CHECK(config && (*config)[10] == 3 && !parseConfiguration("^AEA33333333330"));
    CHECK(configurationSet(5, 2) == "^AE052;" && configurationSet(5, 0).isEmpty());
    CHECK(configurationSet(11, 1).isEmpty());
    const auto memory = decodeMemoryBin("^DM F03805 BN01 BIN040 FR03800-03819 AN1 BYN SIT CR0E LRD8 AN2 EMPTY");
    CHECK(memory && memory->matches.size() == 2 && memory->matches[0].capacitorMask == 14 && memory->matches[1].empty);
    CHECK(!decodeMemoryBin("^DM F03805 BN01 BIN040 FR03810-03819 AN1 EMPTY AN2 EMPTY"));
    CHECK(!decodeMemoryBin("^DM F03805 BN11 BIN040 FR03800-03819 AN1 EMPTY AN2 EMPTY"));
    CHECK(!decodeMemoryBin("^DM F03805 BN01 BIN040 FR03800-03819 AN1 EMPTY AN1 EMPTY"));
    frames.clear(); parser.feed("^DM F03805 BN01 BIN040 FR03800-03819\r\nAN1 BYN SIT CR0E LRD8\r\nAN2 EMPTY;");
    CHECK(frames.size() == 1 && decodeMemoryBin(frames[0]));
    return 0;
}
