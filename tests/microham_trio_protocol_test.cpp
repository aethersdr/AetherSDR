// microHAM TRIO protocol tests, read side: parsing, frame kinds and the
// session decision. No socket, no clock.
//
// Every state fixture is a 995-byte frame as one controller sent it. Four were
// recorded as raw bytes on a held session; the four with the all-zero id were
// recorded parsed and are compact re-serialisations, byte-identical to raw
// frames of the same state except for the id. Sizes are asserted first.
//
// Run:  ./build/microham_trio_protocol_test

#include "core/MicrohamTrioProtocol.h"

#include <QByteArray>
#include <QLatin1String>
#include <QString>
#include <QVector>

#include <cstdio>
#include <string>

using namespace AetherSDR;
using namespace AetherSDR::MicrohamTrio;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-66s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// Focus on radio2 "HL2", antenna ant6 "Dummy".
const QByteArray kHl2OnDummy(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant6","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");

// Focus on radio1 "FT710", antenna ant6.
const QByteArray kFt710OnDummy(
    R"({"s":{"radio":{"v":"radio1","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant6","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");

// Focus on radio2, antenna ant2 "Fan Dipole".
const QByteArray kHl2OnFanDipole(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");

// Focus on radio2, antenna ant1 "EFHW".
const QByteArray kHl2OnEfhw(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant1","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");

// The one reply to the antenna command with id 2d00ea67: radio2, ant1.
const QByteArray kReplyAnt1(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant1","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"2d00ea67-27bd-4e3f-8bc6-8408674d7a59"})");

// The one reply to the radio command with id 5422cb17: radio2, ant1.
const QByteArray kReplyRadio2(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant1","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"5422cb17-999b-4381-af7c-4e86d872ab9f"})");

// Pushed 8 s later when band data moved the antenna to ant2; it carries 5422cb17.
const QByteArray kPushBandData(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"5422cb17-999b-4381-af7c-4e86d872ab9f"})");

// Pushed 400 s later for a front-panel RADIO press to radio3; still 5422cb17.
const QByteArray kPushFrontPanel(
    R"({"s":{"radio":{"v":"radio3","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},)"
    R"("radio3":{"n":"RADIO3","a":"R3","l":"SWR","e":true}}},)"
    R"("split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split","e":true}}},)"
    R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW","e":true},)"
    R"("ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},)"
    R"("ant4":{"n":"ANTENNA4","a":"A4","l":"Antenna4","e":true},)"
    R"("ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5","e":true},)"
    R"("ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},)"
    R"("rxant":{"v":"rx1","b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},)"
    R"("rx2":{"n":"RX2","a":"RX2","l":"RX2","e":true},)"
    R"("rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},)"
    R"("rxext":{"v":false,"b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"5422cb17-999b-4381-af7c-4e86d872ab9f"})");

// d184 (2026-10-08), raw frames pushed while ON8ST worked the front panel.
// Split on, TX ant1 "EFHW", RX ant2 "Fan Dipole" (frame at t=166.1 s).
const QByteArray kD184SplitEfhwFan(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},"radio3":{"n":"RADIO3","a":"R3",)"
    R"("l":"SWR","e":true}}},"split":{"v":true,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split",)"
    R"("e":true}}},"pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"txv":"tx_ant1","rxv":"rx_ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1",)"
    R"("l":"EFHW","e":true},"ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},"ant4":{"n":"ANTENNA4",)"
    R"("a":"A4","l":"Antenna4","e":true},"ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5",)"
    R"("e":true},"ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},"rxant":{"v":"rx1",)"
    R"("b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},"rx2":{"n":"RX2","a":"RX2",)"
    R"("l":"RX2","e":true},"rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},"rxext":{"v":false,)"
    R"("b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");
