// IcomCIV Phases 4 and 5 — metering calibration, the poll scheduler, and the
// per-model capability table.
//
// Pure logic: no sockets, no Qt, no hardware. The scheduler is driven by a
// synthetic clock, which is the only practical way to prove the in-flight and
// user-guard rules.

#include "core/backends/icom/IcomMeters.h"
#include "core/backends/icom/IcomModels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
static bool near(double a, double b, double tol = 0.05)
{
    return std::fabs(a - b) < tol;
}
static bool contains(const std::vector<MeterId>& v, MeterId id)
{
    return std::find(v.begin(), v.end(), id) != v.end();
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

static void testInterpolation()
{
    const std::array<CurvePoint, 3> c{{{0, 0.0}, {100, 10.0}, {200, 12.0}}};
    check(near(interpolateCurve(c, 0), 0.0), "first point");
    check(near(interpolateCurve(c, 50), 5.0), "linear inside the first segment");
    check(near(interpolateCurve(c, 150), 11.0), "linear inside the second segment");
    check(near(interpolateCurve(c, 200), 12.0), "last point");
    // Clamp rather than extrapolate: a raw byte outside the published range is
    // the radio saying "off the scale", not an invitation to invent numbers.
    check(near(interpolateCurve(c, -5), 0.0), "below the table clamps");
    check(near(interpolateCurve(c, 255), 12.0), "above the table clamps");
    check(near(interpolateCurve({}, 42), 0.0), "an empty curve is zero, not a crash");
}

static void testSMeter()
{
    // Icom's published breakpoints, mapped through the IARU convention.
    check(near(sMeterDbm(0, kS9DbmHf), -127.0), "raw 0 is S0 = S9 - 54 dB");
    check(near(sMeterDbm(120, kS9DbmHf), -73.0), "raw 120 is S9 = -73 dBm on HF");
    check(near(sMeterDbm(241, kS9DbmHf), -13.0), "raw 241 is S9 + 60 dB");

    // The reference is BAND-dependent. Using -73 everywhere reports VHF signals
    // 20 dB hot, which on 2 m weak-signal work is the entire usable range.
    check(near(sMeterDbm(120, kS9DbmVhf), -93.0), "S9 is -93 dBm above 30 MHz");
    check(near(sMeterDbm(120, s9ReferenceFor(14'074'000)), -73.0), "20 m uses the HF reference");
    check(near(sMeterDbm(120, s9ReferenceFor(144'174'000)), -93.0), "2 m uses the VHF reference");

    check(near(sMeterDbm(60, kS9DbmHf), -100.0), "S4.5 sits halfway up the lower segment");

    // The two segments have genuinely different slopes — 54 dB over 120 counts
    // below S9, 60 dB over 121 above it. Divergence from a single straight line
    // through the endpoints PEAKS AT S9 ITSELF, at 2.76 dB: not huge, but about
    // half an S-unit, and it lands exactly on the reading operators quote.
    const auto naiveLine = [](int raw) {
        return -127.0 + (raw / 241.0) * (-13.0 - -127.0);
    };
    check(near(std::fabs(sMeterDbm(120, kS9DbmHf) - naiveLine(120)), 2.76, 0.05),
          "a two-point fit is 2.76 dB out at S9 — the worst point on the scale");
    check(std::fabs(sMeterDbm(241, kS9DbmHf) - naiveLine(241)) < 0.01,
          "and exact at the endpoints, which is why an endpoint check would miss it");
}

static void testPowerAndOthers()
{
    // Published breakpoints from Hamlib/flrig.
    check(near(meterValue(MeterId::Power, 143, kS9DbmHf), 5.0), "raw 143 is 5 W");
    check(near(meterValue(MeterId::Power, 213, kS9DbmHf), 10.0), "raw 213 is 10 W (rated)");
    // The curve tops out ABOVE the rated 10 W — the meter has headroom past
    // 100%, and clamping at 10 would misreport a hot final.
    check(near(meterValue(MeterId::Power, 255, kS9DbmHf), 12.0), "raw 255 is 12 W, above rated");
    check(meterValue(MeterId::Power, 240, kS9DbmHf) > 10.0, "and the region between is reachable");

    // Same CI-V addresses, different radio-specific faces.
    check(near(meterValue(MeterId::Power, 143, kS9DbmHf, 0xB6), 50.0),
          "IC-7300MK2 raw 143 is 50 W");
    check(near(meterValue(MeterId::Power, 213, kS9DbmHf, 0xB6), 100.0),
          "IC-7300MK2 raw 213 is its rated 100 W");

    check(near(meterValue(MeterId::Swr, 0, 0), 1.0), "SWR 1.0 at zero");
    check(near(meterValue(MeterId::Swr, 48, 0), 1.5), "SWR 1.5");
    check(near(meterValue(MeterId::Swr, 120, 0), 3.0), "SWR 3.0");
    // Icom's table stops at 3.0 and the field is a full byte. A bad match must
    // read as "very high", not pin at exactly 3.0 — which looks like a working
    // antenna to anyone glancing at the meter.
    check(meterValue(MeterId::Swr, 200, 0) > 3.5, "a severe mismatch reads above 3.0");

    check(near(meterValue(MeterId::Comp, 130, 0), 15.0), "COMP 15 dB");
    check(near(meterValue(MeterId::Vd, 75, 0), 5.0), "Vd 5 V");
    check(near(meterValue(MeterId::Id, 121, 0), 2.0), "Id 2 A");
    check(near(meterValue(MeterId::Vd, 13, 0, 0xB6), 10.0),
          "IC-7300MK2 Vd uses its desktop calibration");
    check(near(meterValue(MeterId::Id, 97, 0, 0xB6), 10.0),
          "IC-7300MK2 Id uses its 25 A face");

    // ALC full scale is 120, NOT 255 — the guide says so. Scaling by 255 makes
    // a fully-driven ALC read 47%.
    check(near(meterValue(MeterId::Alc, 120, 0), 100.0), "ALC full scale is raw 120");
    check(near(meterValue(MeterId::Alc, 60, 0), 50.0), "and half scale is raw 60");

    // OVF is a flag, not a scaled reading.
    check(near(meterValue(MeterId::Overflow, 1, 0), 1.0), "OVF set");
    check(near(meterValue(MeterId::Overflow, 0, 0), 0.0), "OVF clear");
}

static void testSpecs()
{
    const MeterSpec* s = meterSpecForSub(meter::kSMeter);
    check(s && s->id == MeterId::SMeter, "S-meter resolves from its subcommand");
    check(s && s->unit == "dBm", "and reports dBm");
    const MeterSpec* swr = meterSpecFor(MeterId::Swr);
    check(swr && swr->when == MeterWhen::TxOnly, "SWR is transmit-only");
    check(meterSpecForSub(0x99) == nullptr, "an unknown subcommand resolves to nothing");
}

// ---------------------------------------------------------------------------
// The poll scheduler
// ---------------------------------------------------------------------------

static void testPollerVisibilityAndTxSplit()
{
    MeterPoller p;
    check(p.due(1000).empty(), "nothing is polled when nothing is visible");

    p.setVisible(MeterId::SMeter, true);
    auto d = p.due(1000);
    check(contains(d, MeterId::SMeter), "a visible meter becomes due immediately");
    check(d.size() == 1, "and only that one");

    // Rule 2: polling SWR while receiving returns zero, which renders as a
    // meter pinned at the bottom rather than as "not applicable".
    p.setVisible(MeterId::Swr, true);
    p.markAnswered(MeterId::SMeter, 1000);
    auto rx = p.due(2000);
    check(!contains(rx, MeterId::Swr), "a TX-only meter is not polled while receiving");

    p.setTransmitting(true);
    auto tx = p.due(3000);
    check(contains(tx, MeterId::Swr), "and IS polled while transmitting");
    check(!contains(tx, MeterId::SMeter), "while the RX-only S-meter stops");
}

static void testPollerInFlight()
{
    MeterPoller p;
    p.setVisible(MeterId::SMeter, true);

    auto first = p.due(1000);
    check(contains(first, MeterId::SMeter), "asked once");

    // Rule 3. Without this a slow link accumulates duplicate requests and the
    // backlog never drains.
    check(p.due(1100).empty(), "not re-asked while a request is in flight");
    check(p.due(1500).empty(), "still not, well past the interval");

    p.markAnswered(MeterId::SMeter, 1500);
    check(p.due(1500).empty(), "the interval runs from the ANSWER, not the request");
    check(contains(p.due(1600), MeterId::SMeter), "and it comes due once the interval elapses");
}

static void testPollerInFlightTimeout()
{
    MeterPoller p;
    p.setVisible(MeterId::SMeter, true);
    (void)p.due(1000);
    check(p.due(1000 + MeterPoller::kInFlightTimeoutMs - 1).empty(),
          "a pending request is respected right up to the timeout");
    // A lost reply must eventually be re-asked, or the meter dies silently for
    // the rest of the session.
    check(contains(p.due(1000 + MeterPoller::kInFlightTimeoutMs + 1), MeterId::SMeter),
          "a lost reply is re-asked after the in-flight timeout");
}

static void testPollerUserGuard()
{
    MeterPoller p;
    p.setVisible(MeterId::SMeter, true);
    p.setVisible(MeterId::Vd, true);

    // Rule 4: a frequency change that queues behind three meter polls is a VFO
    // knob that feels broken.
    p.noteUserCommand(1000);
    check(p.due(1000).empty(), "metering yields immediately after a user command");
    check(p.due(1000 + MeterPoller::kUserGuardMs - 1).empty(), "for the whole guard window");
    check(!p.due(1000 + MeterPoller::kUserGuardMs + 1).empty(), "and resumes after it");
}

static void testPollerVisibilityIsImmediate()
{
    MeterPoller p;
    p.setVisible(MeterId::SMeter, true);
    (void)p.due(1000);
    p.markAnswered(MeterId::SMeter, 1000);
    p.setVisible(MeterId::SMeter, false);
    check(p.due(5000).empty(), "hidden meters stop costing round trips");

    // Re-showing must not wait out a stale interval — the operator opened the
    // panel and expects a reading, not a second of blank.
    p.setVisible(MeterId::SMeter, true);
    check(contains(p.due(5000), MeterId::SMeter), "re-showing polls immediately");
}

// ---------------------------------------------------------------------------
// Phase 5 — the model table
// ---------------------------------------------------------------------------

static void testModelTable()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 != nullptr, "the IC-705 is in the table");
    check(ic705 && ic705->name == "IC-705", "by name");
    check(ic705 && ic705->verified, "and it is the one model whose numbers are tier-1 verified");
    check(ic705 && ic705->scopePoints == 475 && ic705->scopeMaxAmplitude == 160,
          "475 points, 0..160 — straight from Icom's CI-V guide");
    check(ic705 && ic705->receivers == 1 && ic705->hasWifi, "one receiver, WiFi");
    check(ic705 && ic705->hasVfoModeCommand,
          "and its official guide verifies selected-VFO 26 00 mode/DATA/filter");

    // Geometry genuinely varies, which is why it cannot be a compile-time
    // constant shared across models.
    const IcomModel* ic7610 = modelForCivAddress(0x98);
    check(ic7610 && ic7610->scopePoints == 689 && ic7610->scopeMaxAmplitude == 200,
          "the IC-7610 has a DIFFERENT scope geometry");
    check(ic7610 && !ic7610->verified, "and is honestly marked unverified");
    check(ic7610 && !ic7610->hasVfoModeCommand,
          "so it does not inherit the IC-705's selected-VFO 26 00 shape");

    // The IC-9700 is the case that shows the two claims are independent. Its
    // scope geometry is still ASSUMED from the IC-705, so `verified` stays
    // false — but its 26 00 shape was read off the radio, so the DATA-capable
    // flag is true. Without the flag, selecting FM-D falls to the legacy 06
    // branch, which sends no DATA byte and takes the radio back OUT of data
    // mode — the failure #4931 reported, on the radio that reported it.
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    check(ic9700 != nullptr, "the IC-9700 is in the table");
    check(ic9700 && !ic9700->verified,
          "its geometry is cross-referenced, not confirmed against its own guide");
    check(ic9700 && ic9700->hasVfoModeCommand,
          "yet its measured 26 00 mode/DATA/filter shape is attested independently");

    // Six-byte frequencies. A codec against a hardcoded 5 misaligns by two
    // bytes and decodes a plausible-looking wrong frequency.
    const IcomModel* ic905 = modelForCivAddress(0xAC);
    check(ic905 && ic905->freqBytes == 6, "the IC-905 uses 6-byte frequencies");

    // No RS-BA1 transport — reachable over serial, or via Icom's own server.
    const IcomModel* ic7300 = modelForCivAddress(0x94);
    check(ic7300 && !ic7300->hasNetwork, "the IC-7300 has no network transport");

    // The MK2 is the same radio family with a LAN port bolted on, and that one
    // difference is what puts it in reach of this backend directly rather than
    // through Icom's RS-BA1 server. Verified from its own CI-V guide, whose
    // frame diagram reads FE FE E0 B6.
    const IcomModel* mk2 = modelForCivAddress(0xB6);
    check(mk2 != nullptr, "the IC-7300MK2 is in the table");
    check(mk2 && mk2->name == "IC-7300MK2", "by name");
    check(mk2 && mk2->hasNetwork, "and unlike the original IC-7300 it HAS a network transport");
    check(mk2 && !mk2->hasWifi, "Ethernet, not WiFi — the IC-705 is the WiFi one");
    check(mk2 && mk2->verified, "its numbers came from its own Icom CI-V guide");
    check(mk2 && mk2->hasVfoModeCommand,
          "including its selected-VFO 26 00 mode/DATA/filter command");
    check(mk2 && mk2->scopePoints == 475 && mk2->scopeMaxAmplitude == 160,
          "475 points, 0..160");
    check(mk2 && mk2->tuningMaxHz == 74'800'000ULL, "0.03 to 74.8 MHz");
    check(ic7300 && mk2 && ic7300->civAddress != mk2->civAddress,
          "and it is a DIFFERENT CI-V address from the original — 0x94 vs 0xB6");

    // The MK2's guide states the LAN data length as 490 bytes. That is an
    // independent confirmation of the 15-byte first-division header this
    // decoder computes: 3 + 1 + 5*2 + 1 == 15, and 15 + 475 == 490. If the
    // header maths were ever changed, this arithmetic would stop agreeing with
    // a number Icom published.
    check(15 + mk2->scopePoints == 490, "15-byte header + 475 points == the published 490");

    check(modelForCivAddress(0x01) == nullptr,
          "an unrecognised address resolves to nothing — a normal outcome, not an error");
    check(!knownModels().empty(), "the table is populated");
    // NO MODEL INHERITS THE 26 00 SHAPE BY ASSUMPTION.
    //
    // This started life as `m.verified || !m.hasVfoModeCommand`, which read the
    // right intent off the wrong field. `verified` is a claim about the WHOLE
    // row — scope geometry, amplitude range, tuning limits, all confirmed
    // against that model's own CI-V Reference Guide. The 0x26 shape is a
    // narrower and independent question, and coupling them forced a choice
    // between two dishonest options for a radio whose 0x26 form has been
    // measured but whose scope geometry is still assumed: either overclaim the
    // whole row, or drop a flag that a live trace supports.
    //
    // So attest the flag directly. The hazard the original guarded against is
    // SILENT inheritance — a new row copied from the IC-705 and quietly picking
    // up a command shape nobody checked on that radio. Naming each address here
    // keeps that guard: the flag cannot go true without a deliberate edit to
    // this list, and the comment beside it has to say what the evidence was.
    static const std::array<std::uint8_t, 3> kAttestedVfoMode{
        0xA4,  // IC-705     — its own CI-V Reference Guide documents 26 00
        0xB6,  // IC-7300MK2 — likewise, from its own guide
        0xA2,  // IC-9700    — measured on a live radio, 2026-08-14: 26 00 read
               //              answered 26 00 05 00 01, the same three-byte
               //              mode/DATA/filter body cmdSetVfoMode writes
    };
    check(std::all_of(knownModels().begin(), knownModels().end(), [](const IcomModel& m) {
              return !m.hasVfoModeCommand
                  || std::find(kAttestedVfoMode.begin(), kAttestedVfoMode.end(),
                               m.civAddress)
                      != kAttestedVfoMode.end();
          }),
          "no model silently inherits a DATA command shape");
    // The converse, so the list cannot rot into a permission slip for rows that
    // no longer set the flag.
    check(std::all_of(kAttestedVfoMode.begin(), kAttestedVfoMode.end(),
                      [](std::uint8_t addr) {
                          const IcomModel* m = modelForCivAddress(addr);
                          return m && m->hasVfoModeCommand;
                      }),
          "every attested address is a real model that actually sets the flag");
}

