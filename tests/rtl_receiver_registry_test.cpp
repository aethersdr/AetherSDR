#include "core/backends/rtl/RtlReceiverRegistry.h"

#include <QCoreApplication>
#include <QThreadPool>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

using Registry = AetherSDR::rtl::RtlReceiverRegistry;
using namespace std::chrono_literals;
namespace {
// Test-only replacement of ordinary C++ new/new[]. The acquisition-thread
// guard excludes control/pool preparation, so a vector/new regression inside
// processBlock() is observable even when it does not allocate through WDSP.
// This does not intercept malloc/realloc, aligned new, or private allocators;
// WDSP's own allocation guard remains responsible for its C allocation path.
thread_local bool inCallback = false;
thread_local std::size_t callbackAllocations = 0;
std::atomic<std::size_t> totalCallbackAllocations {0};

void* allocateForTest(std::size_t size)
{
    if (inCallback) { ++callbackAllocations; }
    for (;;) {
        if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) { throw std::bad_alloc(); }
        handler();
    }
}
} // namespace

void* operator new(std::size_t size) { return allocateForTest(size); }
void* operator new[](std::size_t size) { return allocateForTest(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocateForTest(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocateForTest(size); } catch (...) { return nullptr; }
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

namespace {
int failures = 0;
int checks = 0;
void check(bool value, const char* message)
{
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    void wait()
    {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }
    bool await()
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 15s, [this] { return entered; });
    }
    void open()
    {
        const std::scoped_lock lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct Stats {
    std::atomic<int> calls {0};
    std::atomic<int> live {0};
    std::atomic<int> destroyed {0};
    std::atomic<int> callbackDestruction {0};
    std::atomic<int> heldDestruction {0};
    std::atomic<int> running {0};
    std::atomic<int> maximumRunning {0};
    std::atomic<bool> fail {false};
};
class FakeReceiver final : public Registry::Receiver {
public:
    explicit FakeReceiver(std::shared_ptr<Stats> stats) : m_stats(std::move(stats)) { ++m_stats->live; }
    ~FakeReceiver() override
    {
        ++m_stats->destroyed;
        --m_stats->live;
        if (inCallback) { ++m_stats->callbackDestruction; }
        if (held.load()) { ++m_stats->heldDestruction; }
    }
    WdspChannel::ProcessResult processIq(std::span<const float>, std::span<const float>) noexcept override
    { return WdspChannel::ProcessResult::Ok; }
    std::span<const float> left() const noexcept override { return m_output; }
    std::span<const float> right() const noexcept override { return m_output; }
    std::atomic<bool> held {false};
private:
    std::shared_ptr<Stats> m_stats;
    std::array<float, 1> m_output {};
};
Registry::Prepare factory(const std::shared_ptr<Stats>& stats, const std::shared_ptr<Gate>& gate = {})
{
    return [stats, gate](const Registry::ReceiverSpec&, WdspChannel::Reservation&, std::string& error)
        -> std::unique_ptr<Registry::Receiver> {
        const int call = ++stats->calls;
        const int running = ++stats->running;
        int maximum = stats->maximumRunning.load();
        while (maximum < running && !stats->maximumRunning.compare_exchange_weak(maximum, running)) {}
        if (gate && call == 1) { gate->wait(); }
        --stats->running;
        if (stats->fail.load()) {
            error = "injected preparation failure";
            return nullptr;
        }
        return std::make_unique<FakeReceiver>(stats);
    };
}
Registry::Capture capture(std::uint64_t generation = 1, std::uint64_t identity = 42)
{ return {identity, generation, 100000000, 2400000, 1000000, 1000000}; }
Registry::ReceiverSpec spec(Registry::Handle handle)
{
    Registry::ReceiverSpec value;
    value.handle = handle;
    value.passband = {handle.slot, 100000000, 150, 3000, 0, 0, 0};
    return value;
}
Registry::SampleBlock block(Registry::Capture descriptor = capture(), std::uint64_t first = 0)
{
    static const std::array<std::complex<float>, 64> iq {};
    return {0, descriptor, first, false, iq};
}
template<typename Predicate> bool until(Registry& registry, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        if (predicate(registry.service())) { return true; }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
bool ready(Registry& registry, std::uint64_t revision)
{
    return until(registry, [revision](const Registry::Status& status) {
        return status.prepared == revision && !status.preparing;
    });
}
struct Probe final : Registry::BlockProcessor {
    std::array<Registry::Handle, Registry::kMaxSlots> handles;
    std::size_t count = 0;
    std::uint64_t sample = 0;
    std::uint64_t session = 0;
    bool discontinuity = false;
    bool immutable = true;
    double filterHighHz = 0;
    std::shared_ptr<Gate> gate;
    void process(const Registry::SampleBlock& value, std::span<const Registry::ReceiverView> views) noexcept override
    {
        count = views.size();
        sample = value.firstSample;
        discontinuity = value.discontinuity;
        for (std::size_t i = 0; i < count; ++i) {
            handles[i] = views[i].spec->handle;
            static_cast<FakeReceiver*>(views[i].receiver)->held.store(true);
        }
        const double oldFilter = count ? views[0].spec->dsp.filterHighHz : 0;
        filterHighHz = oldFilter;
        if (gate) { gate->wait(); }
        if (count && oldFilter != views[0].spec->dsp.filterHighHz) { immutable = false; }
        for (const Registry::ReceiverView& view : views) {
            static_cast<FakeReceiver*>(view.receiver)->held.store(false);
        }
    }
};
bool countedProcessBlock(Registry::SampleReader& reader, Registry::BlockProcessor& processor,
    const Registry::SampleBlock& block)
{
    callbackAllocations = 0;
    inCallback = true;
    const bool result = reader.processBlock(block, processor);
    inCallback = false;
    totalCallbackAllocations.fetch_add(callbackAllocations, std::memory_order_relaxed);
    return result;
}
bool deliver(Registry::SampleReader& reader, Probe& probe, const Registry::SampleBlock& value = block())
{
    // The injected transport retains the beginSession token independently.
    // Explicit nonzero values exercise delayed samples from older sessions.
    Registry::SampleBlock delivery = value;
    if (delivery.session == 0) { delivery.session = probe.session; }
    return countedProcessBlock(reader, probe, delivery);
}
void drain()
{
    check(QThreadPool::globalInstance()->waitForDone(15000), "bounded worker-pool drain");
}

void publicationAndSlots()
{
    const auto stats = std::make_shared<Stats>();
    Registry registry({4, 2, 6}, factory(stats));
    check(static_cast<bool>(registry), "valid registry admitted");
    const std::uint64_t session = registry.beginSession(capture());
    const auto a = registry.reserveSlot(0);
    const auto b = registry.reserveSlot(2);
    check(a && b && a->session == session, "sparse stable slots reserved in current session");
    check(!registry.reserveSlot(1), "pending reservations count against receiver capacity despite free UI slots");
    if (!a || !b) { return; }
    std::array desired {spec(*a), spec(*b)};
    auto reader = registry.attachReader();
    check(static_cast<bool>(reader) && !registry.attachReader(), "only one acquisition reader attaches");
    check(registry.submit(capture(), desired) == Registry::Result::Accepted, "two receiver bank admitted");
    check(ready(registry, 1), "two receivers prepare asynchronously");
    Probe probe;
    probe.session = session;
    Registry::Capture wrong = capture();
    wrong.achievedSampleRateHz = 2000000;
    check(!deliver(reader, probe, block(wrong)), "same identity/generation with different achieved rate is refused");
    check(registry.service().published == 0, "wrong metadata does not publish bank");
    std::array<std::complex<float>, 1> nonfinite {{{std::numeric_limits<float>::quiet_NaN(), 0}}};
    Registry::SampleBlock malformed = block();
    malformed.samples = nonfinite;
    check(!deliver(reader, probe, malformed), "nonfinite IQ is refused before publication");
    check(registry.service().published == 0, "malformed sample block does not publish bank");
    check(deliver(reader, probe), "complete prepared bank publishes on matching sample boundary");
    check(totalCallbackAllocations == 0, "publication performs no ordinary C++ allocation");
    check(probe.count == 2 && probe.handles[0] == *a && probe.handles[1] == *b, "sparse slots preserve identities");
    check(probe.discontinuity, "first bank delivery marks discontinuity");
    check(deliver(reader, probe, block(capture(), 64)) && !probe.discontinuity, "contiguous next block retains continuity");
    check(totalCallbackAllocations == 0, "ongoing processing performs no ordinary C++ allocation");
    check(deliver(reader, probe, block(capture(), 192)) && probe.discontinuity, "missing sample positions mark discontinuity");

    const auto held = std::make_shared<Gate>();
    Probe heldProbe;
    heldProbe.session = session;
    heldProbe.gate = held;
    bool heldResult = false;
    std::thread acquisition([&] { heldResult = deliver(reader, heldProbe); });
    check(held->await(), "acquisition enters held sample view");
    desired[0].dsp.filterHighHz = 2200;
    check(registry.submit(capture(), desired) == Registry::Result::Accepted, "replacement prepares while old block is held");
    check(ready(registry, 2), "replacement ready without interrupting active bank");
    check(stats->destroyed == 0 && registry.service().residentReceivers == 4, "active plus offered resources remain charged");
    held->open();
    acquisition.join();
    check(heldResult && heldProbe.immutable, "held spec stays immutable through preparation");
    check(deliver(reader, probe), "replacement adopted only on next block");
    check(totalCallbackAllocations == 0, "replacement publication and retirement acknowledgment perform no ordinary C++ allocation");
    check(until(registry, [&](const auto&) { return stats->destroyed == 2; }), "acknowledged old bank destroyed off callback");
    check(stats->callbackDestruction == 0 && stats->heldDestruction == 0, "no callback or held-view destruction");

    stats->fail = true;
    check(registry.submit(capture(), std::span(desired).first(1)) == Registry::Result::Accepted, "removal replacement admitted");
    check(until(registry, [](const auto& status) { return status.result == Registry::Result::PreparationFailed; }), "failed removal is observable");
    check(deliver(reader, probe) && probe.count == 2, "failed removal preserves full previous bank");
    stats->fail = false;
    check(registry.submit(capture(), desired) == Registry::Result::Accepted, "failed removal preserves previous live handle for retry");
    check(ready(registry, 4), "full previous set retry prepares");
    check(deliver(reader, probe), "retry publishes");
    check(until(registry, [](const auto& status) { return status.residentReceivers == 2; }), "retry retirement drains");
    check(registry.submit(capture(), std::span(desired).first(1)) == Registry::Result::Accepted, "middle-slot removal prepares");
    check(ready(registry, 5), "removal ready");
    check(!registry.reserveSlot(2), "slot cannot be reused before removal acknowledgment");
    check(deliver(reader, probe) && probe.count == 1 && probe.handles[0] == *a, "survivor keeps stable handle");
    check(until(registry, [](const auto& status) { return status.residentReceivers == 1; }), "removed slot destruction acknowledged");
    check(registry.submit(capture(), desired) == Registry::Result::Invalid, "fully retired handle cannot resurrect a receiver before slot reuse");
    const auto reused = registry.reserveSlot(2);
    check(reused && reused->instance != b->instance && reused->session == b->session, "retired slot reuse advances instance only");
    check(registry.submit(capture(), desired) == Registry::Result::Invalid, "previous occupant cannot control reused slot");
    reader.stop();
    check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "stopped reader acknowledges final active bank");
    check(stats->live == 0, "all fake receiver resources released");
}

void coalescingAndOwnerLifetime()
{
    const auto stats = std::make_shared<Stats>();
    const auto gate = std::make_shared<Gate>();
    auto first = std::make_unique<Registry>(Registry::Limits {2, 2, 4}, factory(stats, gate));
    const std::uint64_t oldSession = first->beginSession(capture());
    const auto oldHandle = first->reserveSlot(0);
    if (!oldHandle) { check(false, "initial lifetime handle"); return; }
    Registry::ReceiverSpec old = spec(*oldHandle);
    check(first->submit(capture(), std::span(&old, 1)) == Registry::Result::Accepted, "initial blocked request admitted");
    check(gate->await(), "preparation blocked outside owner");
    first.reset(); // must return while its FFTW-equivalent call is blocked
    Registry second({2, 2, 4}, factory(stats, gate));
    const std::uint64_t newSession = second.beginSession(capture());
    check(newSession != oldSession, "session identity cannot collide after owner recreation");
    const auto freshHandle = second.reserveSlot(0);
    if (!freshHandle) { gate->open(); check(false, "replacement lifetime handle"); return; }
    Registry::ReceiverSpec fresh = spec(*freshHandle);
    for (int i = 0; i < 200; ++i) {
        fresh.dsp.filterHighHz = 2000 + i;
        check(second.submit(capture(), std::span(&fresh, 1)) == Registry::Result::Accepted, "latest whole desired set coalesces");
    }
    Registry other({1, 1, 2}, factory(stats));
    other.beginSession(capture(1, 99));
    const auto otherHandle = other.reserveSlot();
    if (otherHandle) {
        Registry::ReceiverSpec otherSpec = spec(*otherHandle);
        check(other.submit(capture(1, 99), std::span(&otherSpec, 1)) == Registry::Result::Busy,
            "independent pending owner is refused, never overwrites another capture");
    } else { check(false, "independent registry handle"); }
    check(stats->calls == 1, "recreated owners cannot accumulate executing preparation jobs");
    gate->open();
    check(ready(second, 200), "only latest replacement owner's configuration completes");
    check(stats->calls == 2 && stats->maximumRunning == 1, "one old executing plus one coalesced request across owners");
    check(stats->destroyed == 1, "stale old owner's result destroyed off-thread");
    auto reader = second.attachReader();
    Probe probe;
    probe.session = newSession;
    Registry::SampleBlock oldBlock = block();
    oldBlock.session = oldSession;
    check(!deliver(reader, probe, oldBlock) && second.service().published == 0,
        "old session IQ cannot adopt a new bank even with identical capture metadata");
    check(deliver(reader, probe) && probe.handles[0] == *freshHandle, "old endpoint completion cannot publish into recreated owner");
    check(probe.filterHighHz == 2199, "coalescing preserves the last complete desired configuration");
    reader.stop();
    check(until(second, [](const auto& status) { return status.residentReceivers == 0; }), "replacement owner drains");
}

void pressureFailureAndValidation()
{
    const auto stats = std::make_shared<Stats>();
    Registry registry({2, 2, 4}, factory(stats));
    const std::uint64_t session = registry.beginSession(capture());
    const auto handle = registry.reserveSlot();
    if (!handle) { check(false, "pressure handle"); return; }
    Registry::ReceiverSpec desired = spec(*handle);
    check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Accepted, "offer without reader");
    check(ready(registry, 1), "initial unconsumed offer ready");
    desired.dsp.filterHighHz = 2500;
    check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Accepted, "supersede offer without reader");
    check(ready(registry, 2), "control reaps stale offer without waiting for acquisition start");
    auto reader = registry.attachReader();
    for (int i = 0; i < 100; ++i) {
        desired.dsp.filterHighHz = 2000 + i;
        check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Accepted, "pressure coalesces with attached stalled reader");
    }
    drain();
    check(stats->calls == 2, "unconsumed offered bank bounds preparation under no reader progress");
    Probe probe;
    probe.session = session;
    check(!deliver(reader, probe), "stale offer retires instead of publishing");
    check(ready(registry, 102), "reader acknowledgment allows latest pending bank preparation");
    check(deliver(reader, probe), "latest bank publishes after pressure");
    check(registry.cancelReservation(*handle) == Registry::Result::Busy, "live handle cannot be canceled as unused reservation");
    Registry::ReceiverSpec invalid = desired;
    invalid.dsp.filterTaps = std::numeric_limits<int>::max();
    check(registry.submit(capture(), std::span(&invalid, 1)) == Registry::Result::Invalid, "unbounded DSP allocation rejected at ingress");
    invalid = desired;
    invalid.dsp.blockForOutput = true;
    check(registry.submit(capture(), std::span(&invalid, 1)) == Registry::Result::Invalid, "blocking WDSP output is refused on acquisition path");
    invalid = desired;
    invalid.passband.filterHighHz = 1100000;
    check(registry.submit(capture(), std::span(&invalid, 1)) == Registry::Result::Invalid, "full passband must fit actual capture");
    Registry::SampleBlock malformed = block();
    malformed.firstSample = std::numeric_limits<std::uint64_t>::max();
    check(!deliver(reader, probe, malformed), "sample position overflow refused");
    malformed = block();
    malformed.samples = {};
    check(!deliver(reader, probe, malformed), "empty null IQ span refused at ingress");
    std::vector<std::complex<float>> tooMany(Registry::kMaxInputSamples + 1);
    malformed = block();
    malformed.samples = tooMany;
    check(!deliver(reader, probe, malformed), "oversized sample view refused");
    Registry::Capture next = capture(2);
    next.centerHz += 1000;
    check(registry.submit(next, std::span(&desired, 1)) == Registry::Result::Accepted, "new capture generation prepares as whole bank");
    check(ready(registry, 103), "capture replacement ready");
    check(deliver(reader, probe), "old capture still uses old active bank while new offer waits");
    check(deliver(reader, probe, block(next)) && probe.discontinuity, "new actual capture atomically adopts compatible bank and resets continuity");
    check(!deliver(reader, probe), "old capture cannot feed newly published bank");
    check(until(registry, [](const auto& status) { return status.residentReceivers == 1; }), "capture replacement retirement drains");
    stats->fail = true;
    Registry::Capture future = next;
    future.generation = 3;
    future.centerHz += 1000;
    check(registry.submit(future, std::span(&desired, 1)) == Registry::Result::Accepted, "future capture preparation admitted");
    check(until(registry, [](const auto& status) { return status.result == Registry::Result::PreparationFailed; }), "future capture preparation fails observably");
    stats->fail = false;
    check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Invalid, "arbitrary older capture cannot bypass generation high-water");
    Registry::Capture changed = next;
    changed.centerHz += 100;
    check(registry.submit(changed, std::span(&desired, 1)) == Registry::Result::Invalid, "modified old descriptor is not the active-capture exception");
    check(registry.submit(next, std::span(&desired, 1)) == Registry::Result::Accepted, "failed future preparation does not prevent editing exact still-active capture");
    check(ready(registry, 105), "still-active capture retry prepares");
    check(deliver(reader, probe, block(next)), "still-active capture retry publishes safely");
    changed = future;
    changed.centerHz += 100;
    check(registry.submit(changed, std::span(&desired, 1)) == Registry::Result::Invalid, "old-capture retry does not lower remembered generation high-water");
    reader.stop();
    check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "pressure session drains");
}

