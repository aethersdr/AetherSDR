#include "core/backends/hl2/Hl2TelemetryService.h"

#include "core/backends/hl2/Hl2TelemetryPoller.h"
#include "core/backends/hl2/Hl2TelemetrySource.h"

#include <QTimer>

namespace AetherSDR::hl2 {

namespace {
// How long a health read keeps the poller interested once nothing is asking.
// Long enough that a 1 Hz consumer never sees a gap, short enough that a closed
// dialog stops the traffic promptly.
constexpr qint64 kDemandWindowMs = 5000;
// How often the poller's link state and demand are re-evaluated. Its own tick,
// never one that stops with a connection.
constexpr int kStateIntervalMs = 1000;

// The family declares itself to OfflineHealthRegistry; shared code never names
// "hl2". A self-registering TU nothing references can be dropped from a static
// archive silently; this one is reached via Hl2Backend.cpp's dynamic_cast to
// Hl2TelemetryService, and offline_health_registry_test asserts the declaration.
[[maybe_unused]] const bool kRegisteredWithOfflineHealthRegistry = [] {
    OfflineHealthRegistry::declare(
        QStringLiteral("hl2"),
        [](QObject* parent) -> std::unique_ptr<IOfflineHealthSource> {
            return std::make_unique<Hl2TelemetryService>(parent);
        });
    return true;
}();
}  // namespace

struct Hl2TelemetryService::Impl {
    Hl2TelemetryPoller* poller = nullptr;
    QHostAddress target;       // mirrored so a CHANGE of radio can be detected
    Hl2LinkState state = Hl2LinkState::NotConnected;
    std::optional<DiscoveryReply> reply;
    QElapsedTimer at;          // when `reply` arrived
    int unanswered = 0;
    QElapsedTimer demand;      // when the snapshot was last read
    int linkStateUpdates = 0;  // proof that something drives this
};

// Declared in the header; defined here because Impl is only complete in this
// translation unit.
void Hl2TelemetryServiceTestAccess::placeReply(Hl2TelemetryService& svc,
                                               const DiscoveryReply& r)
{
    svc.d->reply = r;
    svc.d->at.start();
}

Hl2TelemetryService::Hl2TelemetryService(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->poller = new Hl2TelemetryPoller(this);

    connect(d->poller, &Hl2TelemetryPoller::readingReceived, this,
            [this](const DiscoveryReply& r, qint64 /*ageMs*/) {
                d->reply = r;
                d->unanswered = 0;
                // Age runs from ARRIVAL, not from the round trip: what a reader
                // needs is how stale the number on screen is, and that clock
                // starts when we received it.
                d->at.restart();
            });
    connect(d->poller, &Hl2TelemetryPoller::pollUnanswered, this,
            [this](int consecutive) { d->unanswered = consecutive; });

    auto* tick = new QTimer(this);
    tick->setInterval(kStateIntervalMs);
    connect(tick, &QTimer::timeout, this, [this] {
        const bool wanted = d->demand.isValid() && d->demand.elapsed() < kDemandWindowMs;
        d->poller->setSurfaceVisible(wanted);
        d->poller->setLinkState(d->state);
    });
    tick->start();
}

// Out of line, and it has to be: Impl is incomplete in the header, so the
// implicit destructor there could not delete it.
Hl2TelemetryService::~Hl2TelemetryService() = default;

void Hl2TelemetryService::setTarget(const QHostAddress& addr)
{
    if (d->target != addr) {
        // Whatever we last read belonged to the radio we are no longer pointed
        // at. Forget it rather than letting a stale reading outlive its subject.
        //
        // ON ANY CHANGE, not only on a null address. Clearing only for "off"
        // meant re-aiming from one radio to another kept the first radio's
        // temperature, power and PTT and re-published them under the second
        // radio's rows -- with a fresh `telemetryAgeMs` clock, because the age
        // runs from arrival and nothing had invalidated it.
        d->target = addr;
        d->reply.reset();
        d->at.invalidate();
        d->unanswered = 0;
    }
    d->poller->setTarget(addr);
}

void Hl2TelemetryService::setAllowBroadcastFallback(bool allow)
{
    d->poller->setAllowBroadcastFallback(allow);
}

int Hl2TelemetryService::linkStateUpdateCount() const noexcept
{
    return d->linkStateUpdates;
}

void Hl2TelemetryService::setLinkState(Hl2LinkState state)
{
    ++d->linkStateUpdates;
    // A reply cached during our own session has `run` set by us. Drop it (and
    // its age clock) when leaving Streaming/StreamStalled, or a disconnected
    // read would report HeldByOther / radioInUse for a radio nobody is using.
    const bool leftOurOwnSession =
        (d->state == Hl2LinkState::Streaming
         || d->state == Hl2LinkState::StreamStalled)
        && state != Hl2LinkState::Streaming
        && state != Hl2LinkState::StreamStalled;
    if (leftOurOwnSession) {
        d->reply.reset();
        d->at.invalidate();
    }
    d->state = state;
    d->poller->setLinkState(state);
}

void Hl2TelemetryService::noteDemand()
{
    d->demand.restart();
    // Apply now, not on the next tick, so a cold-start health read polls
    // without a one-second delay.
    d->poller->setSurfaceVisible(true);
    d->poller->setLinkState(d->state);
}

std::optional<DiscoveryReply> Hl2TelemetryService::lastReply() const
{
    return d->reply;
}

bool Hl2TelemetryService::hasOfflineTarget() const
{
    // The mirrored target, not the poller's destination (which may be a
    // broadcast): "a radio was named" is not "something would be sent".
    return !d->target.isNull();
}

IRadioBackend::HealthSnapshot Hl2TelemetryService::healthRows() const
{
    IRadioBackend::HealthSnapshot h;
    // A section goes on the first row of each group only
    // (docs/automation-bridge.md `health` contract; RadioHealthDialog draws a
    // header per section key). put() consumes `pendingSection`, so the leader
    // is whichever row comes first, since the readings are conditional.
    QString pendingSection = QStringLiteral("Telemetry source");
    auto put = [&h, &pendingSection](const char* key, const QString& label,
                                     const QVariant& v) {
        const QString k = QString::fromLatin1(key);
        h.order.push_back(k);
        h.labels.insert(k, label);
        if (!pendingSection.isEmpty()) {
            h.sections.insert(k, pendingSection);
            pendingSection.clear();
        }
        // An invalid variant is omitted: "never reported" (JSON null), never
        // a zero.
        if (v.isValid())
            h.values.insert(k, v);
    };

    const bool polling = d->poller->currentIntervalMs() > 0;

    // Only `port-1025` or `none` from here; the backend's row (`in-band`)
    // overrides at the merge point. Both sides call hl2TelemetrySource().
    put("telemetrySource", QStringLiteral("Source"),
        hl2TelemetrySource(/*connected=*/false, /*haveInBand=*/false,
                           /*haveStreamFree=*/d->reply.has_value()));

    // Absent until something has actually arrived. A frozen reading and a fresh
    // one render identically without this, and frozen is the failure this
    // feature exists to catch.
    put("telemetryAgeMs", QStringLiteral("Stream-free reading age (ms)"),
        (d->reply && d->at.isValid())
            ? QVariant(static_cast<qlonglong>(d->at.elapsed())) : QVariant());

    // Separates "asked, heard nothing" from "never asked". Absent when not
    // polling, where 0 would read as "asking, all fine".
    put("telemetryUnanswered", QStringLiteral("Unanswered polls"),
        polling ? QVariant(d->unanswered) : QVariant());

    // What the cadence rule decided, so it is visible rather than inferred from
    // a packet capture. 0 = deliberately silent.
    put("telemetryPollMs", QStringLiteral("Poll interval (ms), 0 = not polling"),
        d->poller->currentIntervalMs());

    // Same keys and raw units as the in-band path, which overrides these
    // key-for-key at the merge point whenever it has a value.
    auto reading = [&](const char* key, const QString& label, auto opt) {
        put(key, label, opt ? QVariant(*opt) : QVariant());
    };
    if (d->reply) {
        // Second group: the numbers, as opposed to the attribution rows above.
        pendingSection = QStringLiteral("Stream-free readings");
        const DiscoveryReply& r = *d->reply;
        reading("temperatureRaw",  QStringLiteral("Temperature (raw counts)"), r.temperatureRaw);
        // Unsmoothed (one reading per poll), same conversion as in-band
        // (MetisProtocol.h); Hl2Backend's smoothed row wins while it streams.
        put("temperatureC", QStringLiteral("PA temperature (°C)"),
            r.temperatureRaw ? QVariant(hl2TemperatureCelsius(*r.temperatureRaw))
                             : QVariant());
        reading("forwardPowerRaw", QStringLiteral("Forward (raw counts)"),     r.forwardPowerRaw);
        reading("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),     r.reversePowerRaw);
        reading("biasCurrentRaw",  QStringLiteral("PA bias (raw counts)"),     r.biasCurrentRaw);
        reading("txFifoFillMsbs",  QStringLiteral("TX FIFO fill (0-127, coarse)"),
                r.txFifoFillMsbs);
        reading("txFifoRecovery",  QStringLiteral("TX pacing fault (under OR overrun)"),
                r.txFifoRecovery);
        reading("ptt",             QStringLiteral("PTT (radio)"),              r.ptt);
        // The `run` bit says someone streams, not who. While our own session
        // holds the stream (incl. stalled) the bit may be ours, so omit the key;
        // Hl2Backend publishes no radioInUse, so this value survives the merge.
        const bool streamIsOurs = d->state == Hl2LinkState::Streaming
                               || d->state == Hl2LinkState::StreamStalled;
        put("radioInUse", QStringLiteral("In use by another client"),
            streamIsOurs ? QVariant() : QVariant(r.streaming));
        // 31 disables the gateware's PTT auto-unkey entirely, not "longest
        // hang" (softerhardware/Hermes-Lite2 #178); readable before keying.
        reading("pttHangTimeMs", QStringLiteral("PTT hang time (ms), 31 = auto-unkey DISABLED"),
                r.pttHangTimeMs);
        // adcClipCount is deliberately NOT published here. Only an EP6 packet
        // clears it, so off-stream it latches at its maximum after one
        // historical clip, and while another client streams it is cleared on a
        // cadence we neither see nor control. Either way the number would
        // describe someone else's window. See docs/architecture/
        // hl2-stream-free-telemetry.md.
    }

    return h;
}

}  // namespace AetherSDR::hl2
