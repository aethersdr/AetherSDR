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
}  // namespace

struct Hl2TelemetryService::Impl {
    Hl2TelemetryPoller* poller = nullptr;
    Hl2LinkState state = Hl2LinkState::NotConnected;
    std::optional<DiscoveryReply> reply;
    QElapsedTimer at;          // when `reply` arrived
    int unanswered = 0;
    QElapsedTimer demand;      // when the snapshot was last read
    int linkStateUpdates = 0;  // proof that something drives this
};

Hl2TelemetryService::Hl2TelemetryService(QObject* parent)
    : QObject(parent)
    , d(new Impl)
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

Hl2TelemetryService::~Hl2TelemetryService()
{
    delete d;
}

void Hl2TelemetryService::setTarget(const QHostAddress& addr)
{
    if (addr.isNull()) {
        // Whatever we last read belonged to the radio we are no longer pointed
        // at. Forget it rather than letting a stale reading outlive its subject.
        d->reply.reset();
        d->at.invalidate();
        d->unanswered = 0;
    }
    d->poller->setTarget(addr);
}

void Hl2TelemetryService::setExpectedMac(const std::array<std::uint8_t, 6>& mac)
{
    d->poller->setExpectedMac(mac);
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

IRadioBackend::HealthSnapshot Hl2TelemetryService::healthRows() const
{
    IRadioBackend::HealthSnapshot h;
    auto put = [&h](const char* key, const QString& label, const QVariant& v) {
        const QString k = QString::fromLatin1(key);
        h.order.push_back(k);
        h.labels.insert(k, label);
        h.sections.insert(k, QStringLiteral("Telemetry source"));
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
        const DiscoveryReply& r = *d->reply;
        reading("temperatureRaw",  QStringLiteral("Temperature (raw counts)"), r.temperatureRaw);
        reading("forwardPowerRaw", QStringLiteral("Forward (raw counts)"),     r.forwardPowerRaw);
        reading("reversePowerRaw", QStringLiteral("Reverse (raw counts)"),     r.reversePowerRaw);
        reading("biasCurrentRaw",  QStringLiteral("PA bias (raw counts)"),     r.biasCurrentRaw);
        reading("txFifoFillMsbs",  QStringLiteral("TX FIFO fill (0-127, coarse)"),
                r.txFifoFillMsbs);
        reading("txFifoRecovery",  QStringLiteral("TX pacing fault (under OR overrun)"),
                r.txFifoRecovery);
        reading("ptt",             QStringLiteral("PTT (radio)"),              r.ptt);
        // The radio's own view of whether somebody is streaming from it. On this
        // path that somebody is not us, which is the case A-telemetry is about.
        put("radioInUse", QStringLiteral("In use by another client"),
            QVariant(r.streaming));
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
