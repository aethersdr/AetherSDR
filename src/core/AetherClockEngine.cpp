#include "core/AetherClockEngine.h"

#include "core/WwvDecoder.h"
#include "core/WwvbDecoder.h"
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
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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

struct AetherClockEngine::Impl {
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

    AetherClockEngine* q = nullptr;

    // DAX-hold provider (EB3: the engine never touches the vendor stream). The
    // wiring layer wraps the central PanadapterStream::acquire/releaseDaxChannel
    // with DaxConsumer::Clock; the engine drives these with the bound channel.
    std::function<void(int)> acquireCh;
    std::function<void(int)> releaseCh;

    QPointer<SliceModel> slice;
    std::unique_ptr<IDecoder> decoder;

    int heldChannel = 0;                              // 0 = none
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

    // Sample <-> host anchor: host time `anchorHostMs` corresponds to total
    // consumed sample `anchorSamples` (the END of the last fed buffer).
    std::int64_t anchorSamples = 0;
    qint64 anchorHostMs = 0;

    // Second-0 sample index of the most recent completed frame (from onFrame,
    // which always fires immediately before the vote refresh). votedField
    // (FieldMinutes) is normalized to this newest frame, so hh:mm from the vote
    // is the time AT this frame's second 0 — the anchor for onTime composition.
    std::int64_t lastFrameStartSample = 0;
    bool haveFrame = false;

    std::vector<float> monoScratch;

    QMetaObject::Connection connDax;
    QMetaObject::Connection connDestroyed;

    double hostMsAtSample(std::int64_t s) const {
        return static_cast<double>(anchorHostMs)
             - static_cast<double>(anchorSamples - s) * 1000.0
                   / static_cast<double>(AetherClockEngine::kSampleRateHz);
    }

    void setState(ClockLockState s) {
        if (s == lastState) return;
        lastState = s;
        emit q->lockStateChanged(s);
        emit q->lockedChanged(s == ClockLockState::Locked);
    }

    void disconnectAll() {
        QObject::disconnect(connDax);
        QObject::disconnect(connDestroyed);
        connDax = {};
        connDestroyed = {};
    }

    void releaseHold() {
        if (releaseCh && heldChannel >= 1 && heldChannel <= 4)
            releaseCh(heldChannel);
        heldChannel = 0;
    }

    void armDecayTimer() { decayTimer->start(decayTimeoutMs); }

    void onDecayTimeout() {
        if (!running) return;
        // Demote ONE step and re-arm; at the NoSignal floor stop re-arming until
        // the next classified second re-arms the watchdog (handleSecond).
        if (lastState == ClockLockState::Locked) {
            setState(ClockLockState::Acquiring);
            armDecayTimer();
        } else if (lastState == ClockLockState::Acquiring) {
            setState(ClockLockState::NoSignal);
        }
    }

    // ---- Decoder callbacks: fire inline on the feed thread during process() ----

    void handleState(ClockLockState st) { setState(st); }

    void handleSecond(const ClockSecondInfo& info) {
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
        emit q->alignmentFrame(f);

        // Surface the station once the decoder classifies it (WWV/WWVH by tick
        // band per NIST SP 432; WWVB by construction).
        const ClockStation st = decoder->station();
        if (st != lastStation) {
            lastStation = st;
            emit q->stationDetected(st);
        }

        // A classified second means audio is live: re-arm the lock-decay
        // watchdog, then resync engine state from the decoder's live accessor.
        // The decoder suppresses no-change state edges, so after an engine-side
        // decay it may still believe Locked and never re-emit it; the resync
        // recovers the engine to the decoder's truth.
        armDecayTimer();
        const ClockLockState ds = decoder->lockState();
        if (ds != lastState) setState(ds);

        // WS-7 stage-4 ring: this callback fires once per CLASSIFIED second.
        const qint64 nowMs = nowUtcMs();
        classifiedMs.push_back(nowMs);
        pruneClassified(nowMs);
    }

    void handleFrame(const ClockFrameInfo& frame) {
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
        emit q->timeDecoded(decodedUtc, offsetMs, quality);
    }
};

