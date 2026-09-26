#include "core/AetherClockEngine.h"

#include "core/WwvDecoder.h"
#include "core/WwvbDecoder.h"
#include "core/ClockSampleTimeline.h"
#include "core/DecoderPcmAdapter.h"
#include "models/SliceModel.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QTimeZone>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <utility>
#include <vector>

namespace AetherSDR {

namespace {

// Absolute-plausibility bound handed to the shared voter (WS-4.5): a voted
// timestamp farther than this from the host clock refuses to lock. Generous by
// design — a host clock minutes or even hours wrong is exactly what the applet
// measures; a decode DECADES away (the 2026-07-20 q100 lock on 2006-01-01)
// can only be systematic misreads. Lock gating only; decodedUtc composition
// remains host-free (no host snapping).
constexpr int kPlausibilityBoundMinutes = 24 * 60;

// Type-erasing holder so the engine can own EITHER time-signal decoder behind
// one pointer: WwvDecoder and WwvbDecoder expose an identical surface
// (WwvDecoder.h / WwvbDecoder.h) but share no base class. This is purely an
// implementation detail of Task A's "owned WwvDecoder/WwvbDecoder (create the
// one selected at start())".
struct IDecoder {
    virtual ~IDecoder() = default;
    virtual void process(const float* mono, std::size_t n) = 0;
    virtual void reset() = 0;
    virtual ClockStation station() const = 0;
    virtual std::int64_t samplesConsumed() const = 0;
    // Live decoder lock state — the decoder suppresses no-change state edges, so
    // after an engine-side lock decay handleSecond resyncs from this accessor.
    virtual ClockLockState lockState() const = 0;
    // WS-4.5: arm the shared voter's absolute-plausibility gate.
    virtual void setPlausibility(std::function<TimeFields()> referenceNow,
                                 int boundMinutes) = 0;
    // WS-7: read-only acquisition snapshot (funnel stages 1-3 + 5).
    virtual ClockDecoderDiagnostics diagnostics() const = 0;

    // Engine-side callbacks, forwarded from the concrete decoder's callbacks.
    std::function<void(const ClockSecondInfo&)> onSecond;
    std::function<void(const ClockFrameInfo&)> onFrame;
    std::function<void(const ClockTimeInfo&)> onTime;
    std::function<void(ClockLockState)> onStateChanged;
};

template <class D>
struct DecoderHolder final : IDecoder {
    D d;
    explicit DecoderHolder(int sampleRateHz) : d(sampleRateHz) {
        d.onSecond = [this](const ClockSecondInfo& i) { if (onSecond) onSecond(i); };
        d.onFrame = [this](const ClockFrameInfo& fr) { if (onFrame) onFrame(fr); };
        d.onTime = [this](const ClockTimeInfo& t) { if (onTime) onTime(t); };
        d.onStateChanged = [this](ClockLockState s) { if (onStateChanged) onStateChanged(s); };
    }
    void process(const float* mono, std::size_t n) override { d.process(mono, n); }
    void reset() override { d.reset(); }
    ClockStation station() const override { return d.station(); }
    std::int64_t samplesConsumed() const override { return d.samplesConsumed(); }
    ClockLockState lockState() const override { return d.state(); }
    void setPlausibility(std::function<TimeFields()> referenceNow,
                         int boundMinutes) override {
        d.setPlausibility(std::move(referenceNow), boundMinutes);
    }
    ClockDecoderDiagnostics diagnostics() const override { return d.diagnostics(); }
};

} // namespace

struct AetherClockEngine::Impl : std::enable_shared_from_this<AetherClockEngine::Impl> {
    explicit Impl(AetherClockEngine* owner) : q(owner) {
        // Parented to q so it dies with the engine. Single-shot; the engine is a
        // thread-agnostic QObject whose slots and queued feedRxAudio all run on
        // its own thread, so every start/stop/re-arm of this timer happens there
        // — no cross-thread QTimer use.
        decayTimer = new QTimer(q);
        decayTimer->setSingleShot(true);
        QObject::connect(decayTimer, &QTimer::timeout, q, [this] { onDecayTimeout(); });

        // WS-7: ~1 Hz diagnostics emission while running. Same threading story
        // as decayTimer — every start/stop happens on the engine's thread.
        diagTimer = new QTimer(q);
        diagTimer->setInterval(1000);
        QObject::connect(diagTimer, &QTimer::timeout, q, [this] {
            if (running) emit q->diagnosticsUpdated(q->currentDiagnostics());
        });
    }

