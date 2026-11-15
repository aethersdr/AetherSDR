#pragma once

// Temporary opt-in startup investigation. Each buffer has one writer; inspect
// only after the acquisition thread has joined. No logging/I/O in capture.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace AetherSDR::rtl {
struct RtlStartupTrace {
    enum Kind { ServiceBegin, ControlDone, SnapshotDone, PublishBegin, PublishEnd,
        DrainBegin, DrainEnd, ServiceEnd, SliceBegin, SliceEnd, WorkerAdoptBegin,
        WorkerAdoptEnd, FirstNativeBlock, FirstQueueFull, FirstDrop, FirstRecovery };
    static std::uint64_t nowNs() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    struct Event {
        int kind = 0, slot = -1;
        std::uint64_t ns = 0, session = 0, revision = 0, requestedRevision = 0;
        std::uint64_t captureFirst = 0, packetFirst = 0, callback = 0;
        std::uint64_t drops = 0, queued = 0, frames = 0;
    };
    template<std::size_t N> struct Buffer {
        std::array<Event, N> events{};
        std::size_t size = 0, omitted = 0;
        void add(const Event& event) noexcept
        {
            if (size < N) { events[size++] = event; }
            else { ++omitted; }
        }
    };
    struct Bin {
        std::uint64_t callbacks = 0, iqFrames = 0, attempts = 0, drops = 0;
        std::uint64_t firstNs = 0, lastNs = 0, maxCallbackNs = 0, maxAttempts = 0;
    };
    const std::uint64_t startedNs = nowNs();
    Buffer<16384> owner; // first 20 seconds, bounded even under timer reentry
    Buffer<64> producer;
    std::array<Bin, 128> bins{}; // 20 ms aggregates: first 2.56 s of native PCM
    bool ownerExpired = false;
    std::uint64_t nativeFirstNs = 0, callbackStartNs = 0, callbackIndex = 0;
    std::uint64_t packetAttempts = 0, packetDrops = 0;
    bool firstFull = false, firstDrop = false, firstRecovery = false;

    void markOwner(Kind kind, std::uint64_t session, std::uint64_t revision,
        std::uint64_t requested, std::uint64_t drops, std::uint64_t queued, int slot = -1) noexcept
    {
        if (ownerExpired) { return; }
        const std::uint64_t ns = nowNs();
        if (ns - startedNs > 20'000'000'000ULL) { ownerExpired = true; return; }
        Event event; event.kind = kind; event.slot = slot; event.ns = ns;
        event.session = session; event.revision = revision; event.requestedRevision = requested;
        event.drops = drops; event.queued = queued; owner.add(event);
    }
    void markProducer(Kind kind, std::uint64_t session, std::uint64_t revision,
        std::uint64_t captureFirst, int slot = -1, std::uint64_t packetFirst = 0,
        std::uint64_t frames = 0, std::uint64_t queued = 0) noexcept
    {
        Event event; event.kind = kind; event.slot = slot; event.ns = nowNs();
        event.session = session; event.revision = revision; event.captureFirst = captureFirst;
        event.packetFirst = packetFirst; event.callback = callbackIndex;
        event.drops = packetDrops; event.queued = queued; event.frames = frames;
        producer.add(event);
        if (kind == FirstNativeBlock) { nativeFirstNs = event.ns; }
    }
    void finishCallback(std::uint64_t endedNs, std::uint64_t iqFrames,
        std::uint64_t attemptsBefore, std::uint64_t dropsBefore) noexcept
    {
        if (!nativeFirstNs || endedNs < nativeFirstNs) { return; }
        const std::uint64_t index = (endedNs - nativeFirstNs) / 20'000'000;
        if (index >= bins.size()) { return; }
        Bin& bin = bins[index];
        ++bin.callbacks; bin.iqFrames += iqFrames;
        const std::uint64_t attempts = packetAttempts - attemptsBefore;
        bin.attempts += attempts; bin.drops += packetDrops - dropsBefore;
        if (!bin.firstNs) { bin.firstNs = callbackStartNs; }
        bin.lastNs = endedNs;
        bin.maxCallbackNs = std::max(bin.maxCallbackNs, endedNs - callbackStartNs);
        bin.maxAttempts = std::max(bin.maxAttempts, attempts);
    }
    QByteArray jsonAfterJoin() const
    {
        const auto number = [](std::uint64_t n) { return QString::number(static_cast<qulonglong>(n)); };
        const auto serialize = [&number](const auto& buffer) {
            QJsonArray result;
            for (std::size_t i = 0; i < buffer.size; ++i) {
                const Event& event = buffer.events[i];
                result.append(QJsonObject{{"kind", event.kind}, {"slot", event.slot},
                    {"ns", number(event.ns)}, {"session", number(event.session)},
                    {"revision", number(event.revision)}, {"requestedRevision", number(event.requestedRevision)},
                    {"captureFirst", number(event.captureFirst)}, {"packetFirst", number(event.packetFirst)},
                    {"callback", number(event.callback)}, {"drops", number(event.drops)},
                    {"queued", number(event.queued)}, {"frames", number(event.frames)}});
            }
            return result;
        };
        QJsonArray aggregate;
        for (std::size_t i = 0; i < bins.size(); ++i) {
            const Bin& bin = bins[i];
            if (!bin.callbacks) { continue; }
            aggregate.append(QJsonObject{{"bin", int(i)}, {"callbacks", number(bin.callbacks)},
                {"iqFrames", number(bin.iqFrames)}, {"attempts", number(bin.attempts)},
                {"drops", number(bin.drops)}, {"firstNs", number(bin.firstNs)},
                {"lastNs", number(bin.lastNs)}, {"maxCallbackNs", number(bin.maxCallbackNs)},
                {"maxAttempts", number(bin.maxAttempts)}});
        }
        const QJsonObject document{{"schema", 1}, {"startedNs", number(startedNs)},
            {"nativeFirstNs", number(nativeFirstNs)}, {"ownerEvents", serialize(owner)},
            {"producer", serialize(producer)}, {"producerBins20ms", aggregate},
            {"ownerOmitted", number(owner.omitted)}, {"producerOmitted", number(producer.omitted)},
            {"ownerExpired", ownerExpired}, {"writtenAfterJoin", true},
            {"packetAttemptsAtJoin", number(packetAttempts)}, {"packetDropsAtJoin", number(packetDrops)}};
        return QJsonDocument(document).toJson(QJsonDocument::Compact);
    }
};
} // namespace AetherSDR::rtl