// Split on, TX ant6 "Dummy", RX ant2 (t=172.1 s).
const QByteArray kD184SplitDummyFan(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},"radio3":{"n":"RADIO3","a":"R3",)"
    R"("l":"SWR","e":true}}},"split":{"v":true,"b":{"split":{"n":"SPLIT","a":"SPL","l":"Split",)"
    R"("e":true}}},"pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)"
    R"("antenna":{"txv":"tx_ant6","rxv":"rx_ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1",)"
    R"("l":"EFHW","e":true},"ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},"ant4":{"n":"ANTENNA4",)"
    R"("a":"A4","l":"Antenna4","e":true},"ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5",)"
    R"("e":true},"ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},"rxant":{"v":"rx1",)"
    R"("b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},"rx2":{"n":"RX2","a":"RX2",)"
    R"("l":"RX2","e":true},"rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},"rxext":{"v":false,)"
    R"("b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");
// Split off again: `v` is back, on the former RX half (t=174.5 s).
const QByteArray kD184SplitOff(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},"radio3":{"n":"RADIO3","a":"R3",)"
    R"("l":"SWR","e":true}}},"split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL",)"
    R"("l":"Split","e":true}}},"pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA",)"
    R"("e":true}}},"antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW",)"
    R"("e":true},"ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},"ant4":{"n":"ANTENNA4",)"
    R"("a":"A4","l":"Antenna4","e":true},"ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5",)"
    R"("e":true},"ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},"rxant":{"v":"rx1",)"
    R"("b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},"rx2":{"n":"RX2","a":"RX2",)"
    R"("l":"RX2","e":true},"rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},"rxext":{"v":false,)"
    R"("b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");
// RX antenna RX2 selected, main antenna untouched (t=151.2 s).
const QByteArray kD184RxAnt2(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},"radio3":{"n":"RADIO3","a":"R3",)"
    R"("l":"SWR","e":true}}},"split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL",)"
    R"("l":"Split","e":true}}},"pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA",)"
    R"("e":true}}},"antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW",)"
    R"("e":true},"ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},"ant4":{"n":"ANTENNA4",)"
    R"("a":"A4","l":"Antenna4","e":true},"ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5",)"
    R"("e":true},"ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},"rxant":{"v":"rx2",)"
    R"("b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},"rx2":{"n":"RX2","a":"RX2",)"
    R"("l":"RX2","e":true},"rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},"rxext":{"v":false,)"
    R"("b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");
// RX3 with RX EXT on (t=154.1 s).
const QByteArray kD184Rx3RxExt(
    R"({"s":{"radio":{"v":"radio2","b":{"radio1":{"n":"RADIO1","a":"R1","l":"FT710","e":true},)"
    R"("radio2":{"n":"RADIO2","a":"R2","l":"HL2","e":true},"radio3":{"n":"RADIO3","a":"R3",)"
    R"("l":"SWR","e":true}}},"split":{"v":false,"b":{"split":{"n":"SPLIT","a":"SPL",)"
    R"("l":"Split","e":true}}},"pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA",)"
    R"("e":true}}},"antenna":{"v":"ant2","b":{"ant1":{"n":"ANTENNA1","a":"A1","l":"EFHW",)"
    R"("e":true},"ant2":{"n":"ANTENNA2","a":"A2","l":"Fan Dipole","e":true},)"
    R"("ant3":{"n":"ANTENNA3","a":"A3","l":"Antenna3","e":true},"ant4":{"n":"ANTENNA4",)"
    R"("a":"A4","l":"Antenna4","e":true},"ant5":{"n":"ANTENNA5","a":"A5","l":"Antenna5",)"
    R"("e":true},"ant6":{"n":"ANTENNA6","a":"A6","l":"Dummy","e":true}}},"rxant":{"v":"rx3",)"
    R"("b":{"rx1":{"n":"RX1","a":"RX1","l":"RX1","e":true},"rx2":{"n":"RX2","a":"RX2",)"
    R"("l":"RX2","e":true},"rx3":{"n":"RX3","a":"RX3","l":"RX3","e":true}}},"rxext":{"v":true,)"
    R"("b":{"rxext":{"n":"RXEXT","a":"RxE","l":"RxExt","e":true}}}},)"
    R"("id":"00000000-0000-0000-0000-000000000000"})");

// The two commands that drew kReplyAnt1 and kReplyRadio2, as sent.
const QByteArray kSentAntennaCommand(
    R"({"a": "set_value", "gid": "antenna", "v": "ant1", )"
    R"("id": "2d00ea67-27bd-4e3f-8bc6-8408674d7a59"})");