    QPointer<AetherClockEngine> q;

    // DAX-hold provider (EB3: the engine never touches the vendor stream). The
    // wiring layer wraps the central PanadapterStream::acquire/releaseDaxChannel
    // with DaxConsumer::Clock; the engine drives these with the bound channel.
    std::function<void(int)> acquireCh;
    std::function<void(int)> releaseCh;
    std::function<bool()> daxAvailable;               // unset = assume DAX

    QPointer<SliceModel> slice;
    std::shared_ptr<IDecoder> decoder;
    IDecoder* processingDecoder = nullptr;
    quint64 processingGeneration = 0;
    std::optional<PcmEpochLease> processingSource;
    bool deferredDecoderReset = false;

    // Both channels belong to us while acquire-new invokes the external
    // provider. Nested stop/start must be able to retire either hold.
    std::array<bool, 8> heldChannels{};
    ClockStation configured = ClockStation::Unknown;  // station chosen at start()
    ClockStation lastStation = ClockStation::Unknown; // for stationDetected edges
    ClockLockState lastState = ClockLockState::NoSignal;
    bool running = false;

    // Lock-decay watchdog (see setLockDecayTimeoutMs). Armed on start() and
    // re-armed on every classified second; on timeout it demotes the state one
    // step so a stalled feed cannot pin a stale Locked/Acquiring.
    QTimer* decayTimer = nullptr;
    int decayTimeoutMs = 10000;

    // WS-7 diagnostics cadence + the stage-4 classified-seconds ring: host-ms
    // stamps of the last minute's classified seconds (each handleSecond call IS
    // a classified second — the decoders emit nothing for an unclassified one).
    QTimer* diagTimer = nullptr;
    std::deque<qint64> classifiedMs;

    void pruneClassified(qint64 nowMs) {
        while (!classifiedMs.empty() && nowMs - classifiedMs.front() > 60000)
            classifiedMs.pop_front();
    }

    std::function<qint64()> nowUtcMs =
        [] { return QDateTime::currentMSecsSinceEpoch(); };

    ClockSampleTimeline timeline;
    DecoderPcmAdapter input;
    quint64 inputGeneration = 0;
    quint64 transitionRevision = 0;
    bool expectsDax = true;
    int selectedDaxChannel = 0;
    // Preserve slot/object attribution across stop/start and visits to other
    // slices. A replacement object cannot inherit a still-live retired source.
    std::map<int, QPointer<SliceModel>> nativeOwners;

    // Second-0 sample index of the most recent completed frame (from onFrame,
    // which always fires immediately before the vote refresh). votedField
    // (FieldMinutes) is normalized to this newest frame, so hh:mm from the vote
    // is the time AT this frame's second 0 — the anchor for onTime composition.
    std::int64_t lastFrameStartSample = 0;
    bool haveFrame = false;

    std::vector<float> monoScratch;

    void ingest(const QByteArray& pcm);
    void ingest(const PcmFrame& frame);

    void advanceInputGeneration() {
        const std::shared_ptr<Impl> lifetime = shared_from_this();
        ++inputGeneration;
        if (q) {
            emit q->sourceGenerationChanged(inputGeneration);
        }
    }

    void resetAcquisition() {
        const std::shared_ptr<Impl> lifetime = shared_from_this();
        const std::shared_ptr<IDecoder> resetting = decoder;
        const quint64 revision = transitionRevision;
        const quint64 generation = inputGeneration;
        if (resetting) {
            if (resetting.get() == processingDecoder) {
                deferredDecoderReset = true;
            } else {
                resetting->reset();
            }
        }
        if (!q || revision != transitionRevision || generation != inputGeneration
            || resetting != decoder) {
            return;
        }
        timeline.reset();
        lastFrameStartSample = 0;
        haveFrame = false;
        lastStation = ClockStation::Unknown;
        classifiedMs.clear();
        setState(ClockLockState::NoSignal);
        if (q && running && revision == transitionRevision && generation == inputGeneration
            && resetting == decoder) {
            armDecayTimer();
        }
    }

    bool resetInputContext(bool reselect = false) {
        const std::shared_ptr<Impl> lifetime = shared_from_this();
        const quint64 revision = ++transitionRevision;
        if (reselect) {
            selectInput();
        } else {
            input.reset();
        }
        ++inputGeneration;
        resetAcquisition();
        if (!q || revision != transitionRevision) {
            return false;
        }
        emit q->sourceGenerationChanged(inputGeneration);
        return q && revision == transitionRevision;
    }

