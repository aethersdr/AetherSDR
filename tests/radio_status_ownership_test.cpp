#include "models/RadioStatusOwnership.h"
#include "core/DaxTxPolicy.h"
#include "core/StreamStatus.h"
#include "core/UdpRegistrationPolicy.h"

#include <cstdio>

using namespace MasterSDR;
using namespace MasterSDR::RadioStatusOwnership;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

void testPanadapterOwnershipDecisions()
{
    constexpr quint32 ours = 0x12345678;
    QMap<QString, QString> noOwner;
    QMap<QString, QString> ourOwner{{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}};
    QMap<QString, QString> otherOwner{{QStringLiteral("client_handle"), QStringLiteral("0x87654321")}};

    check(classifyOwnedStatus(false, noOwner, false, ours) == OwnedStatusAction::Defer,
          "unknown pan without client_handle is deferred");
    check(classifyOwnedStatus(false, ourOwner, false, ours) == OwnedStatusAction::Claim,
          "unknown pan with our client_handle is claimed");
    check(classifyOwnedStatus(false, otherOwner, false, ours) == OwnedStatusAction::Ignore,
          "unknown pan with another client_handle is ignored");
    check(classifyOwnedStatus(true, noOwner, false, ours) == OwnedStatusAction::Apply,
          "known pan can apply legacy status without client_handle");
    check(classifyOwnedStatus(true, otherOwner, false, ours) == OwnedStatusAction::Ignore,
          "known pan ignores conflicting other-client status");
    check(classifyOwnedStatus(false, noOwner, true, ours) == OwnedStatusAction::Remove,
          "pan removal wins over ownership uncertainty");
}

void testRemoteAudioRxTracking()
{
    constexpr quint32 ours = 0x12345678;
    RemoteAudioRxTracking state;

    QMap<QString, QString> otherRemoteAudio{
        {QStringLiteral("type"), QStringLiteral("remote_audio_rx")},
        {QStringLiteral("client_handle"), QStringLiteral("0x87654321")}
    };
    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000A"),
                                   otherRemoteAudio,
                                   ours,
                                   false) == RemoteAudioRxAction::IgnoredOtherClient,
          "remote_audio_rx for another client is ignored");
    check(state.streamId == 0, "ignored remote_audio_rx does not set stream id");

    QMap<QString, QString> noOwnerRemoteAudio{
        {QStringLiteral("type"), QStringLiteral("remote_audio_rx")}
    };
    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000A"),
                                   noOwnerRemoteAudio,
                                   ours,
                                   false) == RemoteAudioRxAction::DeferredUnknownOwner,
          "remote_audio_rx without owner is deferred when other clients may exist");
    check(state.streamId == 0, "deferred remote_audio_rx does not set stream id");

    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000A"),
                                   noOwnerRemoteAudio,
                                   ours,
                                   true) == RemoteAudioRxAction::Adopted,
          "remote_audio_rx without owner is adopted for legacy single-client status");
    check(state.streamId == 0x0400000A, "legacy remote_audio_rx adoption records stream id");

    state = {};
    state.createPending = true;
    QMap<QString, QString> ourRemoteAudio{
        {QStringLiteral("type"), QStringLiteral("remote_audio_rx")},
        {QStringLiteral("client_handle"), QStringLiteral("0x12345678")},
        {QStringLiteral("compression"), QStringLiteral("NONE")}
    };
    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000B"),
                                   ourRemoteAudio,
                                   ours,
                                   false) == RemoteAudioRxAction::Adopted,
          "owned remote_audio_rx status before create callback is adopted");
    check(state.streamId == 0x0400000B, "owned remote_audio_rx status records stream id");
    check(!state.createPending, "owned remote_audio_rx status clears create pending");
    check(state.clientHandle == ours, "owned remote_audio_rx status records owner");
    check(state.compression == QStringLiteral("NONE"), "owned remote_audio_rx status records compression");

    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000B"),
                                   QMap<QString, QString>{},
                                   ours,
                                   false) == RemoteAudioRxAction::Updated,
          "known remote_audio_rx can apply later status without type or owner");

    check(applyRemoteAudioRxStatus(state,
                                   QStringLiteral("stream 0x0400000B removed"),
                                   QMap<QString, QString>{},
                                   ours,
                                   false) == RemoteAudioRxAction::Removed,
          "known remote_audio_rx removal clears stream");
    check(state.streamId == 0, "removed remote_audio_rx clears stream id");
}