void dspGeometryValidation()
{
    const auto stats = std::make_shared<Stats>();
    Registry registry({1, 1, 2}, factory(stats));
    const std::uint64_t session = registry.beginSession(capture());
    const auto handle = registry.reserveSlot();
    if (!handle) { check(false, "DSP geometry handle"); return; }
    Registry::ReceiverSpec desired = spec(*handle);
    auto reader = registry.attachReader();
    Probe probe;
    probe.session = session;
    check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Accepted,
        "geometry baseline admitted");
    check(ready(registry, 1) && deliver(reader, probe), "geometry baseline active");
    struct Rates { int input; int dsp; int output; };
    // 64 / 96 becomes zero; 64 / 24 truncates. Test both DSP directions.
    // The final case has exact DSP-side sizes but a fractional exchange ratio.
    const std::array invalidRates {
        Rates {8000, 768000, 8000}, Rates {16384, 1048576, 8192},
        Rates {8000, 192000, 48000},
        Rates {64000, 32000, 96000}
    };
    for (const Rates& rates : invalidRates) {
        Registry::ReceiverSpec invalid = desired;
        invalid.dsp.inputBlockSize = 64;
        invalid.dsp.dspBlockSize = 64;
        invalid.dsp.inputSampleRate = rates.input;
        invalid.dsp.dspSampleRate = rates.dsp;
        invalid.dsp.outputSampleRate = rates.output;
        check(registry.submit(capture(), std::span(&invalid, 1)) == Registry::Result::Invalid,
            "unsafe derived DSP geometry refused before preparation");
        check(registry.service().requested == 1 && stats->calls == 1,
            "geometry refusal preserves revision and does not invoke factory");
        check(deliver(reader, probe) && probe.handles[0] == *handle,
            "geometry refusal preserves active receiver and handle");
    }
    // Power-of-two conversions retain exact positive DSP input/output sizes.
    for (const Rates& rates : {Rates {8000, 512000, 8000}, Rates {48000, 192000, 48000},
                              Rates {192000, 192000, 48000}}) {
        desired.dsp.inputBlockSize = 64;
        desired.dsp.dspBlockSize = 64;
        desired.dsp.inputSampleRate = rates.input;
        desired.dsp.dspSampleRate = rates.dsp;
        desired.dsp.outputSampleRate = rates.output;
        const std::uint64_t revision = registry.service().requested + 1;
        check(registry.submit(capture(), std::span(&desired, 1)) == Registry::Result::Accepted,
            "exact positive DSP geometry admitted");
        check(ready(registry, revision) && deliver(reader, probe), "valid geometry publishes");
        check(until(registry, [](const auto& status) { return status.residentReceivers == 1; }),
            "valid geometry replacement drains");
    }
    reader.stop();
    check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }),
        "geometry session drains");
}

