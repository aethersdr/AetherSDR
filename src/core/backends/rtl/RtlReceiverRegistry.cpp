#include "core/backends/rtl/RtlReceiverRegistry.h"

#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace AetherSDR::rtl {
namespace {
using Registry = RtlReceiverRegistry;
constexpr std::size_t kBankCount = 3;
enum class Stage { Free, Preparing, Offered, Active, Retired, Destroying };
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<Stage>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

bool validCapture(const Registry::Capture& capture)
{
    return capture.captureId != 0 && capture.generation != 0 &&
        std::isfinite(capture.centerHz) && capture.centerHz >= 0 &&
        capture.centerHz <= SharedCapturePolicy::kMaxMagnitudeHz &&
        std::isfinite(capture.achievedSampleRateHz) &&
        capture.achievedSampleRateHz > 0 &&
        capture.achievedSampleRateHz <= SharedCapturePolicy::kMaxMagnitudeHz &&
        std::isfinite(capture.usableLeftHz) && capture.usableLeftHz >= 0 &&
        capture.usableLeftHz <= capture.achievedSampleRateHz / 2 &&
        std::isfinite(capture.usableRightHz) && capture.usableRightHz >= 0 &&
        capture.usableRightHz <= capture.achievedSampleRateHz / 2;
}

bool boundedDsp(const WdspChannel::Config& config)
{
    const auto blockSize = [](std::size_t value) {
        return value >= 64 && value <= 16384 && (value & (value - 1)) == 0;
    };
    const auto rate = [](int value) { return value >= 8000 && value <= 3072000; };
    return config.direction == WdspChannel::Direction::Receive && !config.blockForOutput &&
        blockSize(config.inputBlockSize) && blockSize(config.dspBlockSize) &&
        rate(config.inputSampleRate) && rate(config.dspSampleRate) &&
        config.outputSampleRate >= 8000 && config.outputSampleRate <= 192000 &&
        (config.inputSampleRate % config.dspSampleRate == 0 ||
         config.dspSampleRate % config.inputSampleRate == 0) &&
        (config.outputSampleRate % config.dspSampleRate == 0 ||
         config.dspSampleRate % config.outputSampleRate == 0) &&
        // WDSP channel.c derives DSP-side input/output sizes with integer
        // division. Require exact, nonzero sizes before iobuffs.c uses them
        // as divisors. Its exchange size also requires an integral rate ratio.
        config.dspBlockSize * static_cast<std::size_t>(config.inputSampleRate) %
            static_cast<std::size_t>(config.dspSampleRate) == 0 &&
        config.dspBlockSize * static_cast<std::size_t>(config.outputSampleRate) %
            static_cast<std::size_t>(config.dspSampleRate) == 0 &&
        (config.inputSampleRate % config.outputSampleRate == 0 ||
         config.outputSampleRate % config.inputSampleRate == 0) &&
        (config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate)) %
            static_cast<std::size_t>(config.inputSampleRate) == 0 &&
        config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate) /
            static_cast<std::size_t>(config.inputSampleRate) <= Registry::kMaxInputSamples &&
        static_cast<int>(config.mode) >= static_cast<int>(WdspChannel::Mode::Lsb) &&
        static_cast<int>(config.mode) <= static_cast<int>(WdspChannel::Mode::Wbfm) &&
        std::isfinite(config.filterLowHz) && std::isfinite(config.filterHighHz) &&
        config.filterLowHz < config.filterHighHz &&
        config.filterLowHz >= -config.dspSampleRate / 2.0 &&
        config.filterHighHz <= config.dspSampleRate / 2.0 &&
        config.filterTaps >= 64 && config.filterTaps <= 16384 &&
        (config.filterTaps & (config.filterTaps - 1)) == 0 &&
        config.agcMode >= 0 && config.agcMode <= 4 &&
        std::isfinite(config.maximumAgcGainDb) && config.maximumAgcGainDb >= -100 &&
        config.maximumAgcGainDb <= 150 && config.agcSlopeDb >= 0 && config.agcSlopeDb <= 100 &&
        std::isfinite(config.agcFixedGainDb) && config.agcFixedGainDb >= -100 &&
        config.agcFixedGainDb <= 150 && config.noiseBlankerLevel >= 0 &&
        config.noiseBlankerLevel <= 100 &&
        std::isfinite(config.muteDelayUpSec) && config.muteDelayUpSec >= 0 && config.muteDelayUpSec <= 1 &&
        std::isfinite(config.muteSlewUpSec) && config.muteSlewUpSec >= 0 && config.muteSlewUpSec <= 1 &&
        std::isfinite(config.muteDelayDownSec) && config.muteDelayDownSec >= 0 && config.muteDelayDownSec <= 1 &&
        std::isfinite(config.muteSlewDownSec) && config.muteSlewDownSec >= 0 && config.muteSlewDownSec <= 1;
}