void testStreamStatusOwnershipCompatibility()
{
    constexpr quint32 ours = 0x12345678;
    QMap<QString, QString> noOwner;
    QMap<QString, QString> unknownOwner{{QStringLiteral("client_handle"), QStringLiteral("0x00000000")}};
    QMap<QString, QString> orphanOwner{
        {QStringLiteral("client_handle"), QStringLiteral("0x00000000")},
        {QStringLiteral("ip"), QStringLiteral("0.0.0.0")}
    };
    QMap<QString, QString> ourOwner{{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}};
    QMap<QString, QString> otherOwner{{QStringLiteral("client_handle"), QStringLiteral("0x87654321")}};

    check(streamStatusBelongsToUs(noOwner, ours),
          "stream status without client_handle remains legacy-compatible");
    check(streamStatusBelongsToUs(unknownOwner, ours),
          "client_handle zero without dead endpoint remains legacy-compatible");
    check(!streamStatusBelongsToUs(orphanOwner, ours),
          "client_handle zero with 0.0.0.0 endpoint is ignored as a dead stream");
    check(streamStatusBelongsToUs(ourOwner, ours),
          "stream status with our client_handle belongs to us");
    check(!streamStatusBelongsToUs(otherOwner, ours),
          "stream status with another client_handle is ignored");
    check(isDeadOrphanDaxRxStatus(orphanOwner),
          "dead DAX RX helper identifies owner-zero 0.0.0.0 endpoint");
    check(!isDeadOrphanDaxRxStatus(unknownOwner),
          "owner-zero stream without dead endpoint is not classified dead");
}

void testDaxTxStatusOwnership()
{
    constexpr quint32 ours = 0x12345678;
    constexpr quint32 current = 0x04000001;
    constexpr quint32 incoming = 0x04000002;
    QMap<QString, QString> noOwner;
    QMap<QString, QString> ownerZero{{QStringLiteral("client_handle"), QStringLiteral("0x00000000")}};
    QMap<QString, QString> ourOwner{{QStringLiteral("client_handle"), QStringLiteral("0x12345678")}};
    QMap<QString, QString> otherOwner{{QStringLiteral("client_handle"), QStringLiteral("0x87654321")}};

    check(!daxTxStatusCanUpdateLocalState(incoming, 0, noOwner, ours),
          "missing DAX TX client_handle does not steal unknown stream during startup");
    check(!daxTxStatusCanUpdateLocalState(incoming, 0, ownerZero, ours),
          "owner-zero DAX TX status does not steal unknown stream");
    check(daxTxStatusCanUpdateLocalState(incoming, 0, ourOwner, ours),
          "our DAX TX client_handle is adopted");
    check(!daxTxStatusCanUpdateLocalState(incoming, current, otherOwner, ours),
          "foreign DAX TX status does not overwrite our stream id");
    check(daxTxStatusCanUpdateLocalState(current, current, otherOwner, ours),
          "already-created DAX TX stream id can update local state");
    check(daxTxStatusCanUpdateLocalState(current, current, noOwner, ours),
          "missing owner can update an already-created DAX TX stream");
}

void testDaxTxPolicy()
{
    DaxTxPolicyContext windowsExternalRoute{
        DaxTxRequestReason::ExternalDaxRouteOnly,
        DaxTxPlatform::Windows,
        DaxTxMode::ExternalDax2,
        false,
        false
    };
    check(!evaluateDaxTxPolicy(windowsExternalRoute).allowed,
          "Windows external-DAX2 route-only policy does not create dax_tx");

    DaxTxPolicyContext windowsGenericAudio = windowsExternalRoute;
    windowsGenericAudio.reason = DaxTxRequestReason::GenericAudioRecreate;
    check(!evaluateDaxTxPolicy(windowsGenericAudio).allowed,
          "Windows generic audio recreation policy does not create dax_tx");

    DaxTxPolicyContext hostedBridge{
        DaxTxRequestReason::HostedDaxBridge,
        DaxTxPlatform::MacOS,
        DaxTxMode::HostedDax,
        true,
        true
    };
    check(evaluateDaxTxPolicy(hostedBridge).allowed,
          "hosted DAX bridge policy creates dax_tx when hosted DAX is available");

    DaxTxPolicyContext hostedTci = hostedBridge;
    hostedTci.reason = DaxTxRequestReason::TciTxAudio;
    check(evaluateDaxTxPolicy(hostedTci).allowed,
          "TCI DAX TX policy is explicit and allowed when supported");

    DaxTxPolicyContext windowsTci = windowsExternalRoute;
    windowsTci.reason = DaxTxRequestReason::TciTxAudio;
    check(evaluateDaxTxPolicy(windowsTci).allowed,
          "Windows external-DAX2 TCI policy creates its own dax_tx stream (#2276)");

    // Linux non-PipeWire — same regression class as Windows. The TciTxAudio
    // policy must allow regardless of hosted-DAX availability.
    DaxTxPolicyContext linuxNoBridgeTci{
        DaxTxRequestReason::TciTxAudio,
        DaxTxPlatform::Linux,
        DaxTxMode::None,
        false,
        false
    };
    check(evaluateDaxTxPolicy(linuxNoBridgeTci).allowed,
          "Linux without hosted DAX still creates its own dax_tx stream for TCI (#2276)");

    // RadeModemTx — regression guard for #2343.  RADE sends VITA-49 modem
    // audio directly (never touches Windows audio devices), so it must be
    // allowed to register its own dax_tx stream slot even when SmartSDR DAX2
    // owns the Windows audio-device layer (ExternalDax2 mode).
    DaxTxPolicyContext windowsRade = windowsExternalRoute;
    windowsRade.reason = DaxTxRequestReason::RadeModemTx;
    check(evaluateDaxTxPolicy(windowsRade).allowed,
          "Windows ExternalDax2 RadeModemTx policy creates its own dax_tx stream (#2343)");

    // Linux without hosted DAX — RadeModemTx must also be unconditionally allowed.
    DaxTxPolicyContext linuxNoBridgeRade{
        DaxTxRequestReason::RadeModemTx,
        DaxTxPlatform::Linux,
        DaxTxMode::None,
        false,
        false
    };
    check(evaluateDaxTxPolicy(linuxNoBridgeRade).allowed,
          "Linux without hosted DAX still creates its own dax_tx stream for RadeModemTx (#2343)");
}

