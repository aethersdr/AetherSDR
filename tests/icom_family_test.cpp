// IcomCIV — backend selection. Proves `family = "icom"` actually reaches
// IcomCivBackend through RadioModel's real swap path, and that the capabilities
// the model then reports are Icom-shaped rather than the Flex defaults.
//
// Modelled on hl2_family_transition_test: connectToRadio() rebuilds the backend
// SYNCHRONOUSLY before any network I/O, so an unroutable address exercises the
// whole post-swap state without hardware.
//
// This is the test that would have caught the gap where every layer below was
// written, tested and green while nothing in the application could construct it.

#include "models/RadioModel.h"
#include "core/RadioDiscovery.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/CivCodec.h"
#include "models/BandDefs.h"
#include "models/DeclaredBands.h"
#include "gui/ExperimentalRadioSupport.h"

#include <QCoreApplication>

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static RadioInfo infoFor(const QString& family)
{
    RadioInfo i;
    i.family  = family;
    i.serial  = QStringLiteral("ICOM-TEST-1");
    i.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    i.port    = 50001;
    return i;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    RadioModel model;

    const auto icomNotice = experimentalRadioDescriptor(QStringLiteral("icom"));
    const auto hl2Notice = experimentalRadioDescriptor(QStringLiteral("hl2"));
    check(icomNotice && icomNotice->displayName == QStringLiteral("Icom"),
          "Icom is identified as an experimental radio family");
    check(hl2Notice && hl2Notice->displayName == QStringLiteral("Hermes-Lite 2"),
          "Hermes-Lite 2 is identified as an experimental radio family");
    check(!experimentalRadioDescriptor(QStringLiteral("flex")),
          "Flex is not marked as an experimental radio family");
    check(icomNotice
              && experimentalRadioNoticeText(icomNotice->displayName)
                     .contains(QStringLiteral("Help \u2192 File an Issue")),
          "the experimental notice points operators to the issue-reporting workflow");

    // ---- the factory selects it ------------------------------------------
    model.connectToRadio(infoFor(QStringLiteral("icom")));
    check(model.family() == QStringLiteral("icom"), "the model adopts the icom family");
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) != nullptr,
          "and makeBackend() actually produced an IcomCivBackend");

    // ---- and the capabilities are Icom-shaped, not Flex defaults ---------
    const RadioCapabilities caps = model.backend()->capabilities();
    check(caps.family == QStringLiteral("icom"), "capabilities report the icom family");

    // The three that would be silently wrong if the fall-through to FlexBackend
    // were still happening, and each has a real consequence:
    check(!caps.hostModulates,
          "the RADIO modulates — true would open the host mic on a radio that never uses it");
    check(!caps.hasDaxStreams,
          "no IQ on any networked Icom — a true here offers a DAX-IQ path that cannot exist");
    check(caps.clientSettingsDomains == RadioCapabilities::ClientSettingsDomains{},
          "an Icom remembers its own state, so the client restores NOTHING");
    RadioCapabilities transmittingIcom = caps;
    transmittingIcom.canTransmit = true;
    check(wsprSeamAudioRouteReady(true, transmittingIcom),
          "an armed Icom seam-audio route is ready without host modulation");
    transmittingIcom.takesTxAudioOverSeam = false;
    check(!wsprSeamAudioRouteReady(true, transmittingIcom),
          "the WSPR route fails closed if the current backend cannot take seam audio");

    const auto ic705Mod = icom::modulationProfileFor(
        *icom::modelForCivAddress(0xA4));
    const auto mk2Mod = icom::modulationProfileFor(
        *icom::modelForCivAddress(0xB6));
    check(ic705Mod && ic705Mod->dataOffInputItem == 118
              && ic705Mod->dataInputItem == 119
              && ic705Mod->networkOnlyValue == 0x03,
          "IC-705 uses SET 0118/0119 and WLAN value 03");
    check(mk2Mod && mk2Mod->dataOffInputItem == 84
              && mk2Mod->dataInputItem == 85
              && mk2Mod->networkOnlyValue == 0x05,
          "IC-7300MK2 uses SET 0084/0085 and LAN value 05");
    // The fallback PC Audio "off" writes when there is nothing captured to put
    // back. It belongs to the model, not to the call site: these two agree at
    // 0x00 today, and a third model whose MIC is elsewhere must not inherit it.
    check(ic705Mod && ic705Mod->micValue == 0x00
              && mk2Mod && mk2Mod->micValue == 0x00,
          "both verified models name MIC in their own profile rather than "
          "leaving the caller to hardcode it");
    // An IC-9700 has no Wi-Fi and, unlike the two profiles above, no verified
    // model-specific modulation map. It must therefore remain outside this
    // read/write path instead of borrowing the IC-705's WLAN table. Pin that
    // distinction so the old false warning cannot quietly come back.
    check(!AetherSDR::icom::modelForCivAddress(0xA2)->hasWifi,
          "the IC-9700 has no Wi-Fi — so no WLAN MOD Input to demand");
    check(!AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA2))
               .meters.hasPaTemperatureTelemetry,
          "the IC-9700 profile does not declare PA-temperature telemetry");

    // ── TX bandwidth: the models genuinely differ ─────────────────────────
    {
        const auto ic705Tbw = icom::txBandwidthProfileFor(*icom::modelForCivAddress(0xA4));
        const auto mk2Tbw   = icom::txBandwidthProfileFor(*icom::modelForCivAddress(0xB6));
        check(ic705Tbw && ic705Tbw->wideItem == 19 && ic705Tbw->midItem == 20
                  && ic705Tbw->narrowItem == 21 && ic705Tbw->dataItem == 22,
              "IC-705 TX bandwidth lives at SET 0019/0020/0021/0022");
        check(mk2Tbw && mk2Tbw->wideItem == 14 && mk2Tbw->midItem == 15
                  && mk2Tbw->narrowItem == 16 && mk2Tbw->dataItem == 17,
              "IC-7300MK2 TX bandwidth lives at SET 0014/0015/0016/0017");

        // THE MK2 ADDED TWO LOW EDGES the IC-705 has not got. This is the whole
        // reason the tables are per-model rather than one shared constant, so
        // it is pinned rather than left as a comment.
        check(ic705Tbw && ic705Tbw->lowEdgesHz.size() == 4,
              "the IC-705 offers four TX low edges");
        check(mk2Tbw && mk2Tbw->lowEdgesHz.size() == 6,
              "the IC-7300MK2 offers six — it added 120 and 150 Hz");
        check(mk2Tbw && mk2Tbw->lowEdgesHz[1] == 120 && mk2Tbw->lowEdgesHz[2] == 150,
              "and those two are where the guide puts them");
        check(ic705Tbw && mk2Tbw
                  && ic705Tbw->highEdgesHz.size() == 4 && mk2Tbw->highEdgesHz.size() == 4,
              "both share the same four high edges");

        // SNAPPING is what makes the seam's continuous Hz honest. A request the
        // radio cannot reach must land on one it can, per model.
        check(mk2Tbw && icom::nearestEdgeHz(mk2Tbw->lowEdgesHz, 130) == 120,
              "an IC-7300MK2 reaches 120 Hz");
        check(ic705Tbw && icom::nearestEdgeHz(ic705Tbw->lowEdgesHz, 130) == 100,
              "an IC-705 asked for the same lands on 100 — it has no 120");
        check(ic705Tbw && icom::nearestEdgeHz(ic705Tbw->highEdgesHz, 3300) == 2900,
              "a 3.3 kHz high cut clamps to the 2.9 kHz ceiling");
        check(mk2Tbw && icom::edgeIndexFor(mk2Tbw->lowEdgesHz, 500) == 5
                  && icom::edgeIndexFor(mk2Tbw->highEdgesHz, 2500) == 0,
              "edge indices are what goes in the packed BCD nibbles");

        // AN UNREAD MODEL GETS NOTHING, so setTxFilter() declines rather than
        // writing a passband into whatever SET item happens to share the number.
        check(!icom::txBandwidthProfileFor(*icom::modelForCivAddress(0xA2)),
              "the IC-9700 has no TBW profile and must not borrow one");
        check(!icom::txBandwidthProfileFor(icom::unknownModel()),
              "and neither does an unrecognised radio");
    }
    check(AetherSDR::icom::modelForCivAddress(0xA4)->hasWifi,
          "the IC-705 does — the one model the WLAN check is legitimate for");

    check(!caps.radioOwnsDbmScale,
          "the scope scale is FIXED (ScopeCalibration), not the radio's to set — "
          "true here re-arms the noise-floor auto-adjust against a radio that "
          "never echoes a range back, and it ratchets 24 dB/s off the scale");

    // ---- the published dBm axis tracks the reference level, WITH THE RIGHT SIGN ----
    //
    // toDbm() maps a sample to `floorDbm + (v/max)*spanDb - referenceDb`, so
    // raising the radio's reference level moves the decoded trace DOWN. The axis
    // the display draws has to move the same way, or trace and scale disagree by
    // twice the reference — invisible at the default 0, and a growing error the
    // further the operator moves it. The first version of this emit ADDED
    // referenceDb where toDbm subtracts it.
    //
    // Driven through invokeExtension("scope.reference"), which is the real
    // operator path AND the place the range must be re-published: before this,
    // the range was emitted once at connect and never updated, so the trace slid
    // and the scale stayed put.
    {
        auto* icomBackend = dynamic_cast<icom::IcomCivBackend*>(model.backend());
        check(icomBackend != nullptr, "an Icom backend to drive");
        if (icomBackend) {
            double gotMin = 0.0, gotMax = 0.0;
            int emits = 0;
            QObject::connect(icomBackend, &IRadioBackend::panRangeChanged,
                             [&](const QString&, double lo, double hi) {
                                 gotMin = lo; gotMax = hi; ++emits;
                             });

            // An UNIDENTIFIED radio falls back to kUnknown, which has no scope,
            // so this must publish NOTHING. Pinning the quiet case matters: the
            // emit is on the connect path, and a version that published a range
            // for a radio with no scope would put an axis on a pane that never
            // gets a trace.
            icomBackend->invokeExtension(QStringLiteral("icom"),
                                         QStringLiteral("scope.reference"), 0, 10.0);
            check(emits == 0,
                  "a radio with no scope publishes no dBm range");

            // The SIGN itself is pinned in icom_scope_test against toDbm(),
            // which is the relationship that matters and can be asserted
            // without a live radio. Reaching a scope-capable m_model needs the
            // 0x19 0x00 reply over a real serial stream, so the emitted VALUE
            // is exercised on hardware rather than faked here — recorded as a
            // gap rather than papered over with a mock that proves nothing.
            (void)gotMin;
            (void)gotMax;
        }
    }

    // A pure seam backend owns no RadioConnection and no PanadapterStream, so
    // setupBackend()'s dynamic_cast chain must SKIP it — the same shape as HL2.
    check(model.backend()->ownsRxAudio(),
          "audio arrives over the seam, which is what the RX-audio wiring keys off");

    // ---- unknown families still fall through to Flex ----------------------
    model.connectToRadio(infoFor(QStringLiteral("nonsuch")));
    check(model.backend()->capabilities().family != QStringLiteral("icom"),
          "an unrecognised family does NOT get the Icom backend");

    // ---- round trip leaves a clean model ----------------------------------
    // Flex -> Icom -> Flex. The swap destroys the old backend, and every
    // connection made in setupBackend() has it as sender or receiver, so a
    // leaked one would show up here as a crash rather than a wrong value.
    model.connectToRadio(infoFor(QStringLiteral("flex")));
    model.connectToRadio(infoFor(QStringLiteral("icom")));
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) != nullptr,
          "flex -> icom swaps in cleanly");
    model.connectToRadio(infoFor(QStringLiteral("flex")));
    check(dynamic_cast<icom::IcomCivBackend*>(model.backend()) == nullptr,
          "and icom -> flex swaps back out");
    check(model.family() == QStringLiteral("flex"), "leaving the model Flex-capable");

    // ---- the model table, which the CI-V chooser is populated from ---------
    //
    // The connect panel builds its list from knownModels() rather than a
    // hand-typed one precisely so the two cannot drift, which makes these table
    // invariants load-bearing for the UI as well as the decoder.
    {
        const auto models = icom::knownModels();
        check(!models.empty(), "the model table is not empty");
        for (const auto& m : models) {
            check(m.isKnown(),
                  "every table row has a real CI-V address - a 0 would render as "
                  "a chooser entry that addresses nobody");
            check(!m.name.empty(), "and a name to put in front of the operator");
            // The address is the key modelForCivAddress() looks up, so a
            // duplicate would make one of the two models unreachable by the
            // 0x19 0x00 reply - silently, and in favour of whichever came first.
            check(icom::modelForCivAddress(m.civAddress) != nullptr,
                  "and is reachable by its own address");
            // ROUND TRIP through the name, which is the ONLY identity available
            // during the RS-BA1 handshake - before any CI-V stream exists, and
            // therefore the only thing that can seed the address in time for the
            // connect-edge read burst.
            const icom::IcomModel* byName = icom::modelForName(m.name);
            check(byName != nullptr && byName->civAddress == m.civAddress,
                  "and its own name resolves back to it");

            // ---- the declared band set (#5041) ---------------------------
            //
            // Two invariants, because a band declaration is the ONE row field
            // that renders straight into a button the operator presses.
            //
            // (1) EVERY token survives parseDeclaredBands(). That function
            //     silently drops anything outside BandDefs, which is the right
            //     boundary behaviour for a hostile gateway and exactly the
            //     wrong failure for a typo in this table: the band simply has
            //     no button and nothing anywhere says why. Comparing the
            //     surviving count against the tokens written turns that silence
            //     into a red test.
            //
            // (2) Every declared band lies INSIDE this row's own tuning range.
            //     The range already disables unreachable band buttons, so a
            //     declaration reaching past it renders a button that is drawn
            //     and then greyed out — the row contradicting itself, in the
            //     UI. Containment keeps the two statements one statement.
            const QString raw = QString::fromUtf8(
                m.bands.data(), static_cast<int>(m.bands.size()));
            const QStringList declared = parseDeclaredBands(raw);
            const int tokens = raw.split(',', Qt::SkipEmptyParts).size();
            check(declared.size() == tokens,
                  "every declared band name is a real BandDefs band - a token "
                  "dropped here is a band button that silently never appears");
            for (const QString& name : declared) {
                for (const auto& def : kBands) {
                    if (name != QLatin1String(def.name))
                        continue;
                    check(def.lowMhz * 1e6 >= static_cast<double>(m.tuningMinHz)
                              && def.highMhz * 1e6
                                     <= static_cast<double>(m.tuningMaxHz),
                          "and lies inside the tuning range the same row "
                          "declares");
                    break;
                }
            }
        }
    }

    // ---- the bands an IC-705 actually gets --------------------------------
    //
    // The reported bug: an IC-705 reaches 2 m and 70 cm natively, and had no
    // band button for either, because the band menu falls back to FlexLib's
    // ModelCapabilities table when nothing is declared and no Flex covers UHF.
    // These pin the declaration that closes it — the whole chain from this row
    // to a rendered button is table -> parseDeclaredBands -> band grid.
    {
        const icom::IcomModel* ic705 = icom::modelForName("IC-705");
        check(ic705 != nullptr, "the IC-705 is in the table");
        const QStringList bands = parseDeclaredBands(
            QString::fromUtf8(ic705->bands.data(),
                              static_cast<int>(ic705->bands.size())));
        check(bands.contains(QStringLiteral("2m")),
              "the IC-705 declares 2m, so the band menu offers it");
        check(bands.contains(QStringLiteral("440")),
              "and 440 - the band that had no entry in the built-in grid at all");
        check(bands.contains(QStringLiteral("20m")) && bands.contains(QStringLiteral("6m")),
              "and still every HF band plus 6m, which the declaration REPLACES "
              "the built-in grid with rather than adding to");
        check(!bands.contains(QStringLiteral("2200m"))
                  && !bands.contains(QStringLiteral("630m")),
              "but not the LF/MF utility rows, which are not declarable");

        // The tri-bander, whose HF grid was entirely unpressable before.
        const icom::IcomModel* ic9700 = icom::modelForName("IC-9700");
        check(ic9700 != nullptr, "the IC-9700 is in the table");
        check(parseDeclaredBands(
                  QString::fromUtf8(ic9700->bands.data(),
                                    static_cast<int>(ic9700->bands.size())))
                  == QStringList({QStringLiteral("2m"), QStringLiteral("440"),
                                  QStringLiteral("23cm")}),
              "the IC-9700 declares exactly its three bands");

        // AND THE HF-ONLY ROWS DECLARE NOTHING. Empty is a decision here, not
        // an omission: it keeps the built-in HF grid, which is already right for
        // them. A declaration added to one of these would REPLACE that grid, so
        // this is the guard against a well-meant edit doing it by halves.
        for (std::uint8_t addr : {0x98, 0x8E, 0x94, 0xB6}) {   // 7610, 785x, 7300, 7300MK2
            const icom::IcomModel* m = icom::modelForCivAddress(addr);
            check(m != nullptr && m->bands.empty(),
                  "an HF-only row declares no bands and keeps the built-in grid");
        }
        check(icom::unknownModel().bands.empty(),
              "and an unidentified radio declares nothing at all");
    }

    // ---- hasNetwork is what the Connect-by-IP chooser filters on ----------
    //
    // The connect page lists only radios it can actually dial, so this flag is
    // now load-bearing for the UI as well as the backend. Pin both sides of the
    // discriminator: flipping either one silently changes what the operator is
    // offered, and the IC-7300 / IC-7300MK2 pair is exactly where it is easy to
    // get wrong — same family name, one USB-only, one with an Ethernet port.
    check(!icom::modelForCivAddress(0x94)->hasNetwork,
          "the IC-7300 is CI-V only - it cannot answer a Connect-by-IP session "
          "directly, so the chooser must not offer it");
    check(icom::modelForCivAddress(0xB6)->hasNetwork,
          "the IC-7300MK2 CAN - it has an Ethernet port, and dropping it from "
          "the chooser would hide the model this backend was validated on");

    // The name is FREE TEXT set on the radio, so the match has to survive the
    // ways it is really written.
    check(icom::modelForName("IC-705") != nullptr, "IC-705 resolves");
    check(icom::modelForName("ic-705") != nullptr, "lower case resolves");
    check(icom::modelForName("IC705") != nullptr, "no hyphen resolves");
    check(icom::modelForName("ic705") != nullptr, "neither resolves");
    check(icom::modelForName("IC-705") == icom::modelForName("ic705"),
          "and all of them are the same model");
    check(icom::modelForName("") == nullptr, "an empty name resolves nothing");
    check(icom::modelForName("IC-7760") == nullptr,
          "a model absent from the table resolves nothing - which is the case "
          "only the broadcast address query can rescue");

    // ---- the 0x19 0x00 reply, including our own echo of it ----------------
    {
        auto idFrame = [](std::uint8_t from, std::vector<std::uint8_t> data) {
            icom::CivFrame f;
            f.to = icom::kControllerAddress;
            f.from = from;
            f.cmd = icom::cmd::kReadId;
            f.hasSub = true;
            f.sub = 0x00;
            f.data = std::move(data);
            return f;
        };
        check(icom::parseModelIdReply(idFrame(0xA2, {0xA2})) == 0xA2,
              "a real reply yields the reported address");

        // THE ECHO. A broadcast send comes back as to=0x00, from=0xE0 and
        // reaches the parser on the real radios - measured 2026-08-14, echo
        // first on every run. It carries NO data byte, because the query form
        // has none, so it must not be mistaken for an answer. Getting this
        // wrong would read the address as whatever happened to be in an empty
        // payload, once per connect, on every Icom.
        icom::CivFrame echo;
        echo.to = icom::kBroadcastAddress;
        echo.from = icom::kControllerAddress;
        echo.cmd = icom::cmd::kReadId;
        echo.hasSub = true;
        echo.sub = 0x00;
        check(!icom::parseModelIdReply(echo).has_value(),
              "our own broadcast echo is not an address report");

        // Neighbouring commands must not be read as one either.
        icom::CivFrame other = idFrame(0xA2, {0xA2});
        other.cmd = icom::cmd::kReadFreq;
        check(!icom::parseModelIdReply(other).has_value(),
              "a different command is not an address report");
        other = idFrame(0xA2, {0xA2});
        other.sub = 0x01;
        check(!icom::parseModelIdReply(other).has_value(),
              "and neither is a different sub-command");
    }

    if (g_failures == 0)
        std::printf("icom_family_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
