#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <atomic>
#include <complex>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace AetherSDR {

// A backend-neutral source of canonical receiver IQ (#4955).
//
// WHAT THIS SEAM IS FOR
//
// A TCI skimmer (SDC / CW Skimmer) needs three things from a radio: complex
// samples for a named receiver, the rate they actually arrive at, and the
// frequency they are centred on. None of those are FlexRadio concepts, but the
// only implementation we had was: create a Flex `dax_iq` stream, bind a pan to
// a DAX-IQ channel, decode a VITA-49 class code, byte-swap a little-endian
// payload. A radio family that already produces perfectly good IQ — the HL2
// demultiplexes one block per DDC on every EP6 packet — could not serve a
// skimmer without impersonating a Flex.
//
// So the contract below is written in terms of what a consumer needs, and each
// family adapts its own transport to it exactly once, at the boundary.
//
// THE ORIENTATION INVARIANT
//
//     Positive complex frequency represents RF ABOVE the reported IQ centre.
//
// This is the load-bearing sentence in the file. It is not a preference: the
// HPSDR wire is the CONJUGATE of the analytic convention, so an HL2 provider
// that published raw Metis IQ would mirror every skimmer band about the DDC
// centre — 40 m FT8 reported at 7.071 instead of 7.074, which reads as a
// mistuned radio rather than a software fault. Hl2RxDsp already carries this
// scar (see its m_conjugated comment, and the demod path that deliberately
// does NOT conjugate). Every adapter normalises to the invariant above and
// every consumer may assume it.
//
// Absolute handedness is therefore established by feeding a tone at a KNOWN
// offset above and below centre and checking the sign that comes out. An
// RX/TX loopback cannot do it: matching conjugation errors at both ends cancel
// and the test passes on mirrored IQ.
//
// THREADING
//
// publishBlock() is called on whatever thread the backend produces samples on
// (the HL2's UDP I/O thread, the Flex VITA-49 reader). It never blocks: it
// appends to a BOUNDED per-endpoint queue, dropping the oldest block and
// counting the drop when a consumer falls behind, and posts one coalesced
// wake to the provider's own thread. Everything else — acquire/release/
// requestRate and the iqBlockReady signal — happens on the provider's thread.

// The transport-neutral sample. Native-endian, native alignment; endianness
// conversion belongs at the transport boundary, not in each consumer.
using IqSample = std::complex<float>;

// One contiguous run of samples from one endpoint.
//
// Handed to consumers as a shared_ptr<const> so a fan-out to four subscribers
// on the same receiver is four pointer copies, not four buffer copies.
struct IqBlock {
    QString   endpointId;        // the endpoint that produced it
    int       sampleRateHz{0};   // ACHIEVED rate, never the requested one
    long long centerHz{0};       // the DDC/pan centre these samples are about
    quint64   sequence{0};       // monotonic per endpoint, from the first block
    quint64   droppedBefore{0};  // blocks discarded by backpressure before this
                                 // one — a discontinuity marker, not a total
    std::vector<IqSample> samples;
};
using IqBlockPtr = std::shared_ptr<const IqBlock>;

// How the rate can be moved. A consumer that asks for a rate change needs to
// know whether it is asking for its own stream or for the whole radio.
enum class IqRateTopology {
    PerStream,               // each endpoint carries its own rate (Flex DAX-IQ)
    SharedAcrossReceivers,   // one rate for every receiver (HL2's DDC rate)
    FixedObserved,           // the rate is whatever it is; it cannot be set
};

// Why a lease was refused. A structured answer, so a consumer can say something
// truthful instead of acknowledging a subscription that will never deliver.
enum class IqRefusal {
    None = 0,
    NoProvider,        // the backend has no IQ source at all
    NotConnected,      // there is a provider but no live radio behind it
    UnknownEndpoint,   // no such receiver (or it went away)
    CapacityExceeded,  // the provider cannot carry another endpoint
    RateUnavailable,   // the requested rate is not reachable
    BackendRefused,    // the backend declined, with detail
};

QString iqRefusalText(IqRefusal r);

// One receiver's endpoint, as the provider currently sees it.
struct IqEndpoint {
    // The STABLE identity a consumer holds a lease on: the application's pan id
    // ("hl2-0", a Flex pan handle). Deliberately not a wire index — the HL2's
    // DDC indices are contiguous and renumber on every receiver rebuild, and a
    // lease keyed on one of those would slide onto another band unnoticed.
    QString   id;
    QString   label;             // human-readable, diagnostics only
    bool      live{false};
    int       achievedRateHz{0};
    long long centerHz{0};
};

// What the provider can do, asked once rather than assumed per family.
struct IqProviderCaps {
    int            maxConcurrentEndpoints{0};
    QVector<int>   supportedRatesHz;
    IqRateTopology rateTopology{IqRateTopology::FixedObserved};
    // True when moving the rate reconfigures radio-wide receive state (the
    // HL2's shared DDC rate re-spans every panadapter and rebuilds every DSP
    // chain). A consumer must not trigger that on a skimmer's say-so.
    bool           rateChangeIsRadioWide{false};
    // The canonical format guarantee, spelled out for the handshake.
    QString        sampleFormat{QStringLiteral("complex-float32-native")};
};