void preparationExceptions()
{
    const auto stats = std::make_shared<Stats>();
    const auto failureMode = std::make_shared<std::atomic<int>>(0);
    Registry registry({2, 2, 4}, [stats, failureMode](const Registry::ReceiverSpec& value,
        WdspChannel::Reservation&, std::string&) -> std::unique_ptr<Registry::Receiver> {
        if (value.handle.slot == 1) {
            if (failureMode->load() == 1) { throw std::runtime_error("injected standard exception"); }
            if (failureMode->load() == 2) { throw 7; }
        }
        return std::make_unique<FakeReceiver>(stats);
    });
    const std::uint64_t session = registry.beginSession(capture());
    const auto a = registry.reserveSlot(0);
    const auto b = registry.reserveSlot(1);
    if (!a || !b) { check(false, "exception recovery handles"); return; }
    std::array desired {spec(*a), spec(*b)};
    auto reader = registry.attachReader();
    Probe probe;
    probe.session = session;
    check(registry.submit(capture(), desired) == Registry::Result::Accepted, "initial exception-test bank admitted");
    check(ready(registry, 1) && deliver(reader, probe), "initial exception-test bank active");
    for (int mode : {1, 2}) {
        const double previousFilter = probe.filterHighHz;
        const int previousDestruction = stats->destroyed.load();
        desired[0].dsp.filterHighHz = 2100 + mode * 100;
        failureMode->store(mode);
        check(registry.submit(capture(), desired) == Registry::Result::Accepted, "throwing replacement admitted");
        check(until(registry, [](const Registry::Status& status) {
            return status.result == Registry::Result::PreparationFailed && !status.preparing &&
                !status.pending && status.residentReceivers == 2;
        }), "exception reports failure and releases partial-bank capacity");
        const std::string expected = mode == 1 ? "injected standard exception" :
            "The preparation callable threw a non-standard exception";
        check(registry.service().error == expected, "exception diagnostic retained");
        check(stats->live == 2 && stats->destroyed == previousDestruction + 1,
            "receiver created before exception is destroyed while active bank survives");
        check(deliver(reader, probe) && probe.count == 2 && probe.handles[0] == *a &&
            probe.handles[1] == *b && probe.filterHighHz == previousFilter,
            "exception preserves active configuration and handles");
        check(WdspChannel::reserveChannels(30).has_value(), "exception returns all partial-bank pool reservations");
        check(!WdspChannel::reserveChannels(31), "active bank still owns its two pool slots");
        failureMode->store(0);
        check(registry.submit(capture(), desired) == Registry::Result::Accepted, "same handles retry after exception");
        check(ready(registry, 1 + mode * 2), "executor prepares another request after exception");
        check(deliver(reader, probe) && probe.filterHighHz == desired[0].dsp.filterHighHz,
            "successful retry publishes replacement configuration");
        check(until(registry, [](const Registry::Status& status) { return status.residentReceivers == 2; }),
            "retry retires previous active bank");
    }
    reader.stop();
    check(until(registry, [](const Registry::Status& status) { return status.residentReceivers == 0; }),
        "exception recovery session drains");
    check(stats->live == 0 && WdspChannel::reserveChannels(32).has_value(), "exception recovery returns entire pool");
}

