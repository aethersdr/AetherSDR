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

#include <QCoreApplication>

#include <cstdio>

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
    // The MOD Input warning demands a WLAN modulation source. Only a radio with
    // Wi-Fi has one -- and in kModels exactly ONE model does (the IC-705, whose
    // 0xA4 is also the default CI-V address, which is almost certainly the radio
    // the check was written against). On every other networked Icom the warning
    // asked for a setting the radio cannot offer: an IC-9700 set correctly to
    // LAN on the front panel reports 0x01 and was warned at it on every single
    // session. Pin the discriminator so the gate cannot quietly come back.
    check(!AetherSDR::icom::modelForCivAddress(0xA2)->hasWifi,
          "the IC-9700 has no Wi-Fi — so no WLAN MOD Input to demand");
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

    if (g_failures == 0)
        std::printf("icom_family_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