class WdspReceiver final : public Registry::Receiver {
public:
    explicit WdspReceiver(std::unique_ptr<WdspChannel> channel)
        : m_channel(std::move(channel)), m_left(m_channel->outputBlockSize()),
          m_right(m_channel->outputBlockSize()) {}
    WdspChannel::ProcessResult processIq(std::span<const float> i,
        std::span<const float> q) noexcept override
    {
        if (i.data() == nullptr || q.data() == nullptr) {
            return WdspChannel::ProcessResult::InvalidBuffer;
        }
        return m_channel->processIq(i, q, m_left, m_right);
    }
    std::span<const float> left() const noexcept override { return m_left; }
    std::span<const float> right() const noexcept override { return m_right; }
private:
    std::unique_ptr<WdspChannel> m_channel;
    std::vector<float> m_left;
    std::vector<float> m_right;
};

std::unique_ptr<Registry::Receiver> prepareWdsp(const Registry::ReceiverSpec& spec,
    WdspChannel::Reservation& reservation, std::string& error)
{
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(spec.dsp, reservation, &error);
    if (!channel) {
        return nullptr;
    }
    return std::make_unique<WdspReceiver>(std::move(channel));
}

struct Request {
    std::uint64_t session = 0;
    std::uint64_t revision = 0;
    Registry::Capture capture;
    std::array<Registry::ReceiverSpec, Registry::kMaxSlots> receivers;
    std::size_t count = 0;
};
struct Bank {
    // Keep unused reservations too: injected preparation cannot bypass actual
    // shared-pool admission. Production consumes each into a WdspChannel.
    std::optional<WdspChannel::Reservation> reservation;
    std::array<std::unique_ptr<Registry::Receiver>, Registry::kMaxSlots> receivers;
};
struct BankSlot {
    std::atomic<Stage> stage {Stage::Free};
    Request request;
    std::unique_ptr<Bank> bank;
};
} // namespace

struct RtlReceiverRegistry::State {
    Limits limits;
    Prepare prepare;
    Capture capture;
    std::atomic<std::uint64_t> session {0};
    std::atomic<std::uint64_t> revision {0};
    std::atomic<bool> closing {false};
    std::atomic<bool> reader {false};
    std::atomic<int> offered {-1};
    std::array<Handle, kMaxSlots> reservations;
    std::array<bool, kMaxSlots> desiredSlots {};
    std::array<std::uint64_t, kMaxSlots> instances {};
    std::array<BankSlot, kBankCount> banks;
    Status status;
};

struct RtlReceiverRegistry::Executor : std::enable_shared_from_this<Executor> {
    std::mutex mutex;
    std::array<std::shared_ptr<State>, kMaxRegistries> states;
    struct Job { std::shared_ptr<State> state; Request request; };
    std::optional<Job> pending;
    std::uint64_t nextSession = 0;
    bool running = false;