void capacityAndStoppedPreparation()
{
    const auto stats = std::make_shared<Stats>();
    {
        Registry registry({1, 1, 1}, factory(stats));
        const std::uint64_t session = registry.beginSession(capture());
        const auto handle = registry.reserveSlot();
        if (!handle) { check(false, "capacity handle"); return; }
        Registry::ReceiverSpec desired = spec(*handle);
        auto reader = registry.attachReader();
        registry.submit(capture(), std::span(&desired, 1));
        check(ready(registry, 1), "single-resident initial bank prepares");
        Probe probe;
        probe.session = session;
        check(deliver(reader, probe), "single-resident bank active");
        registry.submit(capture(), std::span(&desired, 1));
        check(until(registry, [](const auto& status) { return status.result == Registry::Result::ResourceLimit; }),
            "old plus new overlap refuses beyond resident capacity");
        check(deliver(reader, probe) && registry.service().published == 1, "overlap refusal preserves old receiver");
        reader.stop();
        check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "capacity bank drains");
    }
    drain();
    {
        auto competing = WdspChannel::reserveChannels(32);
        Registry registry({1, 1, 2}, factory(stats));
        registry.beginSession(capture());
        const auto handle = registry.reserveSlot();
        if (!handle) { check(false, "pool capacity handle"); return; }
        Registry::ReceiverSpec desired = spec(*handle);
        registry.submit(capture(), std::span(&desired, 1));
        check(until(registry, [](const auto& status) { return status.result == Registry::Result::ResourceLimit; }),
            "non-RTL shared pool consumption prevents preparation atomically");
        competing.reset();
        registry.submit(capture(), std::span(&desired, 1));
        check(ready(registry, 2), "pool failure preserves reservation for retry");
        registry.cancelSession();
        check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "no-reader cancellation acknowledges ready bank");
    }
    drain();
    {
        const auto gate = std::make_shared<Gate>();
        const auto stoppedStats = std::make_shared<Stats>();
        Registry registry({1, 1, 2}, factory(stoppedStats, gate));
        registry.beginSession(capture());
        const auto handle = registry.reserveSlot();
        if (!handle) { check(false, "stopped preparation handle"); return; }
        Registry::ReceiverSpec desired = spec(*handle);
        auto reader = registry.attachReader();
        registry.submit(capture(), std::span(&desired, 1));
        check(gate->await(), "preparation active before acquisition stop");
        reader.stop();
        gate->open();
        check(until(registry, [](const auto& status) { return status.residentReceivers == 0 && !status.preparing; }),
            "stop invalidates uncancellable job and retires it without another callback");
        check(stoppedStats->live == 0 && stoppedStats->destroyed == 1, "stopped job result cannot remain offered");
    }
}

