// IcomCIV Phases 4 and 5 — metering calibration, the poll scheduler, and the
// per-model capability table.
//
// Pure logic: no sockets, no Qt, no hardware. The scheduler is driven by a
// synthetic clock, which is the only practical way to prove the in-flight and
// user-guard rules.

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomMeters.h"
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/IcomControls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

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
    check(near(meterValue(MeterId::Power, 143, kS9DbmHf,
                          MeterCalibration::Ic7300Mk2), 50.0),
          "IC-7300MK2 raw 143 is 50 W");
    check(near(meterValue(MeterId::Power, 213, kS9DbmHf,
                          MeterCalibration::Ic7300Mk2), 100.0),
          "IC-7300MK2 raw 213 is its rated 100 W");
    check(near(meterValue(MeterId::Power, 255, kS9DbmHf,
                          MeterCalibration::Uncalibrated), 100.0),
          "an uncalibrated model reports Po as percent, never borrowed watts");
    check(near(meterValue(MeterId::Vd, 100, 0,
                          MeterCalibration::Uncalibrated), 0.0)
              && near(meterValue(MeterId::Id, 100, 0,
                                 MeterCalibration::Uncalibrated), 0.0),
          "an uncalibrated model fabricates neither voltage nor PA current");

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
    check(near(meterValue(MeterId::Vd, 13, 0, MeterCalibration::Ic7300Mk2), 10.0),
          "IC-7300MK2 Vd uses its desktop calibration");
    check(near(meterValue(MeterId::Id, 97, 0, MeterCalibration::Ic7300Mk2), 10.0),
          "IC-7300MK2 Id uses its 25 A face");
    check(near(meterValue(MeterId::Vd, 185, 0,
                          MeterCalibration::Ic9700), 13.8),
          "IC-9700 Vd uses its model-specific calibration");
    check(near(meterValue(MeterId::Id, 121, 0,
                          MeterCalibration::Ic9700), 10.0),
          "IC-9700 Id midpoint is 10 A");
    check(near(meterValue(MeterId::Id, 241, 0,
                          MeterCalibration::Ic9700), 20.0),
          "IC-9700 Id full scale is 20 A");
    check(near(meterValue(MeterId::Power, 213, 0,
                          MeterCalibration::Ic9700),
               213.0 * 100.0 / 255.0),
          "IC-9700 meter calibration does not borrow another model's watt curve");

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

static void testPolledTxMeterMinimumHold()
{
    MeterPoller p;
    p.setTransmitting(true);

    // With no real sample in this keyed interval, minimum is the only truth we
    // have and must be published rather than replaced with stale state.
    check(p.shouldPublish(MeterId::Swr, 0, 1000, true),
          "the first keyed SWR minimum is published when there is nothing to hold");
    check(p.shouldPublish(MeterId::Alc, 0, 1000, true),
          "the first keyed ALC minimum is published when there is nothing to hold");

    check(p.shouldPublish(MeterId::Swr, 80, 1200, true),
          "a real SWR sample is published immediately");
    check(!p.shouldPublish(MeterId::Swr, 0, 1300, true),
          "an isolated SWR minimum between samples is held");
    check(p.shouldPublish(MeterId::Swr, 82, 1350, true),
          "the next real SWR sample replaces the held placeholder immediately");

    check(p.shouldPublish(MeterId::Alc, 60, 1400, true),
          "a real ALC sample is published immediately");
    check(!p.shouldPublish(MeterId::Alc, 0, 1500, true),
          "an isolated ALC minimum between samples is held");
    check(!p.shouldPublish(MeterId::Alc, 0,
                           1500 + MeterPoller::kMinimumConfirmationMs - 1, true),
          "a minimum remains held for the complete confirmation interval");
    check(p.shouldPublish(MeterId::Alc, 0,
                          1500 + MeterPoller::kMinimumConfirmationMs, true),
          "a sustained ALC minimum becomes authoritative");

    // The policy is deliberately not a family-wide meter smoother. Other
    // readings, including forward power, keep their existing publication.
    check(p.shouldPublish(MeterId::Power, 0, 2000, true),
          "the minimum hold does not alter other Icom meters");

    check(p.shouldPublish(MeterId::Swr, 80, 2050, false)
              && p.shouldPublish(MeterId::Swr, 0, 2100, false),
          "a model without the meter-profile facet keeps every SWR sample");

    (void)p.shouldPublish(MeterId::Swr, 48, 2200, true);
    check(!p.shouldPublish(MeterId::Swr, 0, 2250, true),
          "a keyed SWR minimum can be held before an edge");
    p.setTransmitting(false);
    p.setTransmitting(true);
    check(p.shouldPublish(MeterId::Swr, 0, 2300, true),
          "a new keyed interval never inherits the previous transmission's hold");
}