    bool canPublish() const {
        return q && running && decoder
            && (!processingDecoder
                || (decoder.get() == processingDecoder
                    && processingGeneration == inputGeneration
                    && (!processingSource || processingSource->current())));
    }

    void processSamples(const float* samples, std::size_t count,
                        std::optional<PcmEpochLease> source = std::nullopt) {
        const std::shared_ptr<Impl> lifetime = shared_from_this();
        // Signals fire inline inside process(). A listener may revoke the
        // producer, stop, retune or restart. Hold the executing instance alive,
        // defer its reset, and suppress every further old-context publication.
        const std::shared_ptr<IDecoder> executing = decoder;
        processingDecoder = executing.get();
        processingGeneration = inputGeneration;
        processingSource = source;
        deferredDecoderReset = false;
        executing->process(samples, count);
        processingDecoder = nullptr;
        processingSource.reset();
        const bool stillSelected = decoder == executing;
        if (q && stillSelected && (deferredDecoderReset || (source && !source->current()))) {
            input.reset();
            resetAcquisition();
        }
        deferredDecoderReset = false;
    }

    void selectInput() {
        input.clearRoute();
        if (!slice) {
            return;
        }
        if (expectsDax) {
            if (selectedDaxChannel > 0) {
                input.selectRoute(DecoderPcmAdapter::RouteLane::Dax, selectedDaxChannel);
            }
        } else {
            input.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, slice->sliceId());
        }
    }

    QMetaObject::Connection connDax;
    QMetaObject::Connection connDestroyed;
    QMetaObject::Connection connFrequency;
    QMetaObject::Connection connMode;

    double hostMsAtSample(std::int64_t s) const {
        return timeline.hostMsAtSample(s);
    }

    void setState(ClockLockState s) {
        if (!q || s == lastState) return;
        lastState = s;
        emit q->lockStateChanged(s);
        if (q && lastState == s && (s == ClockLockState::NoSignal || canPublish())) {
            emit q->lockedChanged(s == ClockLockState::Locked);
        }
    }

    void disconnectAll() {
        QObject::disconnect(connDax);
        QObject::disconnect(connDestroyed);
        QObject::disconnect(connFrequency);
        QObject::disconnect(connMode);
        connDax = {};
        connDestroyed = {};
        connFrequency = {};
        connMode = {};
    }

    void releaseHold(int channel) {
        const std::function<void(int)> release = releaseCh;
        if (std::exchange(heldChannels[static_cast<std::size_t>(channel - 1)], false)
            && release) {
            release(channel);
        }
    }

    void releaseAllHolds() {
        const quint64 revision = transitionRevision;
        for (int channel = 1; channel <= 8; ++channel) {
            releaseHold(channel);
            if (!q || revision != transitionRevision) {
                return;
            }
        }
    }

    void armDecayTimer() { decayTimer->start(decayTimeoutMs); }

    void onDecayTimeout() {
        const std::shared_ptr<Impl> lifetime = shared_from_this();
        const quint64 revision = transitionRevision;
        if (!running) return;
        // Demote ONE step and re-arm; at the NoSignal floor stop re-arming until
        // the next classified second re-arms the watchdog (handleSecond).
        if (lastState == ClockLockState::Locked) {
            setState(ClockLockState::Acquiring);
            if (q && running && revision == transitionRevision) {
                armDecayTimer();
            }
        } else if (lastState == ClockLockState::Acquiring) {
            setState(ClockLockState::NoSignal);
        }
    }

    // ---- Decoder callbacks: fire inline on the feed thread during process() ----

    void handleState(ClockLockState st) {
        if (canPublish()) {
            setState(st);
        }
    }