    void kickLocked()
    {
        if (!running) {
            running = true;
            QThreadPool::globalInstance()->start([self = shared_from_this()] { self->run(); });
        }
    }
    std::size_t resident(const State& state) const
    {
        std::size_t count = 0;
        for (const BankSlot& bank : state.banks) {
            if (bank.stage.load(std::memory_order_acquire) != Stage::Free) {
                count += bank.request.count;
            }
        }
        return count;
    }
    bool referenced(const State& state, int slot) const
    {
        const auto contains = [slot](const Request& request) {
            return std::any_of(request.receivers.begin(),
                request.receivers.begin() + request.count,
                [slot](const ReceiverSpec& spec) { return spec.handle.slot == slot; });
        };
        for (const BankSlot& bank : state.banks) {
            if (bank.stage.load(std::memory_order_acquire) != Stage::Free && contains(bank.request)) {
                return true;
            }
        }
        return pending && pending->state.get() == &state && contains(pending->request);
    }
    void releaseUnusedLocked(State& state)
    {
        for (std::size_t slot = 0; slot < state.limits.slotCount; ++slot) {
            if (!state.desiredSlots[slot] && !referenced(state, static_cast<int>(slot))) {
                state.reservations[slot] = {};
            }
        }
    }
    void invalidateLocked(State& state)
    {
        state.session.store(0, std::memory_order_release);
        state.reservations.fill({});
        state.desiredSlots.fill(false);
        if (pending && pending->state.get() == &state) {
            pending.reset();
        }
        // With an attached reader, ONLY that reader may acknowledge banks.
        // stop() supplies the same acknowledgment if no next callback arrives.
        if (!state.reader.load(std::memory_order_acquire)) {
            const int offered = state.offered.exchange(-1, std::memory_order_acq_rel);
            if (offered >= 0) {
                state.banks[static_cast<std::size_t>(offered)].stage.store(Stage::Retired, std::memory_order_release);
            }
        }
        state.status = {};
    }