const QByteArray kSentRadioCommand(
    R"({"a": "set_value", "gid": "radio", "v": "radio2", )"
    R"("id": "5422cb17-999b-4381-af7c-4e86d872ab9f"})");
const QString kAntennaRequestId = QStringLiteral("2d00ea67-27bd-4e3f-8bc6-8408674d7a59");
const QString kRadioRequestId   = QStringLiteral("5422cb17-999b-4381-af7c-4e86d872ab9f");

// The first bytes of the HTML error page the controller sends in a frame when
// an upgrade races its own teardown.
const QByteArray kHtmlErrorPage("<html><bod");

// The keep-alive pair and the idle frame as recorded on a held session: text
// `ping` out, text `pong` (first byte 0x81, 4 bytes) back, and an empty frame
// with first byte 0x8a about once a second.
const QByteArray kPing("ping");
const QByteArray kPong("pong");
const quint8 kRecordedTextByte = 0x81;
const quint8 kRecordedIdleByte = 0x8a;

const QString kRequestId = QStringLiteral("3f2b8c1e-5d4a-4e6f-9a7b-0c1d2e3f4a5b");
const QString kOtherId   = QStringLiteral("11111111-2222-4333-8444-555555555555");

// `fixture` with exactly one occurrence of `from` replaced; fails the run when
// the edit did not land, so a derived fixture cannot silently equal its source.
QByteArray edited(const QByteArray& fixture, const QByteArray& from, const QByteArray& to)
{
    const int at = fixture.indexOf(from);
    if (at < 0 || fixture.indexOf(from, at + 1) >= 0) {
        report("derived fixture edit is unambiguous", false, from.constData());
        return fixture;
    }
    QByteArray out = fixture;
    out.replace(at, from.size(), to);
    return out;
}

Snapshot snapshotOf(const QByteArray& frame)
{
    const ParseResult result = parse(frame);
    if (!result.snapshot) {
        report("fixture parses", false, result.where.toStdString());
        return Snapshot{};
    }
    return *result.snapshot;
}

bool rejects(const QByteArray& frame, ParseError expected)
{
    const ParseResult result = parse(frame);
    return result.error == expected && !result.snapshot;
}

void testFixturesStillMatchTheRecordings()
{
    for (const QByteArray* fixture : {&kHl2OnDummy, &kFt710OnDummy, &kHl2OnFanDipole,
                                      &kHl2OnEfhw, &kReplyAnt1, &kReplyRadio2,
                                      &kPushBandData, &kPushFrontPanel}) {
        report("state fixture is the 995-byte document", fixture->size() == 995,
               std::to_string(fixture->size()));
    }
}

void testRecordedFormsAgree()
{
    // The same state, once recorded parsed and once as raw bytes, differs in
    // the id alone.
    report("a re-serialised fixture equals the raw frame of that state but for the id",
           edited(kHl2OnEfhw, "00000000-0000-0000-0000-000000000000",
                  "2d00ea67-27bd-4e3f-8bc6-8408674d7a59") == kReplyAnt1);
    report("and so does the band-data push",
           edited(kHl2OnFanDipole, "00000000-0000-0000-0000-000000000000",
                  "5422cb17-999b-4381-af7c-4e86d872ab9f") == kPushBandData);
}

void testParseRealState()
{
    const ParseResult result = parse(kHl2OnDummy);
    report("a recorded state parses", result.error == ParseError::None && result.snapshot);
    if (!result.snapshot) {
        return;
    }
    const Snapshot& s = *result.snapshot;
    report("focus is radio2", s.radio.selected == QLatin1String("radio2"),
           s.radio.selected.toStdString());
    report("three radio buttons", s.radio.buttons.size() == 3);
    report("the focused radio carries the operator's label",
           s.radio.selectedButton() && s.radio.selectedButton()->label == QLatin1String("HL2"));
    report("antenna is ant6", s.antenna.selected == QLatin1String("ant6"));
    report("six antenna buttons", s.antenna.buttons.size() == 6);
    const Button* ant2 = s.antenna.find(QStringLiteral("ant2"));
    report("a label with a space survives",
           ant2 && ant2->label == QLatin1String("Fan Dipole")
               && ant2->name == QLatin1String("ANTENNA2")
               && ant2->abbreviation == QLatin1String("A2") && ant2->enabled);
    report("rx antenna is rx1 of three",
           s.rxAntenna.selected == QLatin1String("rx1") && s.rxAntenna.buttons.size() == 3);
    report("pa loop and rx extension are off", !s.pa.on && !s.rxExt.on);
    report("a push on connect carries the unsolicited id",
           s.id == QLatin1String(kUnsolicitedId));
    report("a snapshot is the controller's echo, nothing more",
           s.evidence == PositionEvidence::ControllerEcho);
}