void reconnectAndRegistryBound()
{
    const auto stats = std::make_shared<Stats>();
    const auto gate = std::make_shared<Gate>();
    {
        Registry registry({2, 2, 4}, factory(stats, gate));
        const std::uint64_t oldSession = registry.beginSession(capture());
        const auto oldHandle = registry.reserveSlot(0);
        if (!oldHandle) { check(false, "reconnect original handle"); return; }
        Registry::ReceiverSpec desired = spec(*oldHandle);
        auto reader = registry.attachReader();
        registry.submit(capture(), std::span(&desired, 1));
        check(gate->await(), "old session preparation held for reconnect");
        const std::uint64_t newSession = registry.beginSession(capture(2));
        check(newSession != oldSession && registry.service().published == 0,
            "reconnect invalidates session identity and current publication");
        check(!registry.reserveSlot(0), "uncancellable old preparation keeps its UI slot reserved");
        const auto newHandle = registry.reserveSlot(1);
        if (!newHandle) { gate->open(); check(false, "reconnect replacement handle"); return; }
        desired = spec(*newHandle);
        registry.submit(capture(2), std::span(&desired, 1));
        check(stats->calls == 1, "reconnect does not add a parallel preparation job");
        gate->open();
        check(ready(registry, 2), "new session prepares after stale old job returns");
        Probe probe;
        probe.session = newSession;
        check(!deliver(reader, probe), "old capture generation cannot publish new session bank");
        check(deliver(reader, probe, block(capture(2))) && probe.handles[0] == *newHandle,
            "new session bank adopts exact compatible capture");
        const auto freed = registry.reserveSlot(0);
        check(freed && freed->session == newSession && freed->instance != oldHandle->instance,
            "canceled old preparation frees slot only after resource destruction");
        reader.stop();
        check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "reconnected session drains");
    }
    drain();
    std::vector<Registry::SampleReader> retainedReaders;
    for (std::size_t i = 0; i < Registry::kMaxRegistries; ++i) {
        auto owner = std::make_unique<Registry>(Registry::Limits {1, 1, 2}, factory(stats));
        check(static_cast<bool>(*owner), "bounded retiring registry owner admitted");
        owner->beginSession(capture(1, 100 + i));
        retainedReaders.push_back(owner->attachReader());
        owner.reset(); // reader keeps retirement state until acquisition stops
    }
    check(!Registry({1, 1, 2}, factory(stats)), "unacknowledged old readers cannot accumulate unbounded registry states");
    for (Registry::SampleReader& reader : retainedReaders) { reader.stop(); }
    retainedReaders.clear();
    drain();
    Registry fresh({1, 1, 2}, factory(stats));
    check(static_cast<bool>(fresh), "stopped readers release process-wide registry capacity");
}