static void testUnknownModelIsConservative()
{
    const IcomModel& u = unknownModel();
    // An unknown radio advertised as scope-capable wires a panadapter to a
    // command it may not implement; advertised as transmit-capable it puts a TX
    // button on something we cannot characterise. Both are worse than a reduced
    // feature set — the operator can still tune and listen.
    check(!u.hasScope, "an unknown radio is NOT advertised as scope-capable");
    check(!u.hasTransmit, "nor as transmit-capable");
    check(u.txPowerMaxWatts == 0.0, "and claims no power");
    check(u.receivers == 1, "one receiver is the safe assumption");
    check(!u.isKnown(), "and it knows it is unknown");
}

static void testModelDiscovery()
{
    // 0x19 0x00 is how the radio names itself. Query it; never assume.
    auto reply = parseFrame(std::vector<std::uint8_t>{0xFE, 0xFE, kControllerAddress, 0xA4,
                                                      cmd::kReadId, 0x00, 0xA4, kCivEom});
    check(reply.has_value(), "the model-id reply parses");
    auto addr = parseModelIdReply(*reply);
    check(addr.has_value() && *addr == 0xA4, "and yields the radio's CI-V address");

    auto notIt = parseFrame(cmdReadFrequency(0xA4));
    check(notIt.has_value() && !parseModelIdReply(*notIt).has_value(),
          "a frequency frame is not a model-id reply");
}

static void testPowerCurveIsNotShared()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    check(ic705 && !powerCurveFor(*ic705).empty(), "the IC-705 has a measured watts curve");
    // Handing back the IC-705's curve for another radio would produce a watts
    // figure an operator would act on, derived from a different PA. Empty means
    // "report percent", which is honest.
    check(ic9700 && powerCurveFor(*ic9700).empty(),
          "another model gets NO curve rather than the IC-705's");
    check(powerCurveFor(unknownModel()).empty(), "and nor does an unknown radio");
}

int main()
{
    testInterpolation();
    testSMeter();
    testPowerAndOthers();
    testSpecs();
    testPollerVisibilityAndTxSplit();
    testPollerInFlight();
    testPollerInFlightTimeout();
    testPollerUserGuard();
    testPollerVisibilityIsImmediate();
    testModelTable();
    testUnknownModelIsConservative();
    testModelDiscovery();
    testPowerCurveIsNotShared();

    if (g_failures == 0)
        std::printf("icom_meters_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