void testParseEveryRecordedPosition()
{
    const Snapshot ft710 = snapshotOf(kFt710OnDummy);
    report("focus on radio1 reads as FT710",
           ft710.radio.selected == QLatin1String("radio1") && ft710.radio.selectedButton()
               && ft710.radio.selectedButton()->label == QLatin1String("FT710")
               && ft710.antenna.selected == QLatin1String("ant6"));
    const Snapshot fan = snapshotOf(kHl2OnFanDipole);
    report("ant2 reads as Fan Dipole",
           fan.antenna.selectedButton()
               && fan.antenna.selectedButton()->label == QLatin1String("Fan Dipole"));
    const Snapshot efhw = snapshotOf(kHl2OnEfhw);
    report("ant1 reads as EFHW",
           efhw.antenna.selectedButton()
               && efhw.antenna.selectedButton()->label == QLatin1String("EFHW"));
}

void testRejectsWhatIsNotAStateDocument()
{
    report("a zero-length frame is Empty", rejects(QByteArray{}, ParseError::Empty));
    report("the HTML error page is not JSON", rejects(kHtmlErrorPage, ParseError::NotJson));
    report("an array is not an object", rejects(QByteArray("[1,2]"), ParseError::NotAnObject));
    report("trailing bytes after the document are refused",
           rejects(kHl2OnDummy + QByteArray("x"), ParseError::NotJson));

    bool everyCutRefused = true;
    int firstAccepted = -1;
    for (int cut = 1; cut < kHl2OnDummy.size(); ++cut) {
        const ParseResult result = parse(kHl2OnDummy.left(cut));
        if (result.snapshot || result.error == ParseError::None) {
            everyCutRefused = false;
            firstAccepted = cut;
            break;
        }
    }
    report("a document truncated at any byte is refused", everyCutRefused,
           everyCutRefused ? std::string{} : "accepted at " + std::to_string(firstAccepted));

    QByteArray big = kHl2OnDummy;
    big.append(QByteArray(kMaxFrameBytes, ' '));
    report("a frame over the cap is refused before parsing",
           rejects(big, ParseError::TooLarge));
    QByteArray atCap = kHl2OnDummy;
    atCap.append(QByteArray(kMaxFrameBytes - atCap.size(), ' '));
    report("a frame exactly at the cap still parses", parse(atCap).snapshot.has_value());
}

