// Socket-free, injected USB operations. Tests the production transaction owner;
// successful calls are intentionally allowed to mutate before a later failure.
#include "core/backends/rtl/RtlCaptureTransaction.h"
#include <cstdio>
#include <functional>
#include <limits>

using T = AetherSDR::rtl::RtlCaptureTransaction;
static int failures = 0;
static int checks = 0;
static void check(bool value, const char* message)
{
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Device : T::DeviceOperations {
    T::Hardware hardware;
    int writes = 0;
    int failAt = -1;
    bool failAll = false;
    bool corruptRead = false;
    std::function<void()> reading;
    bool set(T::Control c, std::int64_t v) override
    {
        ++writes;
        if (failAll || writes == failAt) { return false; }
        switch (c) {
        case T::Control::DirectSampling: hardware.directSampling = static_cast<int>(v); break;
        case T::Control::SampleRate: hardware.sampleRateHz = static_cast<std::uint32_t>(v); break;
        case T::Control::Ppm: hardware.ppm = static_cast<int>(v); break;
        case T::Control::OffsetTuning: hardware.offsetTuning = static_cast<int>(v); break;
        case T::Control::Center: hardware.centerHz = static_cast<std::uint32_t>(v); break;
        case T::Control::Gain: hardware.gainTenths = static_cast<int>(v); break;
        }
        return true;
    }
    std::optional<T::Hardware> read() override
    {
        const auto callback = reading;
        if (callback) { callback(); }
        T::Hardware result = hardware;
        if (corruptRead) { result.sampleRateHz = 0; }
        return result;
    }
};
static T::Desired desired()
{
    T::Desired d;
    d.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
    d.receivers = {{{0, 100'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
    return d;
}
static bool establish(T& owner, Device& device)
{
    owner.beginSession();
    check(bool(owner.submit(desired())), "initial complete set admitted");
    const auto work = owner.takeWork();
    check(bool(work), "initial capture prepares work");
    if (!work) { return false; }
    check(!owner.confirmed(), "prepare does not publish");
    const auto result = T::execute(*work, device);
    check(!owner.confirmed(), "USB completion does not publish before acknowledgment");
    check(owner.complete(result) == T::Completion::Published, "matching readback publishes");
    return owner.confirmed().has_value();
}
int main()
{
    // Removing whole-set admission or publishing requested state breaks these.
    T owner({8, 4}); Device device;
    if (!establish(owner, device)) { return 1; }
    int writes = device.writes;
    auto d = desired();
    d.receivers[0].passband.carrierHz = 100'200'000;
    check(bool(owner.submit(d)), "in-window move admitted");
    auto work = owner.takeWork();
    check(work && !work->hardwareChanged, "in-window move does not quiesce USB");
    if (work) {
        auto result = T::execute(*work, device);
        check(device.writes == writes, "in-window move performs no USB writes");
        check(owner.confirmed()->receivers[0].passband.carrierHz == 100'000'000,
              "receiver move remains unpublished before acknowledgment");
        check(owner.complete(result) == T::Completion::Published, "receiver change published after adoption");
    }
    d.receivers.push_back({{1, 105'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm});
    check(!owner.submit(d), "impossible full set refused");
    check(!owner.takeWork() && device.writes == writes, "refusal has no device work");

    // Each partial failure must restore ALL fields, not only the last setter.
    for (int position = 1; position <= 6; ++position) {
        T tx({8, 4}); Device usb;
        if (!establish(tx, usb)) { continue; }
        auto change = desired();
        change.hardware = {14'100'000, 2'000'000, 2, 0, 17, 280};
        change.receivers[0].passband.carrierHz = 14'100'000;
        check(bool(tx.submit(change)), "hardware change admitted");
        auto job = tx.takeWork();
        usb.failAt = usb.writes + position;
        const auto result = T::execute(*job, usb);
        check(result.code == T::ResultCode::Restored, "partial failure restored");
        check(usb.hardware == desired().hardware, "complete previous hardware restored");
        check(tx.complete(result) == T::Completion::Failed, "failed request never published");
        check(tx.confirmed()->hardware.centerHz == 100'000'000, "accepted center survives refusal");
    }

    // Invalid readback and a failed rollback must withdraw valid capture.
    for (bool badRead : {false, true}) {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { continue; }
        auto change = desired(); change.hardware.ppm = 10;
        tx.submit(change); auto job = tx.takeWork();
        usb.failAll = !badRead; usb.corruptRead = badRead;
        check(tx.complete(T::execute(*job, usb)) == T::Completion::Invalidated,
              "unverifiable rollback invalidates capture");
        check(!tx.confirmed(), "failed rollback withdraws confirmed state");
    }

    // Completion delayed past replacement must compensate, never publish old intent.
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto change = desired(); change.hardware.ppm = 10;
        tx.submit(change); auto job = tx.takeWork();
        auto result = T::execute(*job, usb);
        for (int i = 1; i <= 1000; ++i) {
            change.hardware.ppm = i;
            check(bool(tx.submit(change)) && tx.pendingCount() == 1, "pending requests coalesce");
        }
        check(tx.complete(result) == T::Completion::Compensating, "late success requires compensation");
        check(tx.confirmed()->hardware.ppm == 0, "superseded completion not published");
        auto rollback = tx.takeWork();
        check(rollback && rollback->compensation, "rollback precedes newest request");
        check(tx.complete(T::execute(*rollback, usb)) == T::Completion::Ignored,
              "compensation does not publish a request");
        check(usb.hardware.ppm == 0, "late result compensated physically");
        auto newest = tx.takeWork();
        check(newest && newest->target.hardware.ppm == 1000, "only newest pending request executes");
        check(tx.complete(T::execute(*newest, usb)) == T::Completion::Published,
              "latest result publishes");
        tx.endSession();
        check(!tx.submit(desired()), "closed session cannot enqueue device work");
        tx.beginSession();
        check(tx.complete(result) == T::Completion::Ignored && !tx.confirmed(),
              "old-session completion ignored");
    }
    // Replacement before preparation, during readback, and during receiver
    // adoption. Every result still has to pass the production completion gate.
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.hardware.ppm = 1;
        tx.submit(next); next.hardware.ppm = 2; tx.submit(next);
        auto job = tx.takeWork();
        check(job && job->target.hardware.ppm == 2, "replacement before preparation coalesces");
        usb.reading = [&] { next.hardware.ppm = 3; tx.submit(next); usb.reading = {}; };
        const auto stale = T::execute(*job, usb);
        check(tx.complete(stale) == T::Completion::Compensating, "replacement during readback fenced");
        auto rollback = tx.takeWork();
        check(tx.complete(stale) == T::Completion::Ignored,
              "duplicate forward completion cannot complete compensation");
        check(tx.complete(T::execute(*rollback, usb)) == T::Completion::Ignored,
              "matching compensation completes");
        auto latest = tx.takeWork();
        check(latest && latest->target.hardware.ppm == 3, "replacement survives compensation");
    }
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.receivers[0].passband.carrierHz = 100'100'000;
        tx.submit(next); auto job = tx.takeWork();
        auto result = T::execute(*job, usb);
        next.receivers[0].passband.carrierHz = 100'200'000; tx.submit(next);
        const int before = usb.writes;
        check(tx.complete(result) == T::Completion::Compensating, "superseded receiver adoption fenced");
        auto rollback = tx.takeWork();
        check(rollback && !rollback->hardwareChanged, "receiver compensation does not stop USB");
        tx.complete(T::execute(*rollback, usb));
        auto latest = tx.takeWork(); tx.complete(T::execute(*latest, usb));
        check(usb.writes == before, "receiver replacement and compensation perform no USB writes");
        check(tx.confirmed()->receivers[0].passband.carrierHz == 100'200'000,
              "only current receiver configuration published");
    }
    // An incorrect but nonzero readback is not permission to publish cached intent.
    for (int field = 0; field < 6; ++field) {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.hardware.ppm = 3; tx.submit(next);
        auto job = tx.takeWork();
        usb.reading = [&] {
            switch (field) {
            case 0: usb.hardware.centerHz += 1; break;
            case 1: usb.hardware.sampleRateHz = 2'000'000; break;
            case 2: usb.hardware.directSampling = 2; break;
            case 3: usb.hardware.offsetTuning = 1; break;
            case 4: usb.hardware.ppm = 4; break;
            case 5: usb.hardware.gainTenths = 280; break;
            }
            usb.reading = {};
        };
        check(tx.complete(T::execute(*job, usb)) == T::Completion::Failed,
              "mismatched readback rolls back instead of publishing");
        check(usb.hardware == desired().hardware, "mismatched readback restores full prior configuration");
    }
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.receivers[0].passband.carrierHz = std::numeric_limits<double>::quiet_NaN();
        check(!tx.submit(next) && !tx.takeWork(), "nonfinite request refused before device work");
        next = desired(); next.receivers.push_back(next.receivers.front());
        check(!tx.submit(next), "duplicate stable receiver identity refused");
        next = desired(); tx.submit(next); auto job = tx.takeWork(); auto result = T::execute(*job, usb);
        result.actual->capture.generation++;
        check(tx.complete(result) == T::Completion::Invalidated, "mismatched capture generation never publishes");
    }
    std::fprintf(stderr, "rtl_capture_transaction_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
