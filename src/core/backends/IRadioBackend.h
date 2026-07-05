#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "core/backends/RadioCapabilities.h"

namespace AetherSDR {

// Neutral, family-agnostic connect descriptor. Core fields cover the common
// case; vendor-specific parameters (SmartLink token, Kiwi endpoint path, …)
// ride in `params` so the interface never grows a per-vendor connect signature.
struct RadioConnectRequest {
    QString host;
    quint16 port = 0;
    QString serial;         // when a family identifies radios by serial
    QVariantMap params;     // family-specific extras (namespaced by the backend)
};

// The radio-facing seam of the engine (aetherd RFC §5.5). Everything that
// speaks a vendor wire protocol lives *behind* this interface, inside
// libaethercore; RadioModel and the (future) protocol see only this. The
// SmartSDR stack becomes the first implementor (FlexBackend, step 2.2) with
// ZERO behavior change; other radio families are added as further implementors
// without touching any client.
//
// Design decisions (RFC §5.5 open questions, resolved 2026-07-05):
//   Q2 — RadioModel keeps owning its sub-models (SliceModel, MeterModel, …);
//        the backend DRIVES them by emitting the signals below, which
//        RadioModel connects to. The backend does not own UI-facing model
//        objects. (Minimal churn; the models already communicate via signals.)
//   Q3 — ONE interface, not an RX-only/TX-capable type split. A receive-only
//        family reports capabilities().canTransmit == false and implements
//        setKeying() as a guarded no-op; the engine TX guard (RFC §6, above
//        this seam) denies keying when canTransmit is false.
//
// DSP location is invisible here (RFC §5.5): a backend whose hardware
// demodulates and computes FFTs is a thin protocol decoder; one that ships raw
// samples owns an engine-side DSP chain — either way it emits the same
// normalized signals, so no consumer can tell the difference.
//
// This is the CORE seed. It carries the lifecycle, capability, canonical-verb,
// and extension surface; it grows one method at a time as the touchpoint
// burndown (docs/architecture/aetherd-touchpoints.md) converts each gui→engine
// touchpoint into a protocol/backend verb. Do NOT dump all 140 touchpoints
// here at once.
class IRadioBackend : public QObject {
    Q_OBJECT

public:
    explicit IRadioBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~IRadioBackend() override = default;

    // ---- identity & capability (feeds the protocol `welcome`, §4.1) ----
    virtual RadioCapabilities capabilities() const = 0;

    // ---- connection lifecycle ----
    virtual void connectRadio(const RadioConnectRequest& request) = 0;
    virtual void disconnectRadio() = 0;
    virtual bool isConnected() const = 0;

    // ---- intents DOWN: canonical core-profile verbs (grow per burndown) ----
    // The backend translates each to its vendor wire protocol.
    virtual void setSliceFrequency(int sliceId, double hz) = 0;
    virtual void setSliceMode(int sliceId, const QString& mode) = 0;
    virtual void setSliceFilter(int sliceId, int lowHz, int highHz) = 0;

    // TX keying intent. The decision to allow keying is made ABOVE this seam by
    // the engine guard (RFC §6, single-holder lock + capability check); the
    // backend only translates an already-authorized intent to its mechanism
    // (command verb, in-stream bit, hardware line). A backend whose
    // capabilities().canTransmit is false implements this as a no-op.
    virtual void setKeying(bool key) = 0;

    // ---- vendor extensions (namespaced, capability-advertised) ----
    // Vendor-specific verbs that are NOT part of the core profile. Clients
    // discover available namespaces via capabilities().extensionNamespaces.
    //
    // Fire-and-forget like every other DOWN verb: the result of a real device
    // command (ATU tune, amp state, …) arrives later on the wire, so it comes
    // back asynchronously via extensionResult(requestId, …) / extensionError.
    // The caller (above the seam) mints requestId and correlates the reply; a
    // requestId of 0 means "no reply expected". A synchronous QVariant return
    // would have to block or fabricate a local value against an async backend.
    virtual void invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant& arg = {}) = 0;

signals:
    // ---- connection state UP ----
    void connected();
    void disconnected();
    void connectionError(const QString& reason);
    void capabilitiesChanged();

    // ---- vendor-extension replies UP (correlate to invokeExtension) ----
    // The async result of an invokeExtension() call, keyed by the caller's
    // requestId. A backend that completes locally may emit this synchronously;
    // one speaking a wire protocol emits it when the device answers.
    void extensionResult(quint64 requestId, const QVariant& result);
    void extensionError(quint64 requestId, const QString& reason);

    // ---- normalized model state UP (RadioModel connects these to its
    //      sub-models; Q2) — the key/value shape mirrors the models' fields ----
    void sliceChanged(int sliceId, const QVariantMap& changes);
    void sliceRemoved(int sliceId);
    void meterUpdate(const QString& meterId, double value);
    // Panadapter core display state (universal — every family has a pan center
    // and span). The backend decodes it from vendor status; RadioModel drives
    // the PanadapterModel. panId is the pan's identifier (opaque to the model).
    // (aetherd RFC 2.3 — first converted touchpoint; the template the other
    // universal pan fields + the other mixed models follow.)
    void panCenterBandwidthChanged(const QString& panId,
                                   double centerMhz, double bandwidthMhz);

    // Vendor-specific status data that is NOT part of the core profile — the
    // namespaced *extension* channel (aetherd RFC §5.5). A client that doesn't
    // understand `ns` ignores it; `kind` names the event within the namespace
    // and `fields` carries only the keys the wire actually reported. This is the
    // status counterpart to invokeExtension's request/reply.
    void extensionStatus(const QString& ns, const QString& kind,
                         const QVariantMap& fields);

    // ---- data plane UP (RFC §4.2) ----
    // Declared here so backends have a normalized outlet for spectrum/waterfall/
    // audio; the concrete zero-copy/binary frame formats are step-4 work. Until
    // then a backend may relay the existing in-tree frame types.
    void spectrumFrameReady(int panId, const QByteArray& frame);
    void waterfallRowReady(int panId, const QByteArray& row);
    void audioFrameReady(const QByteArray& pcm);
};

}  // namespace AetherSDR