    void handleSecond(const ClockSecondInfo& info) {
        if (!canPublish()) {
            return;
        }
        ClockAlignmentFrame f;
        f.hostUtcMs = static_cast<qint64>(std::llround(hostMsAtSample(info.edgeSample)));
        f.secondOfFrame = info.secondOfFrame;
        f.envelope = QVector<float>(info.envelope.begin(), info.envelope.end());
        f.expected = QVector<float>(info.expected.begin(), info.expected.end());
        // Where the received pulse sits relative to the template's nominal
        // position (WS-4.5): ~0 on a drift-free stream, nonzero while the
        // decoder is absorbing sample-clock drift. The display shifts the
        // expected overlay by this so envelope and template stay honest.
        f.edgeOffsetMs = (info.seriesRateHz > 0)
            ? qRound(info.windowShift * 1000.0 / info.seriesRateHz)
            : 0;
        f.symbol = static_cast<int>(info.symbol);
        f.confidence = info.confidence;
        f.station = static_cast<quint8>(static_cast<int>(decoder->station()));
        if (!canPublish()) {
            return;
        }
        emit q->alignmentFrame(f);
        if (!canPublish()) {
            return;
        }

        // Surface the station once the decoder classifies it (WWV/WWVH by tick
        // band per NIST SP 432; WWVB by construction).
        const ClockStation st = decoder->station();
        if (st != lastStation) {
            lastStation = st;
            emit q->stationDetected(st);
        }
        if (!canPublish()) {
            return;
        }

        // A classified second means audio is live: re-arm the lock-decay
        // watchdog, then resync engine state from the decoder's live accessor.
        // The decoder suppresses no-change state edges, so after an engine-side
        // decay it may still believe Locked and never re-emit it; the resync
        // recovers the engine to the decoder's truth.
        armDecayTimer();
        const ClockLockState ds = decoder->lockState();
        if (ds != lastState) setState(ds);
        if (!canPublish()) {
            return;
        }

        // WS-7 stage-4 ring: this callback fires once per CLASSIFIED second.
        const qint64 nowMs = nowUtcMs();
        classifiedMs.push_back(nowMs);
        pruneClassified(nowMs);
    }

    void handleFrame(const ClockFrameInfo& frame) {
        if (!canPublish()) {
            return;
        }
        // Fires immediately BEFORE any vote refresh; frameStartSample is the
        // decoder input-sample index of this frame's second 0 (same clock as
        // lastEdgeSample and the host anchor).
        lastFrameStartSample = frame.frameStartSample;
        haveFrame = true;

        // WS-7: re-emit the raw per-frame decode (previously discarded here —
        // frameConfidence, DUT1/DST/leap now reach the debug pane).
        emit q->frameDecoded(frame);
    }

    void handleTime(const ClockTimeInfo& t) {
        if (!canPublish()) {
            return;
        }
        // Compose from the voted frame's second 0 plus the elapsed-sample count
        // to the last edge. This is exact across WWVB per-second re-emission
        // (cached vote), WWV chunk-straddle, and multi-frame backlog, because
        // lastEdgeSecondOfFrame can point into a frame LATER than the vote —
        // using it directly would be up to minutes off. No host-clock snapping.
        if (!haveFrame) return;  // onFrame always precedes the vote; guard anyway

        const int year = 2000 + t.year2;
        // doy is 1-based day-of-year (NIST SP 432 BCD day field).
        const QDate date = QDate::fromJulianDay(
            QDate(year, 1, 1).toJulianDay() + static_cast<qint64>(t.doy) - 1);
        // hh:mm AT the voted frame's second 0 (votedField(Minutes) is normalized
        // to the newest frame = the most recent onFrame).
        const QDateTime baseUtc(date, QTime(t.hour, t.minute, 0), QTimeZone::utc());
        const int elapsedSec = static_cast<int>(std::lround(
            static_cast<double>(t.lastEdgeSample - lastFrameStartSample)
            / static_cast<double>(AetherClockEngine::kSampleRateHz)));
        const QDateTime decodedUtc = baseUtc.addSecs(elapsedSec);

        const double offsetMs =
            static_cast<double>(decodedUtc.toMSecsSinceEpoch())
            - hostMsAtSample(t.lastEdgeSample);
        int quality = static_cast<int>(std::lround(t.quality * 100.0));
        quality = std::clamp(quality, 0, 100);
        if (canPublish()) {
            emit q->timeDecoded(decodedUtc, offsetMs, quality);
        }
    }
};

AetherClockEngine::AetherClockEngine(QObject* parent)
    : QObject(parent), m_impl(std::make_shared<Impl>(this)) {
    // Register everything that crosses queued connections (GpsDelta precedent).
    qRegisterMetaType<AetherSDR::ClockAlignmentFrame>();
    qRegisterMetaType<AetherSDR::ClockLockState>("AetherSDR::ClockLockState");
    qRegisterMetaType<AetherSDR::ClockStation>("AetherSDR::ClockStation");
    qRegisterMetaType<AetherSDR::ClockDiagnostics>();
    qRegisterMetaType<AetherSDR::ClockFrameInfo>();
}