void testUdpRegistrationPolicy()
{
    check(isUdpPortInUseError(kFlexUdpPortInUseCode, QString()),
          "UDP port collision helper matches Flex error code");
    check(isUdpPortInUseError(1, QStringLiteral("Port/IP pair already in use")),
          "UDP port collision helper matches radio error body");
    check(isUdpPortInUseError(1, QStringLiteral("port/ip PAIR already IN use")),
          "UDP port collision helper is case-insensitive");
    check(!isUdpPortInUseError(1, QStringLiteral("command syntax error")),
          "UDP port collision helper ignores unrelated errors");
    check(shouldRetryLanUdpPortRegistration(false, kFlexUdpPortInUseCode, QString()),
          "LAN UDP registration retries on port collision");
    check(!shouldRetryLanUdpPortRegistration(true, kFlexUdpPortInUseCode, QString()),
          "WAN UDP registration does not trigger LAN rebind policy");
}

void testCreateResponseParsing()
{
    check(parseCreateResponseStreamId(QStringLiteral("4000009")) == 0x04000009,
          "numeric-only bare create response stream id parses as hex");
    check(parseCreateResponseStreamId(QStringLiteral("400000A")) == 0x0400000A,
          "alpha bare create response stream id parses as hex");
    check(parseCreateResponseStreamId(QStringLiteral("stream=0x0400000B")) == 0x0400000B,
          "keyed create response stream id parses");
    check(parseCreateResponseStreamId(QStringLiteral("stream=4000009")) == 0x04000009,
          "numeric-only keyed create response stream id parses as hex");
    check(streamCommandId(0x0400000A) == QStringLiteral("0400000a"),
          "stream command id is normalized for stream remove");
    check(parsePanafallCreatePanId(QStringLiteral("0x40000000,0x42000000"))
              == QStringLiteral("0x40000000"),
          "panafall create comma response returns only the pan id");
    check(parsePanafallCreatePanId(QStringLiteral("40000000,0x42000000"))
              == QStringLiteral("0x40000000"),
          "panafall create bare comma response normalizes only the pan id");
    check(parsePanafallCreatePanId(QStringLiteral("pan=0x40000001 waterfall=0x42000001"))
              == QStringLiteral("0x40000001"),
          "panafall create keyed response returns pan id");
}

void testBoundedSliceCapacity()
{
    check(boundedSliceCapacity(8, 8, 8, 2) == 8,
          "slice capacity does not inflate above model limit");
    check(boundedSliceCapacity(8, 10, 8, 2) == 8,
          "stale inflated slice capacity is clamped back to model limit");
    check(boundedSliceCapacity(8, 10, 1, 2) == 8,
          "stale inflated slice capacity is clamped when reported total is below model limit");
    check(boundedSliceCapacity(8, 10, 1, -1) == 8,
          "stale inflated slice capacity is clamped when reported availability is invalid");
    check(boundedSliceCapacity(4, 10, 1, 2) == 4,
          "slice capacity clamps to lower model limits");
    check(boundedSliceCapacity(8, 4, 3, 5) == 8,
          "reported available slices can raise capacity up to model limit");
    check(boundedSliceCapacity(4, 4, 1, -1) == 4,
          "negative reported slice availability is ignored");
    check(boundedSliceCapacity(0, 4, 8, 2) == 4,
          "slice capacity does not change before model limit is known");
}

} // namespace

int main()
{
    std::printf("Radio status ownership tests\n\n");
    testPanadapterOwnershipDecisions();
    testRemoteAudioRxTracking();
    testStreamStatusOwnershipCompatibility();
    testDaxTxStatusOwnership();
    testDaxTxPolicy();
    testUdpRegistrationPolicy();
    testCreateResponseParsing();
    testBoundedSliceCapacity();

    if (failures) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }

    std::printf("\nAll tests passed\n");
    return 0;
}
