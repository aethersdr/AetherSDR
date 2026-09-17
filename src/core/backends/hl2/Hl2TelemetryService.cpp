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
// deliberately not borrowed from anything that stops when a connection does —
// that mistake is why this class exists at all, one level down.
constexpr int kStateIntervalMs = 1000;

// THIS FAMILY DECLARES ITSELF. The only place the string "hl2" appears in the
// stream-free telemetry feature outside src/core/backends/hl2/ is nowhere --
// shared code asks OfflineHealthRegistry whether the selected family declared
// anything, and this is the declaration.
//
// LINKAGE, because a self-registering translation unit that nothing references
// can be dropped from a static archive with no error anywhere and the feature
// then simply does not exist. This TU is reached: Hl2Backend.cpp names
// Hl2TelemetryService in setOfflineHealthSource()'s dynamic_cast, and
// RadioModel::makeBackend names hl2::Hl2Backend. offline_health_registry_test
// asserts the declaration is actually present rather than trusting that chain.
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
    // A REPLY FROM A SESSION WE HAVE LEFT IS OUR OWN VOICE COMING BACK.
    //
    // While connected and stalled the poller still runs, and the `run` bit in
    // the reply it caches is set BY US — so `reply->streaming` is true and the
    // reply describes our own session. setTarget() drops the cache only when
    // the target CHANGES, and a plain disconnect leaves the address alone. The
    // next `health` read then sees `!connected && reply && reply->streaming`
    // and reports HeldByOther, rendering `radioInUse: true` for a radio nobody
    // is using. It self-corrects on the next reply, but HeldByOther is
    // demand-gated, so "the next reply" may be after the operator has already
    // read the wrong answer (aethersdr-agent, #5642 review).
    //
    // Leaving Streaming or StreamStalled is exactly the transition that ends
    // the session the cached reply belongs to, so that is where it is dropped.
    // The age clock goes with it: an age that outlives its reading would say a
    // stale figure is fresh.
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
    // Apply it NOW rather than waiting for the next tick. The first health read
    // from a cold start would otherwise wait a full second before the poller
    // was even told anyone was watching, and then another interval before the
    // first datagram — which reads to a caller as "the feature does nothing".
    d->poller->setSurfaceVisible(true);
    d->poller->setLinkState(d->state);
}

std::optional<DiscoveryReply> Hl2TelemetryService::lastReply() const
{
    return d->reply;
}

bool Hl2TelemetryService::hasOfflineTarget() const
{
    // The MIRRORED target, not the poller's destination: the poller can also
    // choose a broadcast address when the fallback is opted in, and "somebody
    // named a radio" is a different claim from "something would be sent".
    return !d->target.isNull();
}