AetherClockEngine::~AetherClockEngine() {
    // Silent teardown: never leak a DAX hold (INV-3). No signals from a dying
    // object.
    m_impl->disconnectAll();
    m_impl->releaseAllHolds();
    m_impl->decoder.reset();
}

void AetherClockEngine::setDaxChannelProvider(std::function<void(int)> acquire,
                                              std::function<void(int)> release) {
    m_impl->acquireCh = std::move(acquire);
    m_impl->releaseCh = std::move(release);
}

void AetherClockEngine::setDaxAvailabilityProvider(
    std::function<bool()> hasDaxStreams) {
    m_impl->daxAvailable = std::move(hasDaxStreams);
}

void AetherClockEngine::setHostClock(std::function<qint64()> nowUtcMs) {
    if (nowUtcMs)
        m_impl->nowUtcMs = std::move(nowUtcMs);
    else
        m_impl->nowUtcMs = [] { return QDateTime::currentMSecsSinceEpoch(); };
}

void AetherClockEngine::setLockDecayTimeoutMs(int ms) {
    m_impl->decayTimeoutMs = ms < 50 ? 50 : ms;
}

bool AetherClockEngine::isRunning() const { return m_impl->running; }

int AetherClockEngine::boundSliceId() const {
    return m_impl->slice ? m_impl->slice->sliceId() : -1;
}

ClockStation AetherClockEngine::configuredStation() const {
    return m_impl->configured;
}

ClockLockState AetherClockEngine::lockState() const { return m_impl->lastState; }

quint64 AetherClockEngine::inputGeneration() const { return m_impl->inputGeneration; }

ClockDiagnostics AetherClockEngine::currentDiagnostics() const {
    auto& d = *m_impl;
    ClockDiagnostics g;
    if (!d.running) {
        return g;
    }
    if (d.decoder) {
        const ClockDecoderDiagnostics dd = d.decoder->diagnostics();
        g.toneSnrDb = dd.toneSnrDb;
        g.pwmContrast = dd.pwmContrast;
        g.toneDetected = dd.toneDetected;
        g.phaseLocked = dd.phaseLocked;
        g.delayEstMs = dd.delayEstMs;
        g.anchored = dd.anchored;
        g.badFrameStreak = dd.badFrameStreak;
        g.framesInWindow = dd.framesInWindow;
        g.windowSize = dd.windowSize;
        g.voteQuality = dd.voteQuality;
        g.refusalReason = dd.refusalReason;
    }
    // Count, don't prune — this accessor is const; handleSecond's prune keeps
    // the ring bounded.
    const qint64 nowMs = d.nowUtcMs();
    std::size_t live = 0;
    for (qint64 t : d.classifiedMs)
        if (nowMs - t <= 60000) ++live;
    g.classifiedPct = static_cast<int>(std::min<std::size_t>(100, live * 100 / 60));
    return g;
}