void testRejectsASchemaThatMoved()
{
    report("missing id", rejects(edited(kHl2OnDummy,
               R"(,"id":"00000000-0000-0000-0000-000000000000")", ""), ParseError::MissingKey));
    report("id of the wrong type", rejects(edited(kHl2OnDummy,
               R"("id":"00000000-0000-0000-0000-000000000000")", R"("id":7)"),
               ParseError::WrongType));
    report("an extra top-level key", rejects(edited(kHl2OnDummy, R"({"s":)", R"({"t":1,"s":)"),
               ParseError::UnexpectedKey));
    report("s of the wrong type",
           rejects(QByteArray(R"({"s":[],"id":"x"})"), ParseError::WrongType));
    report("a missing group", rejects(edited(kHl2OnDummy,
               R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}},)", ""),
               ParseError::MissingKey));
    report("an unknown group", rejects(edited(kHl2OnDummy, R"("rxext":{"v":false)",
               R"("arxc1":{"v":false,"b":{}},"rxext":{"v":false)"), ParseError::UnexpectedKey));
    report("a group that is not an object",
           rejects(edited(kHl2OnDummy,
               R"("pa":{"v":false,"b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}}})",
               R"("pa":false)"), ParseError::WrongType));
    report("a group with an extra key", rejects(edited(kHl2OnDummy, R"("antenna":{"v":"ant6",)",
               R"("antenna":{"v":"ant6","txv":"ant1",)"), ParseError::UnexpectedKey));
    report("a selector whose value is a boolean", rejects(edited(kHl2OnDummy,
               R"("antenna":{"v":"ant6")", R"("antenna":{"v":true)"), ParseError::WrongType));
    report("a toggle whose value is a string", rejects(edited(kHl2OnDummy,
               R"("pa":{"v":false)", R"("pa":{"v":"false")"), ParseError::WrongType));
    report("a selection that names no button", rejects(edited(kHl2OnDummy,
               R"("antenna":{"v":"ant6")", R"("antenna":{"v":"ant7")"),
               ParseError::SelectionNotAButton));
    report("buttons of the wrong type", rejects(edited(kHl2OnDummy,
               R"("b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}})", R"("b":[])"),
               ParseError::WrongType));
    report("a group with no buttons", rejects(edited(kHl2OnDummy,
               R"("b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}})", R"("b":{})"),
               ParseError::NoButtons));
    report("a button that is not an object", rejects(edited(kHl2OnDummy,
               R"("pa":{"n":"PA","a":"PA","l":"PA","e":true})", R"("pa":1)"),
               ParseError::WrongType));
    report("a button missing its label", rejects(edited(kHl2OnDummy,
               R"("a":"A6","l":"Dummy",)", R"("a":"A6",)"), ParseError::MissingKey));
    report("a button with an extra field", rejects(edited(kHl2OnDummy,
               R"("l":"Dummy","e":true)", R"("l":"Dummy","e":true,"p":1)"),
               ParseError::UnexpectedKey));
    report("a label that is a number", rejects(edited(kHl2OnDummy,
               R"("l":"Dummy")", R"("l":6)"), ParseError::WrongType));
    report("an enabled flag that is a string", rejects(edited(kHl2OnDummy,
               R"("l":"Dummy","e":true)", R"("l":"Dummy","e":"true")"),
               ParseError::WrongType));
    report("split engaged with `v` and no halves is unreadable", rejects(edited(kHl2OnDummy,
               R"("split":{"v":false)", R"("split":{"v":true)"), ParseError::MissingKey));

    // `extra` buttons beside the real one in the pa group.
    auto paGroupWith = [](int extra) {
        QByteArray buttons = R"("b":{)";
        for (int i = 0; i < extra; ++i) {
            buttons += "\"x" + QByteArray::number(i) + R"(":{"n":"N","a":"A","l":"L","e":true},)";
        }
        buttons += R"("pa":{"n":"PA","a":"PA","l":"PA","e":true}})";
        return edited(kHl2OnDummy, R"("b":{"pa":{"n":"PA","a":"PA","l":"PA","e":true}})", buttons);
    };
    report("exactly the button cap still parses",
           parse(paGroupWith(kMaxButtonsPerGroup - 1)).snapshot.has_value());
    report("one button over the cap is refused",
           rejects(paGroupWith(kMaxButtonsPerGroup), ParseError::TooManyButtons));

    const ParseResult located = parse(edited(kHl2OnDummy, R"("l":"Dummy")", R"("l":6)"));
    report("the failure names the element",
           located.where == QLatin1String("s.antenna.b.ant6.l"), located.where.toStdString());
}

void testDisabledButtonIsCarried()
{
    const Snapshot s = snapshotOf(edited(kHl2OnDummy,
        R"("l":"Antenna3","e":true)", R"("l":"Antenna3","e":false)"));
    const Button* ant3 = s.antenna.find(QStringLiteral("ant3"));
    report("a disabled button parses as disabled", ant3 && !ant3->enabled);
}