// The answer to a lease request: an active lease with the ACHIEVED rate and
// centre, or a structured refusal. Requested and achieved are never conflated.
struct IqLease {
    bool       granted{false};
    IqRefusal  refusal{IqRefusal::None};
    QString    endpointId;
    int        achievedRateHz{0};
    long long  centerHz{0};
    QString    detail;           // free text; always safe to log

    static IqLease refuse(IqRefusal r, const QString& detail = {})
    {
        IqLease l;
        l.granted = false;
        l.refusal = r;
        l.detail  = detail.isEmpty() ? iqRefusalText(r) : detail;
        return l;
    }
};

class IqProvider : public QObject {
    Q_OBJECT

public:
    explicit IqProvider(QObject* parent = nullptr);
    ~IqProvider() override;

    virtual IqProviderCaps      capabilities() const = 0;
    virtual QVector<IqEndpoint> endpoints() const = 0;

    // The current view of one endpoint, or nullopt when it is not there.
    std::optional<IqEndpoint> endpoint(const QString& endpointId) const;

    // ---- reference-counted leases (provider thread only) -----------------
    //
    // `subscriber` is an opaque identity token — a client socket, a widget,
    // whatever the consumer wants — and the SAME token acquiring twice counts
    // once. The first subscriber opens the endpoint; the last one to leave
    // closes it. A stream that was already open when the first subscriber
    // arrived is BORROWED, not owned, so releasing it detaches the tap without
    // destroying something another local consumer is using.
    IqLease acquire(const QString& endpointId, const void* subscriber,
                    int requestedRateHz = 0);
    void    release(const QString& endpointId, const void* subscriber);
    void    releaseAll(const void* subscriber);
    int     subscriberCount(const QString& endpointId) const;
    bool    isLeased(const QString& endpointId) const;

    // Ask for a rate. Returns what was ACHIEVED, which may not be what was
    // asked for — see IqRateTopology and rateChangeIsRadioWide.
    IqLease requestRate(const QString& endpointId, int requestedRateHz);

    // Backpressure telemetry: total blocks dropped for this endpoint since it
    // was first published. Nonzero means a consumer could not keep up.
    quint64 droppedBlocks(const QString& endpointId) const;

    // The bound on the per-endpoint queue. Public so a test can prove the
    // producer path is bounded rather than merely "usually short".
    static constexpr int kMaxQueuedBlocks = 64;

signals:
    // Canonical samples, on the provider's thread. Never emitted for an
    // endpoint with no subscribers, and never after a lease is invalidated.
    void iqBlockReady(const QString& endpointId, AetherSDR::IqBlockPtr block);
    // The endpoint set, a rate or a centre moved.
    void endpointsChanged();
    // This endpoint's leases are gone and will not be silently rebound. The
    // consumer must decide whether to re-acquire; nothing here does it for it,
    // because re-acquiring by wire index is exactly how a live subscription
    // ends up on another band.
    void leaseInvalidated(const QString& endpointId, AetherSDR::IqRefusal reason,
                          const QString& detail);

protected:
    // ---- adapter hooks ---------------------------------------------------
    // Called with NO subscribers present, to bring the endpoint up. Returns the
    // achieved rate/centre or a refusal.
    virtual IqLease openEndpoint(const QString& endpointId, int requestedRateHz) = 0;
    // Called after the LAST subscriber leaves. An adapter that borrowed a
    // stream it did not create must detach rather than destroy.
    virtual void    closeEndpoint(const QString& endpointId) = 0;
    // Rate negotiation for an already-open endpoint. The default implementation
    // changes nothing and reports the current achieved rate, which is the
    // correct answer for a fixed-rate or radio-wide-rate backend.
    virtual IqLease adjustRate(const QString& endpointId, int requestedRateHz);

    // Record the achieved rate/centre an adapter observed. Emits
    // endpointsChanged() when either actually moved.
    void noteAchieved(const QString& endpointId, int achievedRateHz, long long centerHz);

    // Producer-thread ingress. Non-blocking, bounded, drop-oldest.
    void publishBlock(const QString& endpointId, int sampleRateHz, long long centerHz,
                      std::vector<IqSample>&& samples);

    // Drop every lease on this endpoint (it vanished, or the radio went away).
    void invalidateEndpoint(const QString& endpointId, IqRefusal reason,
                            const QString& detail = {});
    void invalidateAllEndpoints(IqRefusal reason, const QString& detail = {});

private:
    void drainQueues();

    struct Achieved {
        int       rateHz{0};
        long long centerHz{0};
    };
    struct Queue {
        std::deque<IqBlockPtr> blocks;
        quint64 sequence{0};
        quint64 droppedTotal{0};
        quint64 droppedSinceDelivery{0};
    };

    mutable std::mutex m_leaseMutex;
    QHash<QString, QSet<const void*>> m_subscribers;   // endpoint → tokens
    QHash<QString, Achieved>          m_achieved;

    mutable std::mutex m_queueMutex;
    QHash<QString, Queue>             m_queues;
    std::atomic<bool>                 m_drainPosted{false};
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::IqBlockPtr)
Q_DECLARE_METATYPE(AetherSDR::IqRefusal)
