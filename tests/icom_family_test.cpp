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
#include "core/backends/icom/IcomControls.h"
#include "core/backends/icom/IcomModels.h"
#include "core/backends/icom/CivCodec.h"
#include "models/BandDefs.h"
#include "models/DeclaredBands.h"
#include "models/SliceModel.h"
#include "gui/ExperimentalRadioSupport.h"

#include <QCoreApplication>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

using namespace AetherSDR;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
    }

    static std::optional<std::vector<std::uint8_t>> confirmationFor(
        const IcomCivBackend& backend, const std::vector<std::uint8_t>& frame)
    {
        return backend.confirmationFor(frame);
    }

    static void injectConnectedFrame(IcomCivBackend& backend, const CivFrame& frame)
    {
        backend.m_connected = true;
        backend.onCivFrame(frame, backend.m_sessionGeneration);
    }
};

} // namespace AetherSDR::icom

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static RadioInfo infoFor(const QString& family,
                         const QString& serial = QStringLiteral("ICOM-TEST-1"))
{
    RadioInfo i;
    i.family  = family;
    i.serial  = serial;
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
    const auto ananNotice = experimentalRadioDescriptor(QStringLiteral("anan"));
    check(icomNotice && icomNotice->displayName == QStringLiteral("Icom"),
          "Icom is identified as an experimental radio family");
    check(hl2Notice && hl2Notice->displayName == QStringLiteral("Hermes-Lite 2"),
          "Hermes-Lite 2 is identified as an experimental radio family");
    check(ananNotice && ananNotice->displayName == QStringLiteral("ANAN-G2"),
          "ANAN-G2 is identified as an experimental radio family");
    check(!experimentalRadioDescriptor(QStringLiteral("flex")),
          "Flex is not marked as an experimental radio family");
    check(icomNotice
              && experimentalRadioNoticeText(icomNotice->displayName, /*transmitAvailable=*/true)
                     .contains(QStringLiteral("Help \u2192 File an Issue")),
          "the experimental notice points operators to the issue-reporting workflow");
    check(icomNotice
              && experimentalRadioNoticeText(icomNotice->displayName, /*transmitAvailable=*/true)
                     .contains(QStringLiteral("receive and transmit functions are available")),
          "a transmit-capable family's notice does not claim receive-only");
    check(ananNotice
              && experimentalRadioNoticeText(ananNotice->displayName, /*transmitAvailable=*/false)
                     .contains(QStringLiteral("receive-only")),
          "a receive-only family's notice does not falsely claim transmit support");

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
    check(!caps.hasDownwardExpander,
          "Icom exposes no DEXP surface without an evidenced command path");
    check(!caps.canReboot && !caps.hasRemoteOnControl
              && !caps.canUpgradeFirmware,
          "Icom hides unsupported remote radio-management controls");
    check(!caps.hasSmartLink && !caps.hasLicenseInfo
              && !caps.hasClientNetworkConfig
              && !caps.hasFlexControlIntegration
              && !caps.hasAudioCompression && !caps.hasSharpFilters,
          "Icom hides unsupported Flex Settings surfaces");
    check(!caps.usesVita49Transport,
          "Icom hides Flex VITA-49 receive-buffer tuning");
    check(!caps.hasPrivateIpConnectionPolicy,
          "Icom hides the Flex private-IP connection policy");

    auto* selectedBackend = dynamic_cast<icom::IcomCivBackend*>(model.backend());
    check(selectedBackend != nullptr,
          "the selected Icom backend is available for model capability checks");
    if (selectedBackend) {
        const icom::IcomModel& initialModel = selectedBackend->model();
        const auto* ic705 = icom::modelForCivAddress(0xA4);
        const auto* ic9700 = icom::modelForCivAddress(0xA2);
        check(ic705 && ic9700, "the IC-705 and IC-9700 model profiles exist");
        if (ic705 && ic9700) {
            icom::IcomCivBackendTestAccess::selectModel(*selectedBackend, *ic705);
            const RadioCapabilities ic705Caps = selectedBackend->capabilities();
            check(ic705Caps.fmToneModes.contains(QStringLiteral("dtcs_txrx"))
                      && ic705Caps.fmToneModes.contains(
                          QStringLiteral("ctcss_tx_dtcs_rx"))
                      && ic705Caps.fmDtcsCodes.size() == 104,
                  "IC-705 advertises its documented complete DTCS UI vocabulary");

            icom::IcomCivBackendTestAccess::selectModel(*selectedBackend, *ic9700);
            const RadioCapabilities ic9700Caps = selectedBackend->capabilities();
            check(ic9700Caps.fmToneModes.contains(QStringLiteral("dtcs_txrx"))
                      && ic9700Caps.fmToneModes.contains(
                          QStringLiteral("ctcss_tx_dtcs_rx"))
                      && ic9700Caps.fmDtcsCodes.size() == 104,
                  "IC-9700 advertises its documented complete DTCS UI vocabulary");

            RadioDelta published;
            bool sawNetworkName = false;
            QObject::connect(selectedBackend, &IRadioBackend::radioChanged,
                             [&published, &sawNetworkName](const RadioDelta& delta) {
                if (delta.networkName) {
                    published = delta;
                    sawNetworkName = true;
                }
            });
            icom::CivFrame networkNameFrame;
            networkNameFrame.cmd = icom::cmd::kSetting;
            networkNameFrame.hasSub = true;
            networkNameFrame.sub = 0x05;
            networkNameFrame.data = {0x01, 0x44, 'S', 'H', 'A', 'C', 'K'};
            icom::IcomCivBackendTestAccess::injectConnectedFrame(
                *selectedBackend, networkNameFrame);
            check(sawNetworkName
                      && published.networkName == QStringLiteral("SHACK")
                      && !published.nickname,
                  "IC-9700 Network Name publishes dedicated network identity");
            check(model.networkName() == QStringLiteral("SHACK")
                      && model.nickname().isEmpty(),
                  "Network Name reaches RadioModel without replacing station nickname");

            check(QMetaObject::invokeMethod(&model, "onDisconnected",
                                            Qt::DirectConnection),
                  "Icom network-name reset fixture reached RadioModel");
            check(model.networkName().isEmpty(),
                  "disconnect clears session-owned Icom Network Name");
        }
        icom::IcomCivBackendTestAccess::selectModel(*selectedBackend, initialModel);
    }

    {
        icom::IcomCivBackend backend;
        const auto* ic9700 = icom::modelForCivAddress(0xA2);
        check(ic9700 != nullptr, "the IC-9700 exists for disconnect-state coverage");
        if (ic9700) {
            icom::IcomCivBackendTestAccess::selectModel(backend, *ic9700);
            bool resetPublished = false;
            QObject::connect(&backend, &IRadioBackend::sliceChanged,
                             [&resetPublished](int, const SliceDelta& delta) {
                resetPublished = delta.fmDtcsCode == -1
                    && delta.fmDtcsTxReverse == false
                    && delta.fmDtcsRxReverse == false;
            });
            const bool invoked = QMetaObject::invokeMethod(
                &backend, "onSessionDisconnected", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("test disconnect")));
            check(invoked && resetPublished,
                  "IC-9700 disconnect withdraws established DTCS state");

            const auto confirmation =
                icom::IcomCivBackendTestAccess::confirmationFor(
                    backend, icom::cmdSetDtcsTone(0xA2, 23, true, false));
            // With no live session, confirmationFor() uses the documented
            // disconnected fallback address; this assertion proves the 1B 02
            // write schedules the matching authoritative register readback.
            check(confirmation
                      && *confirmation
                          == icom::cmdReadRepeaterToneRegister(
                              0xA4, icom::repeaterTone::kDtcs),
                  "DTCS operator writes schedule an immediate authoritative readback");
        }
    }

    // The Icom transport reports one stable VFO as slice 0. On reconnect,
    // RadioModel stages the old SliceModel so the UI can keep its subscriptions
    // alive while the backend confirms the new session. The non-Flex materializer
    // used to ignore that staged object and allocate a replacement: the VFO was
    // wired to the replacement by sliceAdded, while RX Controls remained wired
    // to the original object and stopped following TCI frequency changes.
    //
    // Both the IC-705 and IC-7300MK2 use this same family-neutral materializer;
    // model-specific CI-V profiles begin below the seam and cannot change this
    // ownership invariant.
    {
        RadioModel reconnectModel;
        reconnectModel.connectToRadio(infoFor(QStringLiteral("icom")));
        auto* icomBackend =
            dynamic_cast<icom::IcomCivBackend*>(reconnectModel.backend());
        check(icomBackend != nullptr, "an Icom backend exists for reconnect coverage");
        if (icomBackend) {
            int sliceAdds = 0;
            QObject::connect(&reconnectModel, &RadioModel::sliceAdded,
                             &reconnectModel,
                             [&sliceAdds](SliceModel*) { ++sliceAdds; });

            const bool firstConnected = QMetaObject::invokeMethod(
                icomBackend, "onSessionConnected", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("IC-705")));
            check(firstConnected, "the first Icom session reaches its connected edge");
            check(icomBackend->capabilities().hasGpsHardware,
                  "IC-705 backend publishes its profile's GPS hardware capability");
            check(reconnectModel.hasGpsSetupHardware(),
                  "IC-705 profile enables the Settings GPS hardware surface");

            SliceDelta initial;
            initial.panId = QStringLiteral("icom");
            initial.inUse = true;
            initial.active = true;
            initial.txSlice = true;
            initial.frequency = 14.074;
            emit icomBackend->sliceChanged(0, initial);

            SliceModel* subscribedSlice = reconnectModel.slice(0);
            check(subscribedSlice != nullptr,
                  "the first Icom VFO materializes a SliceModel");
            check(sliceAdds == 1,
                  "the first Icom VFO announces one UI slice");

            int subscriberUpdates = 0;
            if (subscribedSlice) {
                QObject::connect(subscribedSlice, &SliceModel::frequencyChanged,
                                 subscribedSlice,
                                 [&subscriberUpdates](double) { ++subscriberUpdates; });
            }

            // Re-enter through connectRadio while the old session is live. Icom
            // synchronously emits disconnected before starting the replacement,
            // exactly matching an operator reconnect to the selected radio.
            reconnectModel.connectToRadio(infoFor(QStringLiteral("icom")));
            const bool replacementConnected = QMetaObject::invokeMethod(
                icomBackend, "onSessionConnected", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("IC-705")));
            check(replacementConnected,
                  "the same Icom reaches its replacement connected edge");

            SliceDelta reconnected;
            reconnected.panId = QStringLiteral("icom");
            reconnected.inUse = true;
            reconnected.active = true;
            reconnected.txSlice = true;
            reconnected.frequency = 7.074;
            emit icomBackend->sliceChanged(0, reconnected);

            check(reconnectModel.slice(0) == subscribedSlice,
                  "Icom reconnect reclaims the subscribed SliceModel");
            check(sliceAdds == 1,
                  "reclaim does not announce a duplicate UI slice");
            check(subscriberUpdates == 1,
                  "a pre-reconnect frequency subscriber receives fresh Icom state");
            check(reconnectModel.slice(0)
                      && reconnectModel.slice(0)->frequency() == 7.074,
                  "the reclaimed Icom VFO applies the radio-authoritative frequency");

        }
    }

    // A different physical radio in the same family also reports slice 0.
    // Reclaim must be identity-shaped, not family/id-shaped: the old object
    // carries radio A's partially reported state and subscriptions.
    {
        RadioModel swapModel;
        swapModel.connectToRadio(infoFor(QStringLiteral("icom")));
        auto* swapBackend = dynamic_cast<icom::IcomCivBackend*>(swapModel.backend());
        check(swapBackend != nullptr, "an Icom backend exists for radio-swap coverage");
        if (swapBackend) {
            int sliceAdds = 0;
            QObject::connect(&swapModel, &RadioModel::sliceAdded,
                             &swapModel, [&sliceAdds](SliceModel*) { ++sliceAdds; });

            const bool firstConnected = QMetaObject::invokeMethod(
                swapBackend, "onSessionConnected", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("IC-705")));
            check(firstConnected, "radio A reaches its real connected edge");
            SliceModel* radioASlice = swapModel.slice(0);

            // Select B while A is still connected. connectToRadio overwrites
            // m_lastInfo with B before IcomCivBackend synchronously tears A down;
            // this is the ordering that requires the separate connected-session
            // serial rather than a disconnect-time read of m_lastInfo.
            swapModel.connectToRadio(infoFor(QStringLiteral("icom"),
                                             QStringLiteral("ICOM-TEST-2")));
            const bool connected = QMetaObject::invokeMethod(
                swapBackend, "onSessionConnected", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("IC-705")));
            check(connected, "the second Icom session reaches its real connected edge");
            check(swapModel.slice(0) != nullptr,
                  "the second Icom radio materializes its own slice");
            check(swapModel.slice(0) != radioASlice,
                  "a different Icom serial does not reclaim the prior radio's slice");
            check(sliceAdds == 2,
                  "a different Icom serial announces one replacement UI slice");
        }
    }

    RadioCapabilities transmittingIcom = caps;
    transmittingIcom.canTransmit = true;
    check(wsprSeamAudioRouteReady(true, transmittingIcom),
          "an armed Icom seam-audio route is ready without host modulation");
    transmittingIcom.takesTxAudioOverSeam = false;
    check(!wsprSeamAudioRouteReady(true, transmittingIcom),
          "the WSPR route fails closed if the current backend cannot take seam audio");

    const auto ic705Mod = icom::modulationProfileFor(
        *icom::modelForCivAddress(0xA4));
    const auto ic9700Mod = icom::modulationProfileFor(
        *icom::modelForCivAddress(0xA2));
    const auto mk2Mod = icom::modulationProfileFor(
        *icom::modelForCivAddress(0xB6));
    check(icom::profileFor(*icom::modelForCivAddress(0xA4)).hasGpsHardware,
          "IC-705 profile declares its internal GPS receiver");
    check(!icom::profileFor(*icom::modelForCivAddress(0xA2)).hasGpsHardware,
          "IC-9700 profile does not declare GPS hardware");
    check(!icom::profileFor(*icom::modelForCivAddress(0xB6)).hasGpsHardware,
          "IC-7300MK2 profile does not declare GPS hardware");
    const auto ic9700Network = icom::profileFor(
        *icom::modelForCivAddress(0xA2)).networkConfiguration;
    const auto ic705Network = icom::profileFor(
        *icom::modelForCivAddress(0xA4)).networkConfiguration;
    const auto mk2Network = icom::profileFor(
        *icom::modelForCivAddress(0xB6)).networkConfiguration;
    check(ic9700Network && ic9700Network->effectiveIpItem == 139
              && ic9700Network->subnetMaskItem == 140
              && ic9700Network->gatewayItem == 141
              && ic9700Network->networkNameItem == 144,
          "IC-9700 profile maps its documented SET 0139-0144 network fields");
    check(!ic705Network,
          "IC-705 does not claim network registers absent from its CI-V guide");
    check(mk2Network && mk2Network->effectiveIpItem == 102
              && mk2Network->subnetMaskItem == 103
              && mk2Network->gatewayItem == 104
              && mk2Network->networkNameItem == 107,
          "IC-7300MK2 profile maps its documented SET 0102-0107 network fields");
    check(ic705Mod && ic705Mod->dataOffInputItem == 118
              && ic705Mod->dataInputItem == 119
              && ic705Mod->networkOnlyValue == 0x03,
          "IC-705 uses SET 0118/0119 and WLAN value 03");
    check(mk2Mod && mk2Mod->dataOffInputItem == 84
              && mk2Mod->dataInputItem == 85
              && mk2Mod->networkOnlyValue == 0x05,
          "IC-7300MK2 uses SET 0084/0085 and LAN value 05");
    check(ic9700Mod && ic9700Mod->usbLevelItem == 113
              && ic9700Mod->accessoryLevelItem == 112
              && ic9700Mod->networkLevelItem == 114
              && ic9700Mod->dataOffInputItem == 115
              && ic9700Mod->dataInputItem == 116
              && ic9700Mod->networkOnlyValue == 0x05
              && ic9700Mod->phoneLevelFollowsNetworkInput,
          "IC-9700 uses its documented SET 0112-0116 modulation map and routes "
          "the Phone level through LAN only while LAN is selected");
    check(icom::profileFor(*icom::modelForCivAddress(0xA2))
              .supports(icom::IcomFeature::ModulationInput),
          "IC-9700 modulation input diagnostics carry model-owned guide evidence");
    check(ic705Mod && !ic705Mod->phoneLevelFollowsNetworkInput
              && mk2Mod && !mk2Mod->phoneLevelFollowsNetworkInput,
          "IC-705 and IC-7300MK2 retain their established physical-mic Phone "
          "level behavior");
    // The fallback PC Audio "off" writes when there is nothing captured to put
    // back. It belongs to the model, not to the call site: these two agree at
    // 0x00 today, and a third model whose MIC is elsewhere must not inherit it.
    check(ic705Mod && ic705Mod->micValue == 0x00
              && mk2Mod && mk2Mod->micValue == 0x00,
          "both verified models name MIC in their own profile rather than "
          "leaving the caller to hardcode it");
    // An IC-9700 has LAN rather than Wi-Fi. Its independently verified profile
    // above must therefore use value 05 and must not borrow the IC-705's WLAN
    // value 03.
    check(!AetherSDR::icom::modelForCivAddress(0xA2)->hasWifi,
          "the IC-9700 has no Wi-Fi — its network modulation source is LAN");
    check(!AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA2))
               .meters.hasPaTemperatureTelemetry,
          "the IC-9700 profile does not declare PA-temperature telemetry");
    check(AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA2))
              .meters.hasPaCurrentTelemetry,
          "the IC-9700 profile independently declares Radio Vitals PA current");
    check(!AetherSDR::icom::profileFor(
               *AetherSDR::icom::modelForCivAddress(0xA4))
               .meters.hasPaCurrentTelemetry
              && !AetherSDR::icom::profileFor(
                      *AetherSDR::icom::modelForCivAddress(0xB6))
                      .meters.hasPaCurrentTelemetry,
          "IC-705 and IC-7300MK2 do not inherit the IC-9700 Radio Vitals surface");
    check(AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA2))
              .speechProcessorLevelMaximum == 100,
          "the IC-9700 profile declares its continuous processor range");
    check(AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA2))
              .speechProcessorLabel == "COMP",
          "the IC-9700 profile declares its radio-native COMP label");
    check(AetherSDR::icom::profileFor(
              *AetherSDR::icom::modelForCivAddress(0xA4))
                  .speechProcessorLevelMaximum == 2
              && AetherSDR::icom::profileFor(
                     *AetherSDR::icom::modelForCivAddress(0xB6))
                     .speechProcessorLevelMaximum == 2,
          "IC-705 and IC-7300MK2 retain the three-position processor contract");
    check(icom::speechProcessorRawLevel(100, 0) == 0
              && icom::speechProcessorRawLevel(100, 50) == 128
              && icom::speechProcessorRawLevel(100, 100) == 255,
          "IC-9700 continuous COMP maps 0/50/100 percent to raw 0/128/255");
    check(icom::speechProcessorRawLevel(2, 0) == 76
              && icom::speechProcessorRawLevel(2, 1) == 153
              && icom::speechProcessorRawLevel(2, 2) == 229,
          "sibling Icom processor presets retain raw 76/153/229 encoding");
    std::vector<std::uint8_t> tunerModels;
    for (const icom::IcomModel& model : icom::knownModels()) {
        if (icom::profileFor(model).supports(icom::IcomFeature::AntennaTuner)) {
            tunerModels.push_back(model.civAddress);
        }
    }
    check(tunerModels == std::vector<std::uint8_t>{0xA4, 0x98, 0x8E, 0x94, 0xB6},
          "each evidenced internal/external-tuner model opts into tuner control");

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
        check(icom::profileFor(*ic705).supports(icom::IcomFeature::GpsPosition),
              "the IC-705 alone declares its verified CI-V GPS position surface");
        check(icom::profileFor(*ic705).supports(
                  icom::IcomFeature::GpsTimeConfiguration),
              "the IC-705 alone declares its verified NTP/GPS clock settings");
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
        const std::span<const icom::IcomBand> ic9700Bands =
            icom::bandsFor(*ic9700);
        check(ic9700Bands.size() == 3
                  && ic9700Bands[0].name == "2m"
                  && ic9700Bands[0].lowHz == 144'000'000ULL
                  && ic9700Bands[1].name == "440"
                  && ic9700Bands[1].lowHz == 430'000'000ULL
                  && ic9700Bands[2].name == "23cm"
                  && ic9700Bands[2].lowHz == 1'240'000'000ULL,
              "the IC-9700 publishes canonical names with native deck limits");
        check(icom::bandsFor(*ic705).empty(),
              "the IC-705 has no discontinuous native-band range override, so "
              "its declared buttons keep canonical labels");
        check(!icom::profileFor(*ic9700).supports(icom::IcomFeature::GpsPosition)
                  && !icom::profileFor(*ic9700).supports(
                      icom::IcomFeature::GpsTimeConfiguration),
              "another Icom does not inherit IC-705 GPS commands by profile position");
        check(parseDeclaredBands(
                  QString::fromUtf8(ic9700->bands.data(),
                                    static_cast<int>(ic9700->bands.size())))
                  == QStringList({QStringLiteral("2m"), QStringLiteral("440"),
                                  QStringLiteral("23cm")}),
              "the IC-9700 declares exactly its three bands");
        const auto ic9700Preamp = icom::preampLabelsFor(*ic9700);
        check(ic9700Preamp.size() == 2
                  && ic9700Preamp[0] == "OFF"
                  && ic9700Preamp[1] == "P.AMP INT",
              "the IC-9700 publishes only its internal preamp through the "
              "shared front-end control");

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