void testKeepAliveAndFrameKinds()
{
    report("the keep-alive is the four bytes ping", encodeKeepAlive() == kPing
               && encodeKeepAlive().size() == 4);
    report("the recorded frame bytes are the ones classified",
           kTextFrame == kRecordedTextByte && kPongFrame == kRecordedIdleByte);

    report("text pong is the keep-alive reply",
           classify(kRecordedTextByte, kPong).kind == FrameKind::KeepAliveReply);
    report("the empty 0x8a frame is a server pong",
           classify(kRecordedIdleByte, QByteArray{}).kind == FrameKind::ServerPong);

    report("quoted pong is not the reply",
           classify(kRecordedTextByte, QByteArray("\"pong\"")).kind == FrameKind::Unknown);
    report("PONG is not the reply",
           classify(kRecordedTextByte, QByteArray("PONG")).kind == FrameKind::Unknown);
    report("an empty text frame is unknown",
           classify(kRecordedTextByte, QByteArray{}).kind == FrameKind::Unknown);
    report("a pong frame with a payload is unknown",
           classify(kRecordedIdleByte, kPong).kind == FrameKind::Unknown);
    report("pong in a binary frame is unknown", classify(0x82, kPong).kind == FrameKind::Unknown);
    report("pong in an unfinished text frame is unknown",
           classify(0x01, kPong).kind == FrameKind::Unknown);
    report("a state in a pong frame is unknown",
           classify(kRecordedIdleByte, kHl2OnEfhw).kind == FrameKind::Unknown);
    const Frame html = classify(kRecordedTextByte, kHtmlErrorPage);
    report("the HTML error page is unknown and says why",
           html.kind == FrameKind::Unknown && html.state.error == ParseError::NotJson);
    const Frame split = classify(kRecordedTextByte, edited(kHl2OnDummy,
        R"("split":{"v":false)", R"("split":{"v":true)"));
    report("an unreadable state is unknown and says why",
           split.kind == FrameKind::Unknown && split.state.error == ParseError::MissingKey);
}

void testD184Split()
{
    report("d184 split frames are 1015 bytes, the others 995 and 994",
           kD184SplitEfhwFan.size() == 1015 && kD184SplitDummyFan.size() == 1015
               && kD184SplitOff.size() == 995 && kD184RxAnt2.size() == 995
               && kD184Rx3RxExt.size() == 994);

    const ParseResult efhw = parse(kD184SplitEfhwFan);
    report("a split state parses", efhw.error == ParseError::None && efhw.snapshot,
           efhw.where.toStdString());
    if (efhw.snapshot) {
        const Snapshot& s = *efhw.snapshot;
        report("split: on, no `v`, TX half ant1, RX half ant2",
               s.split.on && s.antenna.selected.isEmpty() && !s.antenna.selectedButton()
                   && s.antennaTx == QLatin1String("ant1")
                   && s.antennaRx == QLatin1String("ant2"));
        report("split: the six buttons are still read", s.antenna.buttons.size() == 6
               && s.antenna.find(QStringLiteral("ant6"))
               && s.antenna.find(QStringLiteral("ant6"))->label == QLatin1String("Dummy"));
    }
    const Snapshot dummy = snapshotOf(kD184SplitDummyFan);
    report("split with TX on the dummy: TX half ant6, RX half ant2",
           dummy.split.on && dummy.antennaTx == QLatin1String("ant6")
               && dummy.antennaRx == QLatin1String("ant2"));
    const Snapshot off = snapshotOf(kD184SplitOff);
    report("split off: `v` ant2 serves both halves",
           !off.split.on && off.antenna.selected == QLatin1String("ant2")
               && off.antennaTx == QLatin1String("ant2") && off.antennaRx == QLatin1String("ant2"));
    const Snapshot rx2 = snapshotOf(kD184RxAnt2);
    report("rxant is its own group: RX2, main antenna ant2, RX EXT off",
           rx2.rxAntenna.selected == QLatin1String("rx2") && rx2.antennaTx == QLatin1String("ant2")
               && !rx2.rxExt.on);
    const Snapshot rx3 = snapshotOf(kD184Rx3RxExt);
    report("RX3 with RX EXT on", rx3.rxAntenna.selected == QLatin1String("rx3") && rx3.rxExt.on);

    report("split with `v` beside the halves is refused", rejects(edited(kD184SplitEfhwFan,
               R"("txv":"tx_ant1",)", R"("v":"ant1","txv":"tx_ant1",)"),
               ParseError::UnexpectedKey));
    report("split without its RX half is refused", rejects(edited(kD184SplitEfhwFan,
               R"(,"rxv":"rx_ant2")", ""), ParseError::MissingKey));
    report("a half without its prefix is refused", rejects(edited(kD184SplitEfhwFan,
               R"("tx_ant1")", R"("ant1")"), ParseError::SelectionNotAButton));
    report("a half with the other half's prefix is refused", rejects(edited(kD184SplitEfhwFan,
               R"("tx_ant1")", R"("rx_ant1")"), ParseError::SelectionNotAButton));
    report("a half naming no button is refused", rejects(edited(kD184SplitEfhwFan,
               R"("rx_ant2")", R"("rx_ant7")"), ParseError::SelectionNotAButton));
    report("a half that is not a string is refused", rejects(edited(kD184SplitEfhwFan,
               R"("tx_ant1")", "1"), ParseError::WrongType));
    report("halves without split are refused", rejects(edited(kD184SplitEfhwFan,
               R"("split":{"v":true)", R"("split":{"v":false)"), ParseError::MissingKey));
    const ParseResult located = parse(edited(kD184SplitEfhwFan, R"("rx_ant2")", R"("ant2")"));
    report("the failure names the half", located.where == QLatin1String("s.antenna.rxv"),
           located.where.toStdString());
}