AetherClockEngine::AetherClockEngine(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {
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
    m_impl->releaseHold();
    m_impl->decoder.reset();
}

void AetherClockEngine::setDaxChannelProvider(std::function<void(int)> acquire,
                                              std::function<void(int)> release) {
    m_impl->acquireCh = std::move(acquire);
    m_impl->releaseCh = std::move(release);
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

ClockDiagnostics AetherClockEngine::currentDiagnostics() const {
    auto& d = *m_impl;
    ClockDiagnostics g;
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
    auto& d = *m_impl;
    if (d.running) stop();

    // Require a slice to bind; stay stopped otherwise.
    if (!slice) {
        qWarning() << "AetherClockEngine::start: no slice - staying stopped";
        return;
    }

    d.slice = slice;
    d.configured = station;
    d.lastStation = ClockStation::Unknown;

    // Create the selected decoder and wire its inline callbacks.
    if (station == ClockStation::Wwvb)
        d.decoder = std::make_unique<DecoderHolder<WwvbDecoder>>(kSampleRateHz);
    else
        d.decoder = std::make_unique<DecoderHolder<WwvDecoder>>(kSampleRateHz);
    d.decoder->onSecond = [this](const ClockSecondInfo& i) { m_impl->handleSecond(i); };
    d.decoder->onFrame = [this](const ClockFrameInfo& fr) { m_impl->handleFrame(fr); };
    d.decoder->onTime = [this](const ClockTimeInfo& t) { m_impl->handleTime(t); };
    d.decoder->onStateChanged = [this](ClockLockState s) { m_impl->handleState(s); };

    // Arm the absolute-plausibility gate with the host clock (WS-4.5). The
    // callback fires on the feed thread inside process(); nowUtcMs is the same
    // injectable clock the sample<->host anchor uses, so tests stay in control.
    d.decoder->setPlausibility(
        [this] {
            const QDateTime now = QDateTime::fromMSecsSinceEpoch(
                m_impl->nowUtcMs(), QTimeZone::utc());
            TimeFields tf;
            tf.minute = now.time().minute();
            tf.hour = now.time().hour();
            tf.doy = now.date().dayOfYear();
            tf.year2 = now.date().year() % 100;
            return tf;
        },
        kPlausibilityBoundMinutes);

    // Fresh sample<->host and frame anchors for the fresh decoder.
    d.anchorSamples = 0;
    d.anchorHostMs = d.nowUtcMs();
    d.lastFrameStartSample = 0;
    d.haveFrame = false;

    // Hold the slice's LIVE DAX channel through the injected provider (the
    // wiring layer routes this to the central #3305 ownership registry).
    const int ch = slice->daxChannel();
    d.heldChannel = 0;
    if (!d.acquireCh || !d.releaseCh) {
        qWarning() << "AetherClockEngine::start: no DAX channel provider set -"
                   << "hold not acquired; audio will not flow";
    } else if (ch >= 1 && ch <= 4) {
        d.heldChannel = ch;
        d.acquireCh(ch);
    } else {
        qWarning() << "AetherClockEngine::start: slice" << slice->sliceId()
                   << "has no DAX channel assigned - audio will not flow";
    }

    // Follow mid-session DAX reassignment THROUGH THE PROVIDER: acquire NEW
    // before releasing OLD so the channel's holder set never transiently hits
    // zero (RADE precedent).
    d.connDax = connect(slice, &SliceModel::daxChannelChanged, this,
                        [this](int newCh) {
        auto& e = *m_impl;
        if (!e.acquireCh || !e.releaseCh) return;
        const int oldCh = e.heldChannel;
        const int nc = (newCh >= 1 && newCh <= 4) ? newCh : 0;
        if (nc) e.acquireCh(nc);
        if (oldCh >= 1 && oldCh <= 4 && oldCh != nc) e.releaseCh(oldCh);
        e.heldChannel = nc;
    });

    // Graceful loss if the bound slice is destroyed under us.
    d.connDestroyed = connect(slice, &QObject::destroyed, this, [this] {
        auto& e = *m_impl;
        e.decayTimer->stop();
        e.releaseHold();
        e.disconnectAll();
        if (e.decoder) e.decoder->reset();
        e.running = false;
        e.setState(ClockLockState::NoSignal);
        emit runningChanged(false);
    });

    d.running = true;
    d.lastState = ClockLockState::NoSignal;  // state starts NoSignal
    d.armDecayTimer();                       // watchdog runs while running
    d.classifiedMs.clear();
    d.diagTimer->start();                    // ~1 Hz diagnostics while running
    emit runningChanged(true);
}

void AetherClockEngine::stop() {
    auto& d = *m_impl;
    const bool was = d.running;
    d.decayTimer->stop();
    d.diagTimer->stop();
    d.classifiedMs.clear();
    d.releaseHold();
    d.disconnectAll();
    if (d.decoder) d.decoder->reset();
    d.decoder.reset();
    d.slice = nullptr;
    d.lastStation = ClockStation::Unknown;
    d.haveFrame = false;
    d.running = false;
    d.setState(ClockLockState::NoSignal);  // emits only on actual change
    if (was) emit runningChanged(false);
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

void AetherClockEngine::feedRxAudio(int channel, const QByteArray& pcm) {
    auto& d = *m_impl;
    if (!d.running || !d.decoder || !d.slice) return;
    if (channel != d.slice->daxChannel()) return;  // live channel filter

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
        d.monoScratch[i] = 0.5f * (in[2 * i] + in[2 * i + 1]);  // downmix L+R

    // Anchor BEFORE process(): host time corresponds to the END of this buffer
    // (samplesConsumedBefore + n), so callbacks fired inline during process()
    // already read a current sample<->host mapping.
    d.anchorSamples = d.decoder->samplesConsumed() + static_cast<std::int64_t>(n);
    d.anchorHostMs = d.nowUtcMs();
    d.decoder->process(d.monoScratch.data(), n);
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