    void run()
    {
        for (;;) {
            std::shared_ptr<State> state;
            std::unique_ptr<Bank> retired;
            int index = -1;
            std::optional<Request> request;
            {
                const std::scoped_lock lock(mutex);
                // Destruction has priority over preparing the coalesced state.
                // Keep Destroying charged until its actual destructors finish.
                for (const std::shared_ptr<State>& candidate : states) {
                    if (!candidate) {
                        continue;
                    }
                    for (std::size_t i = 0; i < kBankCount; ++i) {
                        BankSlot& slot = candidate->banks[i];
                        if (slot.stage.load(std::memory_order_acquire) == Stage::Retired) {
                            state = candidate;
                            index = static_cast<int>(i);
                            slot.stage.store(Stage::Destroying, std::memory_order_relaxed);
                            retired = std::move(slot.bank);
                            break;
                        }
                    }
                    if (state) {
                        break;
                    }
                }
                if (!state && pending) {
                    State& candidate = *pending->state;
                    if (candidate.closing.load() || candidate.session.load() != pending->request.session ||
                        candidate.revision.load() != pending->request.revision) {
                        pending.reset();
                    } else if (candidate.offered.load(std::memory_order_acquire) < 0) {
                        if (resident(candidate) + pending->request.count > candidate.limits.residentReceiverCapacity) {
                            candidate.status.result = Result::ResourceLimit;
                            candidate.status.error = "Old and new receiver banks exceed the resident receiver limit";
                            pending.reset();
                        } else {
                            for (std::size_t i = 0; i < kBankCount; ++i) {
                                if (candidate.banks[i].stage.load(std::memory_order_acquire) == Stage::Free) {
                                    state = pending->state;
                                    index = static_cast<int>(i);
                                    request = pending->request;
                                    state->banks[i].request = *request;
                                    state->banks[i].stage.store(Stage::Preparing, std::memory_order_relaxed);
                                    pending.reset();
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!state) {
                    for (std::shared_ptr<State>& candidate : states) {
                        if (candidate && candidate->closing.load() && !candidate->reader.load() &&
                            std::all_of(candidate->banks.begin(), candidate->banks.end(),
                                [](const BankSlot& slot) { return slot.stage.load() == Stage::Free; })) {
                            candidate.reset();
                        }
                    }
                    running = false;
                    return;
                }
            }
            if (!request) {
                retired.reset(); // WDSP close/planning never holds our mutex.
                const std::scoped_lock lock(mutex);
                state->banks[static_cast<std::size_t>(index)].stage.store(Stage::Free, std::memory_order_release);
                releaseUnusedLocked(*state);
                continue;
            }

            std::unique_ptr<Bank> prepared = std::make_unique<Bank>();
            std::string error;
            Result result = Result::Accepted;
            if (request->count != 0) {
                prepared->reservation = WdspChannel::reserveChannels(request->count);
                if (!prepared->reservation) {
                    result = Result::ResourceLimit;
                    error = "The shared WDSP pool cannot reserve the complete receiver bank";
                }
            }
            try {
                for (std::size_t i = 0; i < request->count && result == Result::Accepted; ++i) {
                    if (state->session.load(std::memory_order_acquire) != request->session ||
                        state->revision.load(std::memory_order_acquire) != request->revision) {
                        result = Result::Closed;
                        break;
                    }
                    prepared->receivers[i] = state->prepare(request->receivers[i], *prepared->reservation, error);
                    if (!prepared->receivers[i]) {
                        result = Result::PreparationFailed;
                    }
                }
            } catch (const std::exception& exception) {
                result = Result::PreparationFailed;
                error = exception.what();
            } catch (...) {
                result = Result::PreparationFailed;
                error = "The preparation callable threw a non-standard exception";
            }
            {
                const std::scoped_lock lock(mutex);
                BankSlot& slot = state->banks[static_cast<std::size_t>(index)];
                const bool current = !state->closing.load() && state->session.load() == request->session &&
                    state->revision.load() == request->revision;
                if (current) {
                    state->status.result = result;
                    state->status.error = std::move(error);
                }
                if (current && result == Result::Accepted) {
                    slot.bank = std::move(prepared);
                    state->status.prepared = request->revision;
                    slot.stage.store(Stage::Offered, std::memory_order_release);
                    state->offered.store(index, std::memory_order_release);
                } else {
                    slot.stage.store(Stage::Destroying, std::memory_order_release);
                }
            }
            if (prepared) {
                prepared.reset();
                const std::scoped_lock lock(mutex);
                state->banks[static_cast<std::size_t>(index)].stage.store(Stage::Free, std::memory_order_release);
                releaseUnusedLocked(*state);
            }
        }
    }
};

std::shared_ptr<RtlReceiverRegistry::Executor> RtlReceiverRegistry::executor()
{
    static const std::shared_ptr<Executor> instance = std::make_shared<Executor>();
    return instance;
}

RtlReceiverRegistry::RtlReceiverRegistry(Limits limits, Prepare prepare)
{
    if (limits.slotCount == 0 || limits.slotCount > kMaxSlots || limits.receiverCapacity == 0 ||
        limits.receiverCapacity > limits.slotCount || limits.residentReceiverCapacity < limits.receiverCapacity ||
        limits.residentReceiverCapacity > 32) {
        return;
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    for (std::shared_ptr<State>& entry : controller->states) {
        if (!entry) {
            m_state = std::make_shared<State>();
            m_state->limits = limits;
            m_state->prepare = prepare ? std::move(prepare) : prepareWdsp;
            entry = m_state;
            break;
        }
    }
    controller->kickLocked();
}

RtlReceiverRegistry::~RtlReceiverRegistry()
{
    if (m_state) {
        const std::shared_ptr<Executor> controller = executor();
        const std::scoped_lock lock(controller->mutex);
        m_state->closing.store(true, std::memory_order_release);
        controller->invalidateLocked(*m_state);
        controller->kickLocked();
    }
}

std::uint64_t RtlReceiverRegistry::beginSession(const Capture& capture)
{
    if (!m_state || !validCapture(capture)) {
        return 0;
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    if (controller->nextSession == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }
    controller->invalidateLocked(*m_state);
    m_state->capture = capture;
    const std::uint64_t session = ++controller->nextSession;
    m_state->session.store(session, std::memory_order_release);
    m_state->status.result = Result::Accepted;
    controller->kickLocked();
    return session;
}

std::optional<RtlReceiverRegistry::Handle> RtlReceiverRegistry::reserveSlot(std::optional<int> preferred)
{
    if (!m_state) {
        return std::nullopt;
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    const std::uint64_t session = m_state->session.load();
    if (session == 0 || (preferred && (*preferred < 0 || static_cast<std::size_t>(*preferred) >= m_state->limits.slotCount))) {
        return std::nullopt;
    }
    controller->releaseUnusedLocked(*m_state);
    const std::size_t reserved = static_cast<std::size_t>(std::count_if(
        m_state->reservations.begin(), m_state->reservations.end(),
        [session](const Handle& handle) { return handle.session == session; }));
    if (reserved >= m_state->limits.receiverCapacity) {
        return std::nullopt;
    }
    for (std::size_t slot = 0; slot < m_state->limits.slotCount; ++slot) {
        if ((preferred && static_cast<int>(slot) != *preferred) || m_state->reservations[slot].session != 0 ||
            controller->referenced(*m_state, static_cast<int>(slot)) ||
            m_state->instances[slot] == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        const Handle handle {session, ++m_state->instances[slot], static_cast<int>(slot)};
        m_state->reservations[slot] = handle;
        m_state->desiredSlots[slot] = true;
        return handle;
    }
    return std::nullopt;
}

RtlReceiverRegistry::Result RtlReceiverRegistry::cancelReservation(Handle handle)
{
    if (!m_state) {
        return Result::Closed;
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    if (handle.slot < 0 || static_cast<std::size_t>(handle.slot) >= m_state->limits.slotCount ||
        m_state->reservations[static_cast<std::size_t>(handle.slot)] != handle) {
        return Result::Invalid;
    }
    if (controller->referenced(*m_state, handle.slot)) {
        return Result::Busy;
    }
    m_state->reservations[static_cast<std::size_t>(handle.slot)] = {};
    m_state->desiredSlots[static_cast<std::size_t>(handle.slot)] = false;
    return Result::Accepted;
}

RtlReceiverRegistry::Result RtlReceiverRegistry::submit(const Capture& capture, std::span<const ReceiverSpec> desired)
{
    if (!m_state) {
        return Result::Closed;
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    State& state = *m_state;
    if (state.session.load() == 0) {
        return Result::Closed;
    }
    if (!validCapture(capture) || capture.captureId != state.capture.captureId ||
        desired.size() > state.limits.receiverCapacity ||
        state.revision.load() == std::numeric_limits<std::uint64_t>::max()) {
        return Result::Invalid;
    }
    if (capture.generation < state.capture.generation) {
        const bool stillActive = std::any_of(state.banks.begin(), state.banks.end(),
            [&state, &capture](const BankSlot& bank) {
                return bank.stage.load(std::memory_order_acquire) == Stage::Active &&
                    bank.request.session == state.session.load() && bank.request.capture == capture;
            });
        if (!stillActive) {
            return Result::Invalid;
        }
    }
    if (capture.generation == state.capture.generation && capture != state.capture) {
        return Result::Invalid; // readback changes require their own generation
    }
    std::array<SharedCapturePolicy::SliceDescriptor, kMaxSlots> passbands;
    std::array<bool, kMaxSlots> used {};
    for (std::size_t i = 0; i < desired.size(); ++i) {
        const ReceiverSpec& spec = desired[i];
        const int slot = spec.handle.slot;
        if (slot < 0 || static_cast<std::size_t>(slot) >= state.limits.slotCount || used[slot] ||
            spec.handle.session != state.session.load() || state.reservations[slot] != spec.handle ||
            spec.passband.stableId != slot || !boundedDsp(spec.dsp)) {
            return Result::Invalid;
        }
        used[slot] = true;
        passbands[i] = spec.passband;
    }
    // The capture is fixed here: this singleton domain checks passband fit,
    // not tuner legality. M1 must validate the hardware domain/readback first.
    const SharedCapturePolicy::CenterDomain domain {capture.centerHz, capture.centerHz, capture.centerHz, 1};
    const SharedCapturePolicy::RestoreResult fitting = SharedCapturePolicy::restoreFixedCapture(capture,
        std::span(passbands).first(desired.size()), std::span(&domain, 1),
        {state.limits.slotCount, state.limits.receiverCapacity});
    if (fitting.error != SharedCapturePolicy::Error::None || !fitting.rejected.empty()) {
        return Result::Invalid;
    }
    if (controller->pending && controller->pending->state != m_state) {
        return Result::Busy;
    }
    Request request;
    request.session = state.session.load();
    request.revision = state.revision.load() + 1;
    request.capture = capture;
    request.count = desired.size();
    std::copy(desired.begin(), desired.end(), request.receivers.begin());
    // Omitted live handles remain valid until their last bank retires. If
    // preparation fails, the caller can still resubmit the complete old set.
    state.desiredSlots = used;
    if (capture.generation > state.capture.generation) {
        state.capture = capture;
    }
    state.revision.store(request.revision, std::memory_order_release);
    if (!state.reader.load(std::memory_order_acquire)) {
        const int obsolete = state.offered.exchange(-1, std::memory_order_acq_rel);
        if (obsolete >= 0) {
            state.banks[static_cast<std::size_t>(obsolete)].stage.store(Stage::Retired, std::memory_order_release);
        }
    }
    state.status.result = Result::Accepted;
    state.status.requested = request.revision;
    state.status.error.clear();
    controller->pending = Executor::Job {m_state, request};
    controller->releaseUnusedLocked(state);
    controller->kickLocked();
    return Result::Accepted;
}

void RtlReceiverRegistry::cancelSession()
{
    if (m_state) {
        const std::shared_ptr<Executor> controller = executor();
        const std::scoped_lock lock(controller->mutex);
        controller->invalidateLocked(*m_state);
        controller->kickLocked();
    }
}

RtlReceiverRegistry::SampleReader RtlReceiverRegistry::attachReader()
{
    if (!m_state) {
        return {};
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    if (m_state->reader.exchange(true)) {
        return {};
    }
    return SampleReader(m_state);
}

RtlReceiverRegistry::Status RtlReceiverRegistry::service()
{
    if (!m_state) {
        return {};
    }
    const std::shared_ptr<Executor> controller = executor();
    const std::scoped_lock lock(controller->mutex);
    controller->kickLocked();
    Status status = m_state->status;
    status.session = m_state->session.load(std::memory_order_acquire);
    status.preparing = false;
    for (const BankSlot& slot : m_state->banks) {
        const Stage stage = slot.stage.load(std::memory_order_acquire);
        status.preparing |= stage == Stage::Preparing;
        if (stage == Stage::Active && slot.request.session == status.session) {
            status.published = std::max(status.published, slot.request.revision);
        }
    }
    status.residentReceivers = controller->resident(*m_state);
    status.pending = controller->pending && controller->pending->state == m_state;
    return status;
}

RtlReceiverRegistry::SampleReader::SampleReader(std::shared_ptr<State> state)
    : m_state(std::move(state)) {}
RtlReceiverRegistry::SampleReader::SampleReader(SampleReader&& other) noexcept
    : m_state(std::move(other.m_state)), m_active(std::exchange(other.m_active, -1)),
      m_nextSample(other.m_nextSample), m_continuous(other.m_continuous) {}
RtlReceiverRegistry::SampleReader& RtlReceiverRegistry::SampleReader::operator=(SampleReader&& other) noexcept
{
    if (this != &other) {
        stop();
        m_state = std::move(other.m_state);
        m_active = std::exchange(other.m_active, -1);
        m_nextSample = other.m_nextSample;
        m_continuous = other.m_continuous;
    }
    return *this;
}
RtlReceiverRegistry::SampleReader::~SampleReader() { stop(); }

void RtlReceiverRegistry::SampleReader::stop()
{
    if (!m_state) {
        return;
    }
    const std::shared_ptr<Executor> controller = executor();
    {
        const std::scoped_lock lock(controller->mutex);
        if (m_active >= 0) {
            m_state->banks[static_cast<std::size_t>(m_active)].stage.store(Stage::Retired, std::memory_order_release);
            m_active = -1;
        }
        const int offered = m_state->offered.exchange(-1, std::memory_order_acq_rel);
        if (offered >= 0) {
            m_state->banks[static_cast<std::size_t>(offered)].stage.store(Stage::Retired, std::memory_order_release);
        }
        m_state->reader.store(false, std::memory_order_release);
        controller->invalidateLocked(*m_state);
        controller->kickLocked();
    }
    m_state.reset(); // off callback; Executor retains State until retirement
}

bool RtlReceiverRegistry::SampleReader::processBlock(const SampleBlock& block, BlockProcessor& processor) noexcept
{
    if (!m_state) {
        return false;
    }
    State& state = *m_state;
    const int offered = state.offered.load(std::memory_order_acquire);
    // Reload the session AFTER acquiring the offer: a new session may have
    // published between an earlier session read and this mailbox read.
    const std::uint64_t session = state.session.load(std::memory_order_acquire);
    if (m_active >= 0 && state.banks[static_cast<std::size_t>(m_active)].request.session != session) {
        state.banks[static_cast<std::size_t>(m_active)].stage.store(Stage::Retired, std::memory_order_release);
        m_active = -1;
        m_continuous = false;
    }
    const bool validBlock = block.session == session && session != 0 &&
        validCapture(block.capture) && !block.samples.empty() && block.samples.data() != nullptr &&
        block.samples.size() <= kMaxInputSamples &&
        block.firstSample <= std::numeric_limits<std::uint64_t>::max() - block.samples.size() &&
        std::all_of(block.samples.begin(), block.samples.end(), [](const std::complex<float>& sample) {
            return std::isfinite(sample.real()) && std::isfinite(sample.imag());
        });
    if (offered >= 0) {
        BankSlot& slot = state.banks[static_cast<std::size_t>(offered)];
        const bool stale = slot.request.session != session ||
            slot.request.revision != state.revision.load(std::memory_order_acquire);
        if (stale || (validBlock && slot.request.capture == block.capture)) {
            if (stale) {
                slot.stage.store(Stage::Retired, std::memory_order_release);
            } else {
                const int previous = m_active;
                slot.stage.store(Stage::Active, std::memory_order_release);
                m_active = offered;
                m_continuous = false;
                if (previous >= 0) {
                    state.banks[static_cast<std::size_t>(previous)].stage.store(Stage::Retired, std::memory_order_release);
                }
            }
            // Clear last, so preparation cannot mistake the not-yet-acknowledged
            // offer for resident capacity that a replacement must compete with.
            state.offered.store(-1, std::memory_order_release);
        }
    }
    if (session == 0 || m_active < 0 || !validBlock) {
        m_continuous = false;
        return false;
    }
    BankSlot& active = state.banks[static_cast<std::size_t>(m_active)];
    if (active.request.capture != block.capture) {
        m_continuous = false;
        return false;
    }
    std::array<ReceiverView, kMaxSlots> views;
    for (std::size_t i = 0; i < active.request.count; ++i) {
        views[i] = {&active.request.receivers[i], active.bank->receivers[i].get()};
    }
    SampleBlock delivery = block;
    delivery.discontinuity = block.discontinuity || !m_continuous || block.firstSample != m_nextSample;
    processor.process(delivery, std::span(views).first(active.request.count));
    m_nextSample = block.firstSample + block.samples.size();
    m_continuous = true;
    return true;
}

} // namespace AetherSDR::rtl