// Session held and alive, its state received.
State goodState()
{
    State state;
    state.link               = Link::Held;
    state.lastMessageAgeMs   = 400;
    state.lastKeepAliveAgeMs = 1000;
    state.lastConnectAgeMs   = 30000;
    state.hasSnapshot        = true;
    return state;
}

bool is(const SessionDecision& d, SessionAction action, SessionReason reason, qint64 waitMs)
{
    return d.action == action && d.reason == reason && d.waitMs == waitMs;
}

void testPredicates()
{
    const Policy policy;
    State s = goodState();
    report("a held session that just spoke is alive", !sessionDead(s));
    s.lastMessageAgeMs = policy.sessionDeadAfterMs;
    report("silent for exactly the bound is still alive", !sessionDead(s));
    s.lastMessageAgeMs = policy.sessionDeadAfterMs + 1;
    report("silent past the bound is dead", sessionDead(s));
    s.lastMessageAgeMs = -1;
    report("a negative message age is dead", sessionDead(s));
    s.link = Link::Evicted;
    report("only a held session can be dead", !sessionDead(s));

    report("the defaults: keep-alive 5 s, lost after 5 s, backoff 5 s up to 10 min",
           policy.keepAliveIntervalMs == 5000 && policy.sessionDeadAfterMs == 5000
               && policy.failureBackoffMs == 5000 && policy.maxBackoffMs == 600000);
    s = goodState();
    s.unansweredKeepAliveAgeMs = policy.sessionDeadAfterMs;
    report("a keep-alive unanswered for exactly the bound: alive", !sessionDead(s));
    s.unansweredKeepAliveAgeMs = policy.sessionDeadAfterMs + 1;
    report("a keep-alive unanswered past the bound: dead, idle frames or not", sessionDead(s));
    s.unansweredKeepAliveAgeMs = -1;
    report("a negative unanswered age is dead", sessionDead(s));
    s.link = Link::Failed;
    report("an unanswered keep-alive on no session is not a dead session", !sessionDead(s));
}