void AetherClockEngine::start(SliceModel* slice, ClockStation station) {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    const QPointer<SliceModel> requestedSlice = slice;
    if (d.running || d.decoder || d.slice
        || std::any_of(d.heldChannels.begin(), d.heldChannels.end(), [](bool held) { return held; })) {
        const quint64 beforeStop = d.transitionRevision;
        stop();
        if (d.transitionRevision != beforeStop + 1) {
            return;
        }
    }
    if (!d.q) {
        return;
    }

    // Require a slice to bind; stay stopped otherwise.
    if (!requestedSlice) {
        qWarning() << "AetherClockEngine::start: no slice - staying stopped";
        return;
    }
    const quint64 revision = ++d.transitionRevision;

    const bool expectsDax = !d.daxAvailable || d.daxAvailable();
    if (!d.q || revision != d.transitionRevision || !requestedSlice) {
        return;
    }
    d.slice = requestedSlice;
    d.expectsDax = expectsDax;
    d.selectedDaxChannel = slice->daxChannel();
    if (!d.expectsDax) {
        for (auto it = d.nativeOwners.begin(); it != d.nativeOwners.end();) {
            if (!it->second) {
                d.input.retireRoute(DecoderPcmAdapter::RouteLane::NativeSlice, it->first);
                it = d.nativeOwners.erase(it);
            } else {
                ++it;
            }
        }
        const auto previous = d.nativeOwners.find(slice->sliceId());
        if (previous != d.nativeOwners.end() && previous->second != slice) {
            d.input.retireRoute(DecoderPcmAdapter::RouteLane::NativeSlice, slice->sliceId());
        }
        if (previous == d.nativeOwners.end()
            && d.nativeOwners.size() >= DecoderPcmAdapter::kMaxRoutes) {
            d.slice = nullptr;
            qWarning() << "AetherClockEngine::start: receiver attribution capacity reached";
            return;
        }
        d.nativeOwners[slice->sliceId()] = slice;
    }
    d.configured = station;
    d.lastStation = ClockStation::Unknown;

    // Create the selected decoder and wire its inline callbacks.
    if (station == ClockStation::Wwvb)
        d.decoder = std::make_shared<DecoderHolder<WwvbDecoder>>(kSampleRateHz);
    else
        d.decoder = std::make_shared<DecoderHolder<WwvDecoder>>(kSampleRateHz);
    const std::weak_ptr<Impl> weak = lifetime;
    d.decoder->onSecond = [weak](const ClockSecondInfo& i) {
        if (const auto state = weak.lock()) { state->handleSecond(i); }
    };
    d.decoder->onFrame = [weak](const ClockFrameInfo& fr) {
        if (const auto state = weak.lock()) { state->handleFrame(fr); }
    };
    d.decoder->onTime = [weak](const ClockTimeInfo& t) {
        if (const auto state = weak.lock()) { state->handleTime(t); }
    };
    d.decoder->onStateChanged = [weak](ClockLockState s) {
        if (const auto state = weak.lock()) { state->handleState(s); }
    };

    // Arm the absolute-plausibility gate with the host clock (WS-4.5). The
    // callback fires on the feed thread inside process(); nowUtcMs is the same
    // injectable clock the sample<->host anchor uses, so tests stay in control.
    d.decoder->setPlausibility(
        [weak] {
            const auto state = weak.lock();
            if (!state || !state->q) {
                return TimeFields{};
            }
            const QDateTime now = QDateTime::fromMSecsSinceEpoch(
                state->nowUtcMs(), QTimeZone::utc());
            TimeFields tf;
            tf.minute = now.time().minute();
            tf.hour = now.time().hour();
            tf.doy = now.date().dayOfYear();
            tf.year2 = now.date().year() % 100;
            return tf;
        },
        kPlausibilityBoundMinutes);

    // Fresh sample<->host and frame anchors for the fresh decoder.
    d.timeline.reset();
    d.lastFrameStartSample = 0;
    d.haveFrame = false;
    d.selectInput();

    // Hold the slice's LIVE DAX channel through the injected provider (the
    // wiring layer routes this to the central #3305 ownership registry).
    const int ch = slice->daxChannel();
    if (!d.acquireCh || !d.releaseCh) {
        qWarning() << "AetherClockEngine::start: no DAX channel provider set -"
                   << "hold not acquired; audio will not flow";
    } else if (ch >= 1 && ch <= 8) {
        d.heldChannels[static_cast<std::size_t>(ch - 1)] = true;
        const std::function<void(int)> acquire = d.acquireCh;
        acquire(ch);
    } else if (!d.expectsDax) {
        // Seam-native backend: the radio declares no DAX plane, so there is no
        // channel to assign and nothing to hold. Audio arrives through
        // feedRxSliceAudio() instead. Warning here would be true-sounding and
        // wrong, and would send the reader debugging the wrong subsystem.
    } else {
        qWarning() << "AetherClockEngine::start: slice" << slice->sliceId()
                   << "has no DAX channel assigned - audio will not flow";
    }
    if (!d.q || revision != d.transitionRevision) {
        return;
    }
    if (!d.slice) {
        stop();
        return;
    }

    // Follow mid-session DAX reassignment THROUGH THE PROVIDER: acquire NEW
    // before releasing OLD so the channel's holder set never transiently hits
    // zero (RADE precedent).
    d.connDax = connect(slice, &SliceModel::daxChannelChanged, this,
                        [this](int newCh) {
        const std::shared_ptr<Impl> state = m_impl;
        auto& e = *state;
        if (e.expectsDax && e.selectedDaxChannel != newCh) {
            e.selectedDaxChannel = newCh;
            if (!e.resetInputContext(true)) {
                return;
            }
        }
        if (!e.acquireCh || !e.releaseCh) return;
        const quint64 revision = e.transitionRevision;
        const int nc = (newCh >= 1 && newCh <= 8) ? newCh : 0;
        if (nc && !e.heldChannels[static_cast<std::size_t>(nc - 1)]) {
            e.heldChannels[static_cast<std::size_t>(nc - 1)] = true;
            const std::function<void(int)> acquire = e.acquireCh;
            acquire(nc);
            if (!e.q || revision != e.transitionRevision) {
                return;
            }
        }
        for (int channel = 1; channel <= 8; ++channel) {
            if (channel != nc) {
                e.releaseHold(channel);
                if (!e.q || revision != e.transitionRevision) {
                    return;
                }
            }
        }
    });
    // A changed demodulator context invalidates both detector state and queued
    // pre-change audio without altering the operator's running toggle or holds.
    d.connFrequency = connect(slice, &SliceModel::frequencyChanged, this,
                              [this] { m_impl->resetInputContext(); });
    d.connMode = connect(slice, &SliceModel::modeChanged, this,
                         [this] { m_impl->resetInputContext(); });

    // Graceful loss if the bound slice is destroyed under us.
    d.connDestroyed = connect(slice, &QObject::destroyed, this, [this, sliceId = slice->sliceId()] {
        auto& e = *m_impl;
        if (!e.expectsDax) {
            e.input.retireRoute(DecoderPcmAdapter::RouteLane::NativeSlice, sliceId);
        }
        stop();
    });

    d.running = true;
    d.lastState = ClockLockState::NoSignal;  // state starts NoSignal
    d.armDecayTimer();                       // watchdog runs while running
    d.classifiedMs.clear();
    d.diagTimer->start();                    // ~1 Hz diagnostics while running
    d.advanceInputGeneration();
    if (!d.q || revision != d.transitionRevision || !d.slice) {
        return;
    }
    emit runningChanged(true);
}