void actualPreparedDsp()
{
    Registry registry({1, 1, 2}); // production factory, real reserved WDSP channel
    const std::uint64_t session = registry.beginSession(capture());
    const auto handle = registry.reserveSlot();
    if (!handle) { check(false, "actual WDSP handle"); return; }
    Registry::ReceiverSpec desired = spec(*handle);
    auto reader = registry.attachReader();
    registry.submit(capture(), std::span(&desired, 1));
    check(ready(registry, 1), "production factory prepares WDSP on Qt pool");
    struct DspProbe final : Registry::BlockProcessor {
        bool valid = false;
        void process(const Registry::SampleBlock&, std::span<const Registry::ReceiverView> views) noexcept override
        {
            std::array<float, 1024> silence {};
            if (views.size() != 1) { return; }
            if (views[0].receiver->processIq({}, silence) !=
                WdspChannel::ProcessResult::InvalidBuffer) { return; }
            const WdspChannel::ProcessResult result = views[0].receiver->processIq(silence, silence);
            valid = (result == WdspChannel::ProcessResult::Ok || result == WdspChannel::ProcessResult::Underrun) &&
                views[0].receiver->left().size() == 1024 && views[0].receiver->right().size() == 1024;
        }
    } probe;
    Registry::SampleBlock delivery = block();
    delivery.session = session;
    check(countedProcessBlock(reader, probe, delivery) && probe.valid, "published production receiver processes fixed-size IQ without control work");
    check(totalCallbackAllocations == 0, "actual prepared WDSP receiver delivery performs no ordinary C++ allocation");
    reader.stop();
    check(until(registry, [](const auto& status) { return status.residentReceivers == 0; }), "actual WDSP destruction acknowledged off acquisition");
    check(WdspChannel::reserveChannels(32).has_value(), "actual registry returns all global channel slots");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    // Direct calls prevent allocation-expression elision from weakening this
    // self-check: both replacement entry points must reach the guarded count.
    inCallback = true;
    void* scalar = ::operator new(1);
    void* array = ::operator new[](1);
    ::operator delete(scalar);
    ::operator delete[](array);
    inCallback = false;
    check(callbackAllocations == 2, "ordinary scalar and array allocation counter is active");
    check(!Registry({0, 0, 0}), "invalid registry limits refuse");
    check(!Registry({9, 1, 2}), "storage ceiling cannot be exceeded");
    publicationAndSlots(); drain();
    coalescingAndOwnerLifetime(); drain();
    pressureFailureAndValidation(); drain();
    dspGeometryValidation(); drain();
    preparationExceptions(); drain();
    capacityAndStoppedPreparation(); drain();
    reconnectAndRegistryBound(); drain();
    actualPreparedDsp(); drain();
    check(totalCallbackAllocations == 0, "all acquisition callback paths remained free of ordinary C++ allocations");
    std::cout << "RTL receiver registry: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