void testSessionKeepAliveAndLiveness()
{
    const Policy policy;
    State s;
    report("never connected: connect",
           is(decideSession(s), SessionAction::Connect, SessionReason::FirstConnect, 0));

    s = goodState();
    report("held, keep-alive not due: wait the remainder", is(decideSession(s),
               SessionAction::Wait, SessionReason::KeepAliveNotDue,
               policy.keepAliveIntervalMs - 1000));
    s.lastKeepAliveAgeMs = policy.keepAliveIntervalMs - 1;
    report("one millisecond before the keep-alive: wait",
           is(decideSession(s), SessionAction::Wait, SessionReason::KeepAliveNotDue, 1));
    s.lastKeepAliveAgeMs = policy.keepAliveIntervalMs;
    report("at the interval: send a keep-alive",
           is(decideSession(s), SessionAction::SendKeepAlive, SessionReason::KeepAliveDue, 0));
    s.lastKeepAliveAgeMs.reset();
    report("none sent yet on this session: send one",
           is(decideSession(s), SessionAction::SendKeepAlive, SessionReason::KeepAliveDue, 0));
    s.lastKeepAliveAgeMs = -1;
    report("a negative keep-alive age: send one",
           is(decideSession(s), SessionAction::SendKeepAlive, SessionReason::KeepAliveDue, 0));

    s = goodState();
    s.lastMessageAgeMs = policy.sessionDeadAfterMs;
    report("silent for exactly the bound: still the session", is(decideSession(s),
               SessionAction::Wait, SessionReason::KeepAliveNotDue,
               policy.keepAliveIntervalMs - 1000));
    s.lastMessageAgeMs = policy.sessionDeadAfterMs + 1;
    report("silent past the bound: dead, connect again",
           is(decideSession(s), SessionAction::Connect, SessionReason::SessionDead, 0));

    s = goodState();
    s.unansweredKeepAliveAgeMs = policy.sessionDeadAfterMs + 1;
    report("a keep-alive unanswered past the bound: dead, connect again",
           is(decideSession(s), SessionAction::Connect, SessionReason::SessionDead, 0));

    s = goodState();
    s.hasSnapshot = false;
    report("held but no state yet: wait for it",
           is(decideSession(s), SessionAction::Wait, SessionReason::AwaitingState, 0));
}

// The device serves one session. An eviction is never retaken, however long
// ago: connecting would evict the other client in turn.
void testEvictionAndRelease()
{
    const Policy policy;
    State s;
    s.link = Link::Evicted;
    s.lastConnectAgeMs = 0;
    report("just evicted: wait, with no time set",
           is(decideSession(s), SessionAction::Wait, SessionReason::Evicted, 0));
    s.lastConnectAgeMs = 3600000;
    report("an hour later: still wait",
           is(decideSession(s), SessionAction::Wait, SessionReason::Evicted, 0));
    s.consecutiveFailures = 5;
    report("whatever came before: still wait",
           is(decideSession(s), SessionAction::Wait, SessionReason::Evicted, 0));

    s = goodState();
    s.releasedForBrowser = true;
    report("released while held: disconnect",
           is(decideSession(s), SessionAction::Disconnect, SessionReason::Released, 0));
    s.link = Link::NeverConnected;
    report("released and not held: wait",
           is(decideSession(s), SessionAction::Wait, SessionReason::Released, 0));

    s = State{};
    s.link = Link::Failed;
    s.consecutiveFailures = 1;
    s.lastConnectAgeMs = 1000;
    report("failed once: wait out the backoff", is(decideSession(s), SessionAction::Wait,
               SessionReason::BackoffAfterFailure, policy.failureBackoffMs - 1000));
    s.lastConnectAgeMs = policy.failureBackoffMs;
    report("then retry", is(decideSession(s), SessionAction::Connect,
               SessionReason::RetryAfterFailure, 0));
    s.consecutiveFailures = 3;
    report("three failures: the backoff doubled twice", is(decideSession(s), SessionAction::Wait,
               SessionReason::BackoffAfterFailure, 3 * policy.failureBackoffMs));
    s.consecutiveFailures = 30;
    s.lastConnectAgeMs = 0;
    report("never more than the cap", is(decideSession(s), SessionAction::Wait,
               SessionReason::BackoffAfterFailure, policy.maxBackoffMs));
}

} // namespace

int main()
{
    testFixturesStillMatchTheRecordings();
    testRecordedFormsAgree();
    testParseRealState();
    testParseEveryRecordedPosition();
    testRejectsWhatIsNotAStateDocument();
    testRejectsASchemaThatMoved();
    testDisabledButtonIsCarried();
    testKeepAliveAndFrameKinds();
    testPredicates();
    testSessionKeepAliveAndLiveness();
    testEvictionAndRelease();
    testD184Split();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