// ---------------------------------------------------------------------------
// Phase 5 — the model table
// ---------------------------------------------------------------------------

static void testModelTable()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 != nullptr, "the IC-705 is in the table");
    check(ic705 && ic705->name == "IC-705", "by name");
    check(ic705 && ic705->verified, "and its numbers are tier-1 verified");
    check(ic705 && ic705->scopePoints == 475 && ic705->scopeMaxAmplitude == 160,
          "475 points, 0..160 — straight from Icom's CI-V guide");
    check(ic705 && ic705->receivers == 1 && ic705->hasWifi, "one receiver, WiFi");
    check(ic705 && profileFor(*ic705).supports(IcomFeature::VfoMode),
          "and its official guide verifies selected-VFO 26 00 mode/DATA/filter");

    // Geometry genuinely varies, which is why it cannot be a compile-time
    // constant shared across models.
    const IcomModel* ic7610 = modelForCivAddress(0x98);
    check(ic7610 && ic7610->scopePoints == 689 && ic7610->scopeMaxAmplitude == 200,
          "the IC-7610 has a DIFFERENT scope geometry");
    check(ic7610 && !ic7610->verified, "and is honestly marked unverified");
    check(ic7610 && !profileFor(*ic7610).supports(IcomFeature::VfoMode),
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
    check(ic9700 && profileFor(*ic9700).supports(IcomFeature::VfoMode),
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
    check(mk2 && profileFor(*mk2).supports(IcomFeature::VfoMode),
          "including its selected-VFO 26 00 mode/DATA/filter command");
    check(mk2 && mk2->scopePoints == 475 && mk2->scopeMaxAmplitude == 160,
          "475 points, 0..160");
    check(mk2 && mk2->tuningMaxHz == 74'800'000ULL, "0.03 to 74.8 MHz");
    check(ic705 && mk2
              && profileFor(*ic705).meters.holdIsolatedTxMinimums
              && profileFor(*mk2).meters.holdIsolatedTxMinimums,
          "IC-705 and IC-7300MK2 enable their live-proven TX meter minimum hold");
    check(ic9700 && !profileFor(*ic9700).meters.holdIsolatedTxMinimums
              && !profileFor(unknownModel()).meters.holdIsolatedTxMinimums,
          "IC-9700 and unknown Icoms do not borrow the TX meter minimum hold");
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
    if (ic9700) {
        // ONE TABLE, TWO CONSUMERS. The tune guard and the published PA
        // ceilings describe the same three RF decks, so they are read from the
        // same rows — and this is the assertion that keeps them that way. A
        // future edit that widened a range for power but not for tuning (or
        // the reverse) used to be invisible; now it fails here.
        const std::span<const IcomBand> bands = bandsFor(*ic9700);
        check(bands.size() == 3, "the IC-9700 declares three RF decks");
        for (const IcomBand& band : bands) {
            check(supportsFrequency(*ic9700, band.lowHz)
                      && supportsFrequency(*ic9700, band.highHz),
                  "every declared IC-9700 power band is tunable end to end");
            check(band.maxWatts > 0.0,
                  "every declared IC-9700 band carries a PA rating");
        }
        check(bandRatedPowerWatts(*ic9700, 144'000'000ULL) == 100.0
                  && bandRatedPowerWatts(*ic9700, 148'000'000ULL) == 100.0
                  && bandRatedPowerWatts(*ic9700, 430'000'000ULL) == 75.0
                  && bandRatedPowerWatts(*ic9700, 450'000'000ULL) == 75.0
                  && bandRatedPowerWatts(*ic9700, 1'240'000'000ULL) == 10.0
                  && bandRatedPowerWatts(*ic9700, 1'300'000'000ULL) == 10.0,
              "IC-9700 band edges select the correct 100/75/10 W rating");
        check(!bandRatedPowerWatts(*ic9700, 200'000'000ULL)
                  && !bandRatedPowerWatts(*ic9700, 900'000'000ULL),
              "a frequency between IC-9700 RF decks borrows no rating");
        // The other half of the same claim: a continuous model has no table,
        // and that emptiness is what keeps its tune path untouched. #5116
        // names both of these as non-goals.
        for (const std::uint8_t addr : {std::uint8_t(0xA4), std::uint8_t(0xB6)}) {
            const IcomModel* m = modelForCivAddress(addr);
            check(m && bandsFor(*m).empty(),
                  "a continuous model (IC-705, IC-7300MK2) declares no band "
                  "table, so the IC-9700 gate cannot reach it");
        }
        check(nearestSupportedFrequency(*ic9700, 149'000'000ULL) == 148'000'000ULL,
              "an IC-9700 drag above 2 m clamps to the 148 MHz edge");
        check(nearestSupportedFrequency(*ic9700, 500'000'000ULL) == 450'000'000ULL,
              "an IC-9700 drag in the upper gap clamps to the nearest edge");
        check(nearestSupportedFrequency(*ic9700, 1'296'000'000ULL)
                  == 1'296'000'000ULL,
              "an in-band IC-9700 drag remains unchanged");
    }
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
              return !profileFor(m).supports(IcomFeature::VfoMode)
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
                          return m && profileFor(*m).supports(IcomFeature::VfoMode);
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

static void testCapabilityProfiles()
{
    const IcomModel& ic705 = *modelForCivAddress(0xA4);
    const IcomModel& ic9700 = *modelForCivAddress(0xA2);
    const IcomModel& mk2 = *modelForCivAddress(0xB6);
    const IcomModelProfile& p705 = profileFor(ic705);
    const IcomModelProfile& p9700 = profileFor(ic9700);
    const IcomModelProfile& pMk2 = profileFor(mk2);

    check(p705.supportedBringup && p9700.supportedBringup && pMk2.supportedBringup,
          "IC-705, IC-7300MK2 and IC-9700 are the intentional bring-up profiles");
    check(std::ranges::count_if(knownModels(), [](const IcomModel& model) {
              return profileFor(model).supportedBringup;
          }) == 3,
          "exactly three known models have supported command profiles");

    check(p705.setMenu.voxDelayItem == 359 && p705.setMenu.civTransceiveItem == 131,
          "IC-705 owns VOX delay 0359 and CI-V Transceive 0131");
    check(pMk2.setMenu.voxDelayItem == 267 && pMk2.setMenu.civTransceiveItem == 89,
          "IC-7300MK2 owns the distinct VOX delay 0267 and Transceive 0089");
    check(p9700.modulation && p9700.modulation->phoneLevelFollowsNetworkInput,
          "IC-9700 owns its verified LAN modulation-level profile");
    check(p9700.setMenu.voxDelayItem < 0 && p9700.setMenu.civTransceiveItem < 0
              && !p9700.txBandwidth,
          "IC-9700 borrows no unverified SET-menu or TX-bandwidth map");
    check(p705.gps && p705.gps->ntpEnabledItem == 167
              && p705.gps->ntpServerItem == 168
              && p705.gps->timeCorrectItem == 169 && p705.gps->hasNtpAccess,
          "IC-705 owns its GPS/NTP command shape in the model profile");
    check(p705.supports(IcomFeature::GpsPosition)
              && p705.supports(IcomFeature::GpsTimeConfiguration),
          "IC-705 GPS position and clock configuration carry independent evidence");
    check(!p9700.gps && !pMk2.gps
              && !p9700.supports(IcomFeature::GpsPosition)
              && !pMk2.supports(IcomFeature::GpsTimeConfiguration),
          "other supported profiles do not inherit IC-705 GPS commands");

    check(p705.fmRepeater && p705.fmRepeater->dialect == FmRepeaterDialect::Extended
              && p705.fmRepeater->hasDtcs,
          "IC-705 profile retains official-guide extended repeater support");
    check(pMk2.fmRepeater && pMk2.fmRepeater->dialect == FmRepeaterDialect::Basic
              && pMk2.fmRepeater->hasRxCtcss && !pMk2.fmRepeater->hasDtcs,
          "IC-7300MK2 maps tone squelch without inventing DTCS");
    check(p9700.fmRepeater && p9700.fmRepeater->dialect == FmRepeaterDialect::Extended
              && p9700.fmRepeater->hasDtcs && p9700.fmRepeater->hasTxFrequencyReadback,
          "IC-9700 maps the complete live-proved repeater surface");
    const FeatureEvidence* live705 = p705.evidenceFor(IcomFeature::FmRepeaterBasic);
    check(live705 && live705->evidence == EvidenceKind::OfficialGuideAndLiveHardware,
          "IC-705 tone, level, offset and XFC carry guide plus live evidence");
    const FeatureEvidence* extended705 =
        p705.evidenceFor(IcomFeature::FmRepeaterExtendedReadback);
    check(extended705 && extended705->evidence == EvidenceKind::OfficialGuide,
          "IC-705 DTCS and extended readback carry model-specific guide evidence");

    check(p705.scope.center && !p705.scope.fixed,
          "IC-705 scope profile exposes only its attested center mode");
    check(pMk2.scope.center && pMk2.scope.fixed && pMk2.scope.scrollCenter
              && pMk2.scope.scrollFixed && pMk2.scope.hasSweepSpeed,
          "IC-7300MK2 profile records all four scope modes and sweep speed");
    check(pMk2.rxAntenna && pMk2.rxAntenna->selectable
              && !pMk2.rxAntenna->readbackAvailable,
          "IC-7300MK2 RX-ANT quirk is explicit rather than a B6 branch");

    const auto spec = [](std::string_view id) -> const ControlSpec* {
        const auto it = std::ranges::find(controlSpecs(), id, &ControlSpec::id);
        return it == controlSpecs().end() ? nullptr : &*it;
    };
    const ControlSpec* rxAntenna = spec("rx.antenna");
    const ControlSpec* dataMode = spec("data.mode");
    const ControlSpec* txBandwidth = spec("tx.bandwidth.edges");
    const ControlSpec* dtcs = spec("repeater.dtcs");
    const ControlSpec* gpsPosition = spec("gps.position");
    const ControlSpec* gpsNtpServer = spec("gps.ntp.server");
    const IcomModel& model705 = *modelForCivAddress(0xA4);
    const IcomModel& model9700 = *modelForCivAddress(0xA2);
    const IcomModel& modelMk2 = *modelForCivAddress(0xB6);
    check(rxAntenna && !controlSupported(model705, p705, *rxAntenna)
              && controlSupported(modelMk2, pMk2, *rxAntenna),
          "effective registry gates RX-ANT to the IC-7300MK2 profile");
    check(dataMode && controlSupported(model705, p705, *dataMode)
              && controlSupported(model9700, p9700, *dataMode)
              && controlSupported(modelMk2, pMk2, *dataMode),
          "all three profiles attest selected-VFO mode/DATA/filter");
    check(txBandwidth && controlSupported(model705, p705, *txBandwidth)
              && !controlSupported(model9700, p9700, *txBandwidth)
              && controlSupported(modelMk2, pMk2, *txBandwidth),
          "effective registry refuses to borrow TX bandwidth on IC-9700");
    check(dtcs && dtcs->wiring == Wiring::Both && dtcs->encoding == Encoding::Dtcs
              && dtcs->seamVerb == "setSliceFmDtcs"
              && controlSupported(model9700, p9700, *dtcs)
              && controlSupported(model705, p705, *dtcs)
              && !controlSupported(modelMk2, pMk2, *dtcs),
          "DTCS write/read wiring is effective only for documented model profiles");
    const IcomModel& identityOnly = *modelForCivAddress(0x98);
    const IcomModelProfile& identityOnlyProfile = profileFor(identityOnly);
    const ControlSpec* frequency = spec("freq");
    const ControlSpec* scopeMode = spec("scope.onoff");
    check(identityOnlyProfile.supports(IcomFeature::Core)
              && identityOnlyProfile.evidenceFor(IcomFeature::Core) == nullptr
              && frequency
              && controlSupported(identityOnly, identityOnlyProfile, *frequency),
          "identity-only models retain the generic CI-V floor without claiming evidence");
    check(scopeMode && controlSupported(identityOnly, identityOnlyProfile, *scopeMode)
              && !identityOnlyProfile.supports(IcomFeature::Scope),
          "scope reachability follows identity geometry while attestation remains absent");
    check(gpsPosition && gpsNtpServer
              && controlSupported(model705, p705, *gpsPosition)
              && controlSupported(model705, p705, *gpsNtpServer)
              && !controlSupported(model9700, p9700, *gpsPosition)
              && !controlSupported(modelMk2, pMk2, *gpsNtpServer),
          "effective registry maps GPS/NTP only onto the attested IC-705 profile");
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
    const IcomModel* ic7300mk2 = modelForCivAddress(0xB6);
    check(ic705 && !powerCurveFor(*ic705).empty(), "the IC-705 has a measured watts curve");
    // Handing back the IC-705's curve for another radio would produce a watts
    // figure an operator would act on, derived from a different PA. The 9700
    // instead owns a relative-percent curve from its own Po scale.
    check(ic9700 && !powerCurveFor(*ic9700).empty(),
          "the IC-9700 gets its own relative Po curve, not the IC-705 watts curve");
    if (ic9700) {
        check(profileFor(*ic9700).meters.calibration
                      == MeterCalibration::Ic9700
                  && profileFor(*ic9700).meters.powerConversion
                      == MeterCalibrationProfile::PowerConversion::RelativePercentOfBandRating,
              "the IC-9700 retains voltage calibration alongside relative Po conversion");
        const auto curve = powerCurveFor(*ic9700);
        const auto bands = bandsFor(*ic9700);
        check(bands.size() == 3, "the IC-9700 exposes three rated RF decks");
        for (const IcomBand& band : bands) {
            check(near(derivedPowerWatts(interpolateCurve(curve, 0),
                                         band.maxWatts), 0.0),
                  "zero Po maps to zero derived watts on every IC-9700 band");
            check(near(derivedPowerWatts(interpolateCurve(curve, 143),
                                         band.maxWatts), band.maxWatts * 0.5),
                  "50 percent Po maps to half the active IC-9700 deck rating");
            check(near(derivedPowerWatts(interpolateCurve(curve, 213),
                                         band.maxWatts), band.maxWatts),
                  "100 percent Po maps to the active IC-9700 deck rating");
        }
    }
    check(near(interpolateCurve(powerCurveFor(*ic705), 143), 5.0)
              && near(interpolateCurve(powerCurveFor(*ic705), 213), 10.0),
          "the IC-705 retains its native measured-watts curve");
    check(ic7300mk2
              && near(interpolateCurve(powerCurveFor(*ic7300mk2), 143), 50.0)
              && near(interpolateCurve(powerCurveFor(*ic7300mk2), 213), 100.0)
              && profileFor(*ic7300mk2).meters.powerConversion
                  == MeterCalibrationProfile::PowerConversion::NativeWatts,
          "the IC-7300MK2 retains its native watts profile");
    check(powerCurveFor(unknownModel()).empty(), "and nor does an unknown radio");
    check(powerCurveForCalibration(MeterCalibration::Ic7300Mk2).data()
              == powerCurveIc7300Mk2().data(),
          "power presentation and conversion share the IC-7300MK2 curve selector");
    check(powerCurveForCalibration(MeterCalibration::Ic9700).empty(),
          "the IC-9700 meter calibration fails closed to relative power");
}

// The mode vocabulary this backend publishes onto the slice (#5040).
//
// WFM was implemented end to end in CivCodec from the start and was unreachable
// only because nothing published a mode list, so the UI stayed on its
// compiled-in FlexRadio one — which has no WFM because a FLEX-6000 has no WFM.
static void testModeList()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 != nullptr, "the IC-705 is in the table");
    if (!ic705)
        return;

    const auto modes = modeListFor(*ic705);
    check(!modes.empty(), "the IC-705 publishes a mode list");
    check(std::find(modes.begin(), modes.end(), std::string_view{"WFM"}) != modes.end(),
          "and WFM is in it - the whole point of #5040");
    check(std::find(modes.begin(), modes.end(), std::string_view{"CW"}) != modes.end(),
          "normal Icom CW is published under the neutral CW name");
    check(std::find(modes.begin(), modes.end(), std::string_view{"CWU"}) == modes.end(),
          "the Flex-oriented CWU alias is not published for Icom");

    // EVERY ENTRY MUST ROUND-TRIP. A name the radio can be put into but never
    // reports back (RTTY, which comes home as DIGL) makes the combo jump on the
    // confirmation read; a name modeFromNeutral refuses (SAM) silently reverts.
    // Both read as a broken control.
    for (const std::string_view m : modes) {
        bool data = false;
        const auto civ = modeFromNeutral(std::string(m), data);
        check(civ.has_value(), "every published mode is one the radio accepts");
        if (!civ)
            continue;
        check(modeToNeutral(*civ, data) == std::string(m),
              "and one the radio reports back under the same name");
        check(std::count(modes.begin(), modes.end(), m) == 1, "listed exactly once");
    }

    // The same provenance rule powerCurveFor states: a row nobody has read that
    // model's own guide for gets NOTHING, not the IC-705's list.
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    const IcomModel* mk2 = modelForCivAddress(0xB6);
    check(ic9700 && modeListFor(*ic9700).empty(), "another model gets no borrowed list");
    check(mk2 && modeListFor(*mk2).empty(), "including the verified IC-7300MK2");
    check(modeListFor(unknownModel()).empty(), "and nor does an unknown radio");
}

// WFM receives 76-108 MHz broadcast; the transmitter does not follow.
static void testWfmIsReceiveOnly()
{
    const IcomModel* ic705 = modelForCivAddress(0xA4);
    check(ic705 && modeIsReceiveOnly(*ic705, "WFM"), "the IC-705 does not transmit in WFM");
    check(ic705 && !modeIsReceiveOnly(*ic705, "FM"), "but FM keys normally");
    check(ic705 && !modeIsReceiveOnly(*ic705, "USB"), "and so does USB");
    // No claim in EITHER direction for a model whose modes we have not read.
    check(!modeIsReceiveOnly(unknownModel(), "WFM"),
          "an unknown radio is not second-guessed");
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
    testPolledTxMeterMinimumHold();
    testModelTable();
    testUnknownModelIsConservative();
    testCapabilityProfiles();
    testModelDiscovery();
    testPowerCurveIsNotShared();
    testModeList();
    testWfmIsReceiveOnly();

    if (g_failures == 0)
        std::printf("icom_meters_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