void AetherClockEngine::stop() {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    const quint64 revision = ++d.transitionRevision;
    const bool was = d.running;
    d.running = false;
    // Retire resources before any signal or provider callback can start a
    // replacement run. Such a callback must see a fully detached old context.
    d.decayTimer->stop();
    d.diagTimer->stop();
    d.disconnectAll();
    d.input.clearRoute();
    d.decoder.reset();
    d.slice = nullptr;
    d.releaseAllHolds();
    if (!d.q || revision != d.transitionRevision) {
        return;
    }
    d.resetAcquisition();
    if (!d.q || revision != d.transitionRevision) {
        return;
    }
    d.advanceInputGeneration();
    if (!d.q || revision != d.transitionRevision) {
        return;
    }
    d.classifiedMs.clear();
    d.lastStation = ClockStation::Unknown;
    d.haveFrame = false;
    d.running = false;
    d.setState(ClockLockState::NoSignal);  // emits only on actual change
    if (was && d.q && revision == d.transitionRevision) {
        emit runningChanged(false);
    }
}

void AetherClockEngine::applyStationPreset(ClockStation station, double carrierMHz) {
    // The bound slice is null when unbound, so this keeps the no-op-unless-bound
    // semantics (the overload warns + returns on a null slice).
    applyStationPreset(m_impl->slice.data(), station, carrierMHz);
}

void AetherClockEngine::applyStationPreset(SliceModel* slice, ClockStation station,
                                           double carrierMHz) {
    if (!slice) {
        qWarning() << "AetherClockEngine: no slice - station preset not applied";
        return;
    }
    // All-or-nothing: on a locked slice only setFrequency honors the lock
    // (SliceModel.cpp), so applying the preset would strand the slice on its
    // old frequency while still forcing USB (and AGC off for WWVB). Refuse the
    // whole preset instead of leaving an inconsistent state.
    if (slice->isLocked()) {
        qWarning() << "AetherClockEngine: slice is locked -"
                   << "station preset not applied";
        return;
    }
    // Radio-authoritative live-slice state only; nothing persisted. Neither
    // binds the slice nor starts the engine.
    slice->setFrequency(listeningDialMHz(carrierMHz));
    slice->setMode(QStringLiteral("USB"));
    if (station == ClockStation::Wwvb)
        slice->setAgcMode(QStringLiteral("off"));
}

