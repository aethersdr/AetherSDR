// Socket-free Icom incident telemetry contract.
//
// Positive radio/session convergence belongs to the live automation bridge.
// This test drives only the deterministic backend state transition that turns
// an expired key-on confirmation into a payload-free support dossier.

#include "core/backends/icom/IcomCivBackend.h"

#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prepareExpiredKeyOn(IcomCivBackend& backend,
                                    const IcomModel& model,
                                    std::uint64_t generation)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
        backend.m_keyed = false;
        backend.m_pendingPttIntent = true;
        backend.m_pendingPttUntilMs = backend.nowMs() - 1;
    }

    static void deliver(IcomCivBackend& backend, const CivFrame& frame,
                        std::uint64_t generation)
    {
        backend.onCivFrame(frame, generation);
    }

    static QVariantMap freshness(const IcomCivBackend& backend, bool withValues = true)
    {
        return backend.stateFreshness(withValues);
    }
    static void age(IcomCivBackend& backend, const QString& key)
    {
        backend.m_confirmedState[key].atMs = backend.nowMs() - 6000;
    }
    static void intent(IcomCivBackend& backend, const std::vector<std::uint8_t>& frame)
    {
        backend.queueWrite(frame, {}, IcomCivScheduler::Priority::Operator, true, true);
    }
    static void identify(IcomCivBackend& backend) { backend.m_civReported = 0xA4; }
    static QVariantMap incident(const IcomCivBackend& backend)
    {
        return backend.m_lastIncident;
    }

    // A backend that has a radio-authoritative frequency and one frequency
    // write outstanding on the wire — the state a refused tune arrives into.
    static void prepareOutstandingFrequencyWrite(IcomCivBackend& backend,
                                                 const IcomModel& model,
                                                 std::uint64_t generation,
                                                 std::uint64_t heldHz)
    {
        prepareOutstandingFrequencyRequest(
            backend, model, generation, heldHz,
            cmdSetFrequency(model.civAddress, heldHz + 1'000'000));
    }

    // Same state, but the in-flight transaction is the poll's 03 READ. It
    // shares the "frequency" key with the write, and an FA retires it just the
    // same — which is exactly why the fix must look past the key.
    static void prepareOutstandingFrequencyRead(IcomCivBackend& backend,
                                                const IcomModel& model,
                                                std::uint64_t generation,
                                                std::uint64_t heldHz)
    {
        prepareOutstandingFrequencyRequest(backend, model, generation, heldHz,
                                           cmdReadFrequency(model.civAddress));
    }

    static void setHeldFrequency(IcomCivBackend& backend, std::uint64_t hz)
    {
        backend.m_frequencyHz = hz;
    }

    static void prepareOutstandingFrequencyRequest(IcomCivBackend& backend,
                                                   const IcomModel& model,
                                                   std::uint64_t generation,
                                                   std::uint64_t heldHz,
                                                   std::vector<std::uint8_t> frame)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = generation;
        backend.m_frequencyHz = heldHz;
        IcomCivScheduler::Request request;
        request.frame = std::move(frame);
        request.key = "frequency";
        request.expectsReply = true;
        request.acceptsGenericReply = true;
        backend.m_civScheduler.enqueue(request, backend.nowMs());
        // Take it off the queue so it is genuinely in flight: observe() only
        // retires a transaction that was actually dispatched, and the whole
        // point is that the FA below completes THIS request.
        (void)backend.m_civScheduler.takeNext(backend.nowMs());
    }

    static std::string lastCompletedKey(const IcomCivBackend& backend)
    {
        return backend.m_civScheduler.stats().lastCompletedKey;
    }

    // Connected and identified, with NOTHING in flight -- so a frame arriving
    // here is Observation::Unmatched rather than Accepted.
    static void prepareIdleSession(IcomCivBackend& backend,
                                   const IcomModel& model,
                                   std::uint64_t sessionGeneration)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = sessionGeneration;
    }

    static void prepareAcceptedPttRead(IcomCivBackend& backend,
                                       const IcomModel& model,
                                       std::uint64_t sessionGeneration)
    {
        backend.m_model = &model;
        backend.m_connected = true;
        backend.m_sessionGeneration = sessionGeneration;
        const std::vector<std::uint8_t> read =
            buildFrameSub(model.civAddress, cmd::kControl, control::kPtt);
        backend.queueRead(read, "ptt", IcomCivScheduler::Priority::Operator);
        (void)backend.m_civScheduler.takeNext(backend.nowMs());
    }
};

}  // namespace AetherSDR::icom

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const IcomModel* ic705 = modelForName("IC-705");
    check(ic705 != nullptr, "incident telemetry resolves the IC-705 profile");
    if (!ic705) {
        return 1;
    }

    IcomCivBackend backend;
    constexpr std::uint64_t kGeneration = 1;
    IcomCivBackendTestAccess::prepareExpiredKeyOn(
        backend, *ic705, kGeneration);

    CivFrame unkeyed;
    unkeyed.to = kControllerAddress;
    unkeyed.from = ic705->civAddress;
    unkeyed.cmd = cmd::kControl;
    unkeyed.hasSub = true;
    unkeyed.sub = control::kPtt;
    unkeyed.data = {0x00};
    IcomCivBackendTestAccess::deliver(backend, unkeyed, kGeneration);

    const QVariantMap incident = IcomCivBackendTestAccess::incident(backend);
    const QVariantMap ptt = incident.value(QStringLiteral("ptt")).toMap();
    const QVariantMap commandPlane =
        incident.value(QStringLiteral("commandPlane")).toMap();
    check(incident.value(QStringLiteral("kind")).toString()
                  == QLatin1String("ptt-not-confirmed")
              && incident.value(QStringLiteral("model")).toString()
                  == QLatin1String("IC-705"),
          "expired key-on records a typed, model-scoped incident");
    check(ptt.value(QStringLiteral("pendingIntent")).toBool()
              && ptt.value(QStringLiteral("intentKeyed")).toBool()
              && !ptt.value(QStringLiteral("publishedKeyed")).toBool(),
          "incident preserves requested and published PTT state before cleanup");
    check(commandPlane.contains(QStringLiteral("scheduler"))
              && commandPlane.contains(QStringLiteral("transactions")),
          "incident includes scheduler state and bounded transaction history");

    QVariantMap extensionResult;
    QObject::connect(&backend, &IRadioBackend::extensionResult, &app,
                     [&extensionResult](quint64 id, const QVariant& result) {
                         if (id == 0x1C1D) {
                             extensionResult = result.toMap();
                         }
                     });
    backend.invokeExtension(QStringLiteral("icom"),
                            QStringLiteral("civ.incident"), 0x1C1D, {});
    check(extensionResult.value(QStringLiteral("kind")).toString()
              == QLatin1String("ptt-not-confirmed"),
          "read-only CI-V incident verb returns the retained dossier");

    IcomCivBackend confirmationBackend;
    std::vector<bool> confirmations;
    QObject::connect(&confirmationBackend, &IRadioBackend::keyingStateConfirmed,
                     &app, [&confirmations](bool keyed) {
                         confirmations.push_back(keyed);
                     });
    IcomCivBackendTestAccess::prepareAcceptedPttRead(
        confirmationBackend, *ic705, kGeneration);
    check(confirmations.empty(),
          "queueing a PTT read publishes no optimistic radio confirmation");
    IcomCivBackendTestAccess::deliver(
        confirmationBackend, unkeyed, kGeneration);
    check(confirmations.size() == 1 && !confirmations.front(),
          "only an accepted CI-V PTT-off readback publishes confirmation");

    // ---- A REFUSED TUNE IS NOT A SUCCESSFUL ONE --------------------------
    //
    // FA is the radio's NG. observe() retires FB and FA identically — both
    // merely release the slot and carry no state — so before this, nothing in
    // the backend consumed a refusal and the optimistic frequency stood.
    // isNg() existed in CivCodec.h with no caller in the backend at all.
    //
    // Reachable in ordinary use on an IC-9700: three bands, two receivers, so
    // a receiver cannot take a band the other one already holds. The radio
    // answers cmd 05 with FA and does not move. Measured on hardware
    // 2026-08-29 — six cross-band sets, six FAs, display followed all six
    // (#4840).
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        IcomCivBackend refusedBackend;
        std::vector<double> published;
        QObject::connect(&refusedBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });
        QStringList warnings;
        QObject::connect(&refusedBackend, &IRadioBackend::configurationWarning,
                         &app, [&warnings](const QString& w) { warnings << w; });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyWrite(
            refusedBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;
        IcomCivBackendTestAccess::deliver(refusedBackend, refused, kGeneration);

        // The correction is deferred one event-loop turn, for the same reason
        // setSliceFrequency()'s out-of-band gate defers it: SliceModel has
        // already announced the operator's request, so a direct emit would be
        // announced away and the indicator would keep lying.
        QCoreApplication::processEvents();

        check(IcomCivBackendTestAccess::lastCompletedKey(refusedBackend)
                  == "frequency",
              "the FA retires the outstanding frequency write");
        check(!warnings.isEmpty()
                  && warnings.constLast().contains(QLatin1String("refused")),
              "a refused tune TELLS the operator the radio said no");
        // The load-bearing assertion: a backend that ignores FA publishes
        // nothing here, and the display keeps the frequency the radio rejected.
        check(published.size() == 1
                  && std::llround(published.front() * 1.0e6)
                         == static_cast<long long>(kHeldHz),
              "a refused tune republishes the radio's real VFO, not the "
              "frequency the radio rejected");
    }

    // AN UNMATCHED FA MUST NOT FIRE THE CORRECTION.
    //
    // The regression this pins: gating only on `lastCompletedKey == "frequency"`
    // is wrong, because observe() sets that key ONLY when a frame matches the
    // in-flight transaction. An unmatched FA returns Observation::Unmatched and
    // leaves the key at its previous value — and since frequency writes are the
    // most common transaction, the key is usually "frequency" from the last real
    // tune. So a stray or duplicate NG, or an NG for a transaction that already
    // expired, would fire the block with NO frequency write refused: a false
    // "the radio refused the tune" toast and a redundant re-assert.
    //
    // That is the lying-indicator failure this fix exists to remove, inverted —
    // which is why it gets its own row rather than being left to the Accepted
    // path above (that one passes either way, with or without the gate).
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        IcomCivBackend strayBackend;
        std::vector<double> published;
        QObject::connect(&strayBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });
        QStringList warnings;
        QObject::connect(&strayBackend, &IRadioBackend::configurationWarning,
                         &app, [&warnings](const QString& w) { warnings << w; });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyWrite(
            strayBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;

        // First FA: matches the in-flight write, retires it, corrects the
        // display. This is the legitimate case and it must still work.
        IcomCivBackendTestAccess::deliver(strayBackend, refused, kGeneration);
        QCoreApplication::processEvents();
        const std::size_t afterReal = published.size();
        const int warningsAfterReal = warnings.size();

        check(afterReal == 1 && warningsAfterReal == 1,
              "the matched FA still corrects exactly once");

        // Second FA: nothing is in flight now, so observe() returns Unmatched
        // and leaves lastCompletedKey at "frequency" from the write above.
        // Under the old predicate this fires again; under the Accepted gate it
        // must do nothing at all.
        check(IcomCivBackendTestAccess::lastCompletedKey(strayBackend)
                  == "frequency",
              "the stale key really does still read \"frequency\"");

        IcomCivBackendTestAccess::deliver(strayBackend, refused, kGeneration);
        QCoreApplication::processEvents();

        check(published.size() == afterReal,
              "an unmatched FA republishes NOTHING (no redundant re-assert)");
        check(warnings.size() == warningsAfterReal,
              "an unmatched FA does not tell the operator a tune was refused");
    }

    // AN FA TO A FREQUENCY *READ* IS NOT A REFUSED TUNE.
    //
    // semanticKey() folds the poll's 03 read and the +60 ms confirmation read
    // onto the same "frequency" key as the 05 write, and matches() retires any
    // in-flight transaction on an FA. So `Accepted && key == "frequency"` is
    // also true when the radio NGs a READ — and that must not tell the
    // operator a tune was refused, nor republish anything.
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        IcomCivBackend readBackend;
        std::vector<double> published;
        QObject::connect(&readBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });
        QStringList warnings;
        QObject::connect(&readBackend, &IRadioBackend::configurationWarning,
                         &app, [&warnings](const QString& w) { warnings << w; });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyRead(
            readBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;
        IcomCivBackendTestAccess::deliver(readBackend, refused, kGeneration);
        QCoreApplication::processEvents();

        check(IcomCivBackendTestAccess::lastCompletedKey(readBackend)
                  == "frequency",
              "the FA retires the outstanding frequency read under the same key");
        check(published.empty(),
              "an FA to a frequency READ republishes nothing");
        check(warnings.isEmpty(),
              "an FA to a frequency READ does not claim a tune was refused");
    }

    // THE DEFERRED CORRECTION MUST NOT STOMP A NEWER TUNE.
    //
    // The re-assert is one event-loop turn behind the FA. If the operator
    // issues another tune inside that gap, the correction is for a request
    // they have already moved past; firing it anyway drags the readout back
    // behind a write that may well succeed. The epoch guard drops it.
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        IcomCivBackend raceBackend;
        std::vector<double> published;
        QObject::connect(&raceBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });
        QStringList warnings;
        QObject::connect(&raceBackend, &IRadioBackend::configurationWarning,
                         &app, [&warnings](const QString& w) { warnings << w; });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyWrite(
            raceBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;
        IcomCivBackendTestAccess::deliver(raceBackend, refused, kGeneration);
        // A newer operator tune lands before the deferred correction fires.
        // (No session is attached, so nothing goes on the wire; the epoch
        // bump at the top of the seam verb is the part under test.)
        raceBackend.setSliceFrequency(0, 145'500'000.0);
        QCoreApplication::processEvents();

        check(warnings.size() == 1,
              "the refusal is still reported even when a newer tune follows");
        check(published.empty(),
              "a correction overtaken by a newer tune does not fire");
    }

    // THE CORRECTION PUBLISHES THE RADIO'S NEWEST WORD, NOT A SNAPSHOT.
    //
    // A 03 reply can land in the same gap and move m_frequencyHz. Capturing
    // the value at FA time would then publish a frequency the radio has
    // already left; reading it at fire time publishes where the radio is.
    {
        constexpr std::uint64_t kHeldHz = 145'030'000;
        constexpr std::uint64_t kMovedHz = 145'040'000;
        IcomCivBackend freshBackend;
        std::vector<double> published;
        QObject::connect(&freshBackend, &IRadioBackend::sliceChanged, &app,
                         [&published](int, const SliceDelta& delta) {
                             if (delta.frequency)
                                 published.push_back(*delta.frequency);
                         });

        IcomCivBackendTestAccess::prepareOutstandingFrequencyWrite(
            freshBackend, *ic705, kGeneration, kHeldHz);

        CivFrame refused;
        refused.to = kControllerAddress;
        refused.from = ic705->civAddress;
        refused.cmd = kCivNg;
        IcomCivBackendTestAccess::deliver(freshBackend, refused, kGeneration);
        IcomCivBackendTestAccess::setHeldFrequency(freshBackend, kMovedHz);
        QCoreApplication::processEvents();

        check(published.size() == 1
                  && std::llround(published.front() * 1.0e6)
                         == static_cast<long long>(kMovedHz),
              "a deferred correction publishes the frequency the radio holds "
              "when it fires, not the one it held at FA time");
    }

    IcomCivBackend freshBackend;
    IcomCivBackendTestAccess::prepareAcceptedPttRead(freshBackend, *ic705, kGeneration);
    IcomCivBackendTestAccess::identify(freshBackend);
    const auto snapshot = [&]() { return IcomCivBackendTestAccess::freshness(freshBackend); };
    const auto field = [&](const char* name) {
        return snapshot().value("fields").toMap().value(QLatin1String(name)).toMap();
    };
    check(!snapshot().value("trackedStateReady").toBool()
              && field("squelchPercent").value("status") == "never-confirmed",
          "transport and identity do not bless construction defaults");
    const auto deliver = [&](std::uint8_t command, bool hasSub, std::uint8_t sub,
                             std::vector<std::uint8_t> data, std::uint64_t generation = 1) {
        IcomCivBackendTestAccess::deliver(freshBackend,
            CivFrame{kControllerAddress, ic705->civAddress, command, hasSub, sub, data}, generation);
    };
    deliver(0x14, true, 0x03, {0xFA});
    deliver(0x14, true, 0x03, {0x00, 0x50}, 99);
    check(field("squelchPercent").value("status") == "never-confirmed",
          "malformed and previous-session frames cannot establish freshness");
    deliver(0x14, true, 0x03, {0x00, 0x51});
    check(field("squelchPercent").value("value").toInt() == 20
              && field("squelchPercent").value("status") == "confirmed",
          "decoded SQL reply confirms the radio value");
    IcomCivBackendTestAccess::age(freshBackend, QStringLiteral("civ.20.3"));
    check(field("squelchPercent").value("status") == "stale",
          "unchanged values still age out");
    deliver(0x14, true, 0x03, {0x00, 0x51});
    check(field("squelchPercent").value("status") == "confirmed",
          "unchanged valid replies refresh their own field");
    IcomCivBackendTestAccess::intent(freshBackend, cmdSetLevel(ic705->civAddress, level::kSquelch, 60));
    // PENDING IS ITS OWN AXIS. The write is in flight, so `pending` is true and
    // readiness is withheld -- but `status` keeps telling the truth about the
    // last confirmed value's age instead of being masked. Ranking `pending`
    // above every other branch let squelch, the one tracked key nothing
    // re-polls off the MK2 profile, latch `pending` for a whole session.
    check(field("squelchPercent").value("pending").toBool()
              && field("squelchPercent").value("status") == "confirmed",
          "write intent cannot masquerade as radio confirmation");
    deliver(0xFB, false, 0, {});
    check(field("squelchPercent").value("pending").toBool(),
          "generic ACK cannot confirm a state value");
    IcomCivBackendTestAccess::age(freshBackend, QStringLiteral("civ.20.3"));
    check(field("squelchPercent").value("pending").toBool()
              && field("squelchPercent").value("status") == "stale",
          "an unanswered write does not stop its field ageing out");
    deliver(0x14, true, 0x03, {0x00, 0x60});
    deliver(0x03, false, 0, {0x00, 0x00, 0x20, 0x07, 0x00});
    deliver(0x26, true, 0, {0x01, 0x00, 0x01});
    deliver(0x16, true, 0x12, {0x02});
    deliver(0x14, true, 0x0A, {0x00, 0x13});
    deliver(0x1C, true, 0, {0});
    check(snapshot().value("trackedStateReady").toBool(),
          "all six decoded fields establish bounded diagnostic readiness");
    deliver(0x26, true, 0, {0x02, 0x00, 0x01});
    check(!snapshot().value("trackedStateReady").toBool()
              && field("agcCode").value("status") == "previous-context",
          "radio-originated mode change invalidates old-context controls");
    deliver(0x16, true, 0x12, {0xFF});
    check(field("agcCode").value("status") == "previous-context",
          "out-of-range AGC cannot refresh the context");
    IcomCivBackendTestAccess::intent(freshBackend,
        buildFrameSub(ic705->civAddress, 0x07, 0x01));
    check(field("modeDataFilter").value("status") == "previous-context",
          "outgoing VFO selection invalidates even an identical mode and frequency");
    IcomCivBackend neverConfirmed;
    IcomCivBackendTestAccess::intent(neverConfirmed,
        cmdSetLevel(ic705->civAddress, level::kSquelch, 60));
    const QVariantMap unknownSql = IcomCivBackendTestAccess::freshness(neverConfirmed)
        .value("fields").toMap().value("squelchPercent").toMap();
    check(unknownSql.value("pending").toBool()
              && unknownSql.value("status") == "never-confirmed"
              && !unknownSql.value("value").isValid(),
          "a first write records pending intent without inventing a confirmed value");
    check(!snapshot().value("backendInstanceId").toString().isEmpty()
        && snapshot().value("backendInstanceId") != IcomCivBackendTestAccess::freshness(neverConfirmed).value("backendInstanceId"),
          "backend replacement in one process has a distinct diagnostic ID namespace");

    // ---- SQUELCH IS REPORTED BUT DOES NOT GATE READINESS -------------------
    //
    // level::kSquelch is re-read periodically only under the profile flag
    // pollCwSquelchAndTxBandwidth, which only the IC-7300MK2 sets. On this
    // IC-705 nothing reconciles it after connect, so an aggregate that required
    // it went false about five seconds in and stayed there for the session.
    // Its own backend: the checks above deliberately invalidate the context.
    IcomCivBackend readinessBackend;
    IcomCivBackendTestAccess::prepareAcceptedPttRead(readinessBackend, *ic705, kGeneration);
    IcomCivBackendTestAccess::identify(readinessBackend);
    const auto readySnapshot = [&]() {
        return IcomCivBackendTestAccess::freshness(readinessBackend);
    };
    const auto readyField = [&](const char* name) {
        return readySnapshot().value("fields").toMap().value(QLatin1String(name)).toMap();
    };
    const auto feed = [&](std::uint8_t command, bool hasSub, std::uint8_t sub,
                          std::vector<std::uint8_t> data) {
        IcomCivBackendTestAccess::deliver(readinessBackend,
            CivFrame{kControllerAddress, ic705->civAddress, command, hasSub, sub, data},
            kGeneration);
    };
    feed(0x14, true, 0x03, {0x00, 0x60});
    feed(0x03, false, 0, {0x00, 0x00, 0x20, 0x07, 0x00});
    feed(0x26, true, 0, {0x01, 0x00, 0x01});
    feed(0x16, true, 0x12, {0x02});
    feed(0x14, true, 0x0A, {0x00, 0x13});
    feed(0x1C, true, 0, {0});
    check(readySnapshot().value("trackedStateReady").toBool(),
          "the gating fields alone establish bounded diagnostic readiness");
    IcomCivBackendTestAccess::age(readinessBackend, QStringLiteral("civ.20.3"));
    check(readyField("squelchPercent").value("status") == "stale"
              && !readyField("squelchPercent").value("gatesReadiness").toBool()
              && readySnapshot().value("trackedStateReady").toBool(),
          "an unpolled squelch ages out without latching readiness false");
    IcomCivBackendTestAccess::age(readinessBackend, QStringLiteral("civ.22.18"));
    check(readyField("agcCode").value("gatesReadiness").toBool()
              && !readySnapshot().value("trackedStateReady").toBool(),
          "a field that IS reconciled still gates readiness when it goes stale");

    // ---- A MODEL WITH NO 0x26 ROUTE CAN STILL CONFIRM ITS MODE -------------
    //
    // confirmState("mode", ...) used to live only in the 0x26 decode, so a
    // model without IcomFeature::VfoMode — which is what onLinkTick polls with
    // 04 instead — could never reach `confirmed` on the mode field, and
    // trackedStateReady was unreachable for its whole session.
    const IcomModel* ic7300 = modelForName("IC-7300");
    check(ic7300 != nullptr && !profileFor(*ic7300).supports(IcomFeature::VfoMode),
          "the IC-7300 resolves and is the no-VfoMode case this pins");
    if (ic7300) {
        IcomCivBackend noVfoMode;
        IcomCivBackendTestAccess::prepareAcceptedPttRead(noVfoMode, *ic7300, kGeneration);
        IcomCivBackendTestAccess::identify(noVfoMode);
        const auto modeStatus = [&]() {
            return IcomCivBackendTestAccess::freshness(noVfoMode).value("fields")
                .toMap().value("modeDataFilter").toMap().value("status").toString();
        };
        check(modeStatus() == "never-confirmed", "no mode publication yet");
        IcomCivBackendTestAccess::deliver(noVfoMode,
            CivFrame{kControllerAddress, ic7300->civAddress, cmd::kReadMode,
                     false, 0, {0x01, 0x02}}, kGeneration);
        check(modeStatus() == "confirmed",
              "an 04 mode publication confirms on a model with no 26 route");
    }

    // ---- AN OFF-SHAPE PTT FRAME PUBLISHES BUT NEVER CONFIRMS ---------------
    //
    // 1C 00 answers one byte, 00 or 01. A payload we cannot parse is not
    // evidence — but it is not "unkeyed" either, and this is the fail-closed
    // path for a radio that reports KEYED after an unkey request, so it must
    // still reach the publish logic (Constitution VI).
    IcomCivBackend pttShape;
    IcomCivBackendTestAccess::prepareAcceptedPttRead(pttShape, *ic705, kGeneration);
    IcomCivBackendTestAccess::identify(pttShape);
    const auto pttField = [&]() {
        return IcomCivBackendTestAccess::freshness(pttShape)
            .value("fields").toMap().value("ptt").toMap();
    };
    std::vector<bool> shapeKeying;
    QObject::connect(&pttShape, &IRadioBackend::transmitChanged, &pttShape,
                     [&](const TransmitDelta& d) {
                         if (d.mox) { shapeKeying.push_back(*d.mox); }
                     });
    IcomCivBackendTestAccess::deliver(pttShape,
        CivFrame{kControllerAddress, ic705->civAddress, cmd::kControl, true,
                 control::kPtt, {0x01, 0x00}}, kGeneration);
    check(pttField().value("status") == "never-confirmed",
          "a two-byte PTT payload cannot become a confirmation");
    check(shapeKeying.size() == 1 && shapeKeying.front(),
          "...but an unparseable frame that says KEYED is still published");
    IcomCivBackendTestAccess::prepareAcceptedPttRead(pttShape, *ic705, kGeneration);
    IcomCivBackendTestAccess::deliver(pttShape,
        CivFrame{kControllerAddress, ic705->civAddress, cmd::kControl, true,
                 control::kPtt, {0x00}}, kGeneration);
    check(pttField().value("status") == "confirmed"
              && pttField().value("value").toBool() == false,
          "a well-formed PTT-off readback does confirm");
    // ...and says it was an ACCEPTED observation, which is what the TX harness
    // requires before it will call a radio unkeyed. The one frame that can land
    // here Stale -- a stale reply agreeing with a pending unkey intent -- still
    // publishes (Constitution VI) but reports accepted:false, and
    // tools/test_tx_meter_test.py pins that the gate refuses it.
    check(pttField().value("accepted").toBool(),
          "an accepted PTT readback is labelled as one");
    check(!pttField().value("pending").toBool(),
          "a confirmed PTT field carries no outstanding write");

    // UNMATCHED IS STILL AUTHORITATIVE. An unsolicited front-panel PTT frame,
    // and a reply slower than the scheduler's read wait, both arrive as
    // Observation::Unmatched -- this file says so at the CI-V recovery gate:
    // "Unmatched but still authoritative; Stale is the sole outcome that proves
    // a newer semantic generation replaced it." Labelling those accepted:false
    // would make the TX harness reject a real unkey on a loaded bus.
    IcomCivBackend unsolicited;
    IcomCivBackendTestAccess::prepareIdleSession(unsolicited, *ic705, kGeneration);
    IcomCivBackendTestAccess::identify(unsolicited);
    IcomCivBackendTestAccess::deliver(unsolicited,
        CivFrame{kControllerAddress, ic705->civAddress, cmd::kControl, true,
                 control::kPtt, {0x00}}, kGeneration);
    const QVariantMap unsolicitedPtt = IcomCivBackendTestAccess::freshness(unsolicited)
        .value("fields").toMap().value("ptt").toMap();
    check(unsolicitedPtt.value("status") == "confirmed"
              && unsolicitedPtt.value("accepted").toBool(),
          "an unmatched but authoritative PTT readback still counts as accepted");

    // THE INCIDENT SNAPSHOT REACHES THE DEFAULT LOG, so it carries statuses and
    // ages but not the operator's dial frequency. recordIncident() qCWarnings
    // this whole structure, and IcomCivScheduler's payload-free rule is about
    // that log, not only about the transaction ring.
    const QVariantMap redacted = IcomCivBackendTestAccess::freshness(freshBackend, false);
    const QVariantMap openFreq = snapshot().value("fields").toMap()
        .value("frequencyHz").toMap();
    const QVariantMap hiddenFreq = redacted.value("fields").toMap()
        .value("frequencyHz").toMap();
    check(openFreq.value("value").isValid() && !hiddenFreq.value("value").isValid()
              && hiddenFreq.value("valuesRedacted").toBool()
              && hiddenFreq.value("status") == openFreq.value("status")
              && hiddenFreq.value("semanticKey") == openFreq.value("semanticKey"),
          "a redacted snapshot keeps the diagnostic and drops the dial frequency");
    return failures == 0 ? 0 : 1;
}