IRadioBackend::HealthSnapshot Hl2TelemetryService::healthRows() const
{
    IRadioBackend::HealthSnapshot h;
    // A SECTION IS A GROUP HEADING, NOT A TAG ON EVERY ROW.
    // docs/automation-bridge.md states the contract for `health`: "section
    // appears on the first row of each group and is absent on the rest", and
    // RadioHealthDialog::refresh() draws a bold header row for any key that
    // carries one. Stamping every key drew eleven repeated headers interleaved
    // with nearly every row -- latent until this PR, because nothing
    // constructed the service. Reported by ten9876 on #5642.
    //
    // `pendingSection` is emptied by the put() that consumes it, so a group
    // leader is whichever row happens to come first. That matters: the readings
    // below are conditional on a reply having arrived, so the row that leads
    // them is not a fixed key.
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
        // An INVALID variant is left out of `values` entirely, which is how the
        // snapshot spells "never reported" — the bridge renders that as JSON
        // null. Inserting a default-constructed value instead would turn "we
        // were never told" into "the reading is zero", which is the single most
        // misleading thing a health readout can do.
        if (v.isValid())
            h.values.insert(k, v);
    };

    const bool polling = d->poller->currentIntervalMs() > 0;

    // Which path produced the readings. This class only ever has stream-free
    // ones, so it can answer `port-1025` or `none` and never `in-band` — and
    // the backend's row, which CAN, overrides this at the merge point when it
    // has in-band values.
    //
    // That override is now implemented. It was not when this comment first
    // claimed it: the backend had stopped publishing the key, so this value
    // always won and `in-band` was unreachable. A comment asserting a mechanism
    // that has been removed reads as a reason not to check, which is why the
    // rule now lives in one shared function both sides call.
    put("telemetrySource", QStringLiteral("Source"),
        hl2TelemetrySource(/*connected=*/false, /*haveInBand=*/false,
                           /*haveStreamFree=*/d->reply.has_value()));

    // Absent until something has actually arrived. A frozen reading and a fresh
    // one render identically without this, and frozen is the failure this
    // feature exists to catch.
    put("telemetryAgeMs", QStringLiteral("Stream-free reading age (ms)"),
        (d->reply && d->at.isValid())
            ? QVariant(static_cast<qlonglong>(d->at.elapsed())) : QVariant());

    // The third state. `null` already means "the radio never reported this
    // field"; without a count, "we asked and heard nothing" is
    // indistinguishable from "we never asked" — one is a fault to chase, the
    // other is the poller correctly idle. Absent when not polling, because a
    // count of zero while silent would read as "asking, all fine".
    put("telemetryUnanswered", QStringLiteral("Unanswered polls"),
        polling ? QVariant(d->unanswered) : QVariant());

    // What the cadence rule decided, so it is visible rather than inferred from
    // a packet capture. 0 = deliberately silent.
    put("telemetryPollMs", QStringLiteral("Poll interval (ms), 0 = not polling"),
        d->poller->currentIntervalMs());

    // ---- the readings themselves ----
    //
    // Same keys and the SAME RAW UNITS the in-band path uses, deliberately: the
    // two decode into comparable fields so a consumer choosing between them is
    // choosing a source, never applying a conversion. The backend's in-band row
    // overrides these key-for-key at the merge point when it has a value, so
    // these are what an operator sees exactly when the in-band path has
    // nothing — disconnected, stalled, or someone else holding the radio.
    auto reading = [&](const char* key, const QString& label, auto opt) {
        put(key, label, opt ? QVariant(*opt) : QVariant());
    };
    if (d->reply) {
        // Second group: the numbers, as opposed to the attribution rows above.
        pendingSection = QStringLiteral("Stream-free readings");
        const DiscoveryReply& r = *d->reply;
        reading("temperatureRaw",  QStringLiteral("Temperature (raw counts)"), r.temperatureRaw);
        // THE ROW A HUMAN READS, and until now the stream-free path could not
        // fill it. Hl2Backend gates its own `temperatureC` on the in-band path
        // actually delivering, which is right -- a smoothed figure with no age
        // of its own must not outlive the stream that fed it. But nothing took
        // the row over, so the three states this class exists for rendered
        // "PA temperature (°C): —" next to a four-digit raw count, and the
        // feature's own headline question went unanswered (#5642 review).
        //
        // NOT SMOOTHED, unlike the in-band row: each poll is one reading and
        // there is no cadence to average over. It is the same conversion the
        // in-band path uses (MetisProtocol.h), so the two cannot disagree about
        // what a count means, and the in-band row still WINS the merge whenever
        // the stream is live.
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
        // The radio's own view of whether somebody is streaming from it — and
        // WHO that somebody is, is a question this bit cannot answer.
        //
        // The original comment here said "on this path that somebody is not
        // us". That is true in NotConnected and HeldByOther, and false in
        // exactly the state this feature exists to diagnose: while connected
        // and stalled, the poller still runs and the `run` bit in the reply it
        // caches was set BY US. Publishing it then told the operator another
        // client held the radio during their own stalled session, and nothing
        // corrected it — Hl2Backend::healthSnapshot() publishes no radioInUse
        // key at all, so this value always survived the merge. Reported by
        // ten9876 on #5642.
        //
        // The honest answer while our own session owns the stream is to say
        // NOTHING. An absent key is "we were never told"; a false would be a
        // claim we cannot support, since another client could also be
        // streaming and this bit would look identical. leftOurOwnSession()
        // above drops the cache when the session ENDS, which is the same
        // reasoning one transition later — it just never fired mid-stall.
        const bool streamIsOurs = d->state == Hl2LinkState::Streaming
                               || d->state == Hl2LinkState::StreamStalled;
        put("radioInUse", QStringLiteral("In use by another client"),
            streamIsOurs ? QVariant() : QVariant(r.streaming));
        // ptt_hang_time, because 31 does not mean "the longest hang" -- it
        // disables the gateware's PTT auto-unkey entirely (softerhardware/
        // Hermes-Lite2 #178). Being able to read that WITHOUT a stream is how an
        // operator learns their radio's own dead-man's switch is off before
        // keying rather than after.
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