// Shared PCM tail for both ingest slots. The two public slots are a filter
// each - channel-keyed for DAX, slice-keyed for the seam - and the DSP body
// exists exactly once here.
void AetherClockEngine::Impl::ingest(const QByteArray& pcm) {
    auto& d = *this;
    // Payload contract (shared with the Bridge/Tci/Rade DAX consumers):
    // float32 interleaved stereo, native-endian, 24 kHz, 8 bytes per frame,
    // 4-byte-aligned buffer. A trailing partial frame is deliberately floored
    // away by the /8 — the stream is frame-oriented and a fragment carries no
    // usable sample pair.
    const std::size_t n = static_cast<std::size_t>(pcm.size()) / 8;
    if (n == 0) return;
    const float* in = reinterpret_cast<const float*>(pcm.constData());

    d.monoScratch.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        d.monoScratch[i] = 0.5f * in[2 * i] + 0.5f * in[2 * i + 1];

    // Anchor BEFORE process(): host time corresponds to the END of this buffer
    // (samplesConsumedBefore + n), so callbacks fired inline during process()
    // already read a current sample<->host mapping.
    d.timeline.anchor(0, d.decoder->samplesConsumed() + n, kSampleRateHz, 0, d.nowUtcMs());
    d.processSamples(d.monoScratch.data(), n);
}

void AetherClockEngine::Impl::ingest(const PcmFrame& frame) {
    const quint64 generation = inputGeneration;
    const quint64 revision = transitionRevision;
    const std::shared_ptr<IDecoder> selectedDecoder = decoder;
    const QPointer<SliceModel> selectedSlice = slice;
    const qint64 arrivalMs = nowUtcMs();
    const std::optional<DecoderPcmBlock> block = input.accept(frame);
    if (!block || !block->current()) {
        return;
    }
    if (block->discontinuity) {
        resetAcquisition();
    }
    if (!canPublish() || generation != inputGeneration || revision != transitionRevision
        || selectedDecoder != decoder || selectedSlice != slice) {
        return;
    }
    if (block->firstOutputSample != static_cast<quint64>(decoder->samplesConsumed())
        || !timeline.anchor(block->segmentFirstInputSample, block->inputEndSample,
                            block->inputSampleRateHz, block->groupDelayInputFrames,
                            arrivalMs)) {
        input.reset();
        resetAcquisition();
        return;
    }
    // The admitted input-end anchor includes the still-staged partial batch.
    // Decoder callbacks remain in local24 samples, including the converter's
    // signal delay; only their host-time mapping compensates that delay.
    if (block->current() && !block->samples.isEmpty()) {
        processSamples(block->samples.constData(), block->samples.size(), block->source);
    }
}

void AetherClockEngine::feedRxAudio(int channel, const QByteArray& pcm) {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    if (!d.running || !d.decoder || !d.slice || d.processingDecoder) return;
    if (channel != d.slice->daxChannel()) return;  // live channel filter
    d.ingest(pcm);
}

void AetherClockEngine::feedRxSliceAudio(int sliceId, const QByteArray& pcm) {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    if (!d.running || !d.decoder || !d.slice || d.processingDecoder) return;
    // Live slice filter. Deliberately NOT a daxChannel() compare: the sender
    // already identified the slice, and a seam backend's daxChannel() is 0.
    if (sliceId != d.slice->sliceId()) return;
    d.ingest(pcm);
}

void AetherClockEngine::feedRxAudio(int channel, const PcmFrame& frame,
                                  quint64 generation) {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    if (!d.running || !d.decoder || !d.slice || d.processingDecoder || !d.expectsDax
        || generation != d.inputGeneration || channel <= 0
        || channel != d.slice->daxChannel()) {
        return;
    }
    d.ingest(frame);
}

void AetherClockEngine::feedRxSliceAudio(int sliceId, const PcmFrame& frame,
                                       quint64 generation) {
    const std::shared_ptr<Impl> lifetime = m_impl;
    auto& d = *lifetime;
    if (!d.running || !d.decoder || !d.slice || d.processingDecoder || d.expectsDax
        || generation != d.inputGeneration || sliceId != d.slice->sliceId()) {
        return;
    }
    d.ingest(frame);
}

// ---- Station preset statics ----

QVector<double> AetherClockEngine::wwvCarrierFrequenciesMHz() {
    return QVector<double>{2.5, 5.0, 10.0, 15.0, 20.0};
}

double AetherClockEngine::wwvbCarrierFrequencyMHz() { return 0.060; }

double AetherClockEngine::listeningDialMHz(double carrierMHz) {
    return carrierMHz - 0.001;
}

} // namespace AetherSDR
