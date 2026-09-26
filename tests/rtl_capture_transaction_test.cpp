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
    // Receiver adoption still waits for acknowledgment. Configured receivers
    // may park outside capture without being deleted or sent to the DSP bank.
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
    check(bool(owner.submit(d)), "out-of-window receiver remains configured");
    work = owner.takeWork();
    check(work && !work->hardwareChanged && work->target.receivingIds == std::vector<int>{0},
          "parked receiver is absent from receiving membership without USB retune");
    if (work) {
        check(owner.complete(T::execute(*work, device)) == T::Completion::Published,
              "parked receiver configuration publishes after acknowledgment");
        check(owner.confirmed()->receivers == d.receivers && device.writes == writes,
              "parking preserves both configured receivers and USB center");
    }

    // Membership uses the complete guarded interval and is recomputed on
    // each verified capture placement. IDs are sorted independently of the
    // saved receiver order.
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto configured = desired();
        configured.receivers.push_back({{7, 98'948'000, -8000, 8000, 0, 20'000, 20'000}, T::Mode::Usb});
        configured.receivers.push_back({{3, 101'052'000, -8000, 8000, 0, 20'000, 20'000}, T::Mode::Usb});
        check(bool(tx.submit(configured)), "receivers at both guarded capture edges admitted");
        auto job = tx.takeWork();
        check(job && job->target.receivingIds == (std::vector<int>{0, 3, 7}),
              "exact guarded edges receive in sorted ID order");
        if (job) { tx.complete(T::execute(*job, usb)); }

        configured.hardware.centerHz = 99'999'999;
        check(bool(tx.submit(configured)), "one hertz left pan admitted");
        job = tx.takeWork();
        check(job && job->target.receivingIds == (std::vector<int>{0, 7}),
              "right-edge receiver parks when one guarded hertz leaves capture");
        if (job) { tx.complete(T::execute(*job, usb)); }

        configured.hardware.centerHz = 100'000'001;
        check(bool(tx.submit(configured)), "one hertz right pan admitted");
        job = tx.takeWork();
        check(job && job->target.receivingIds == (std::vector<int>{0, 3}),
              "left-edge receiver parks when one guarded hertz leaves capture");
        if (job) { tx.complete(T::execute(*job, usb)); }

        configured.hardware.centerHz = 105'000'000;
        check(bool(tx.submit(configured)), "capture may pan beyond every configured receiver");
        job = tx.takeWork();
        check(job && job->target.receivingIds.empty() && job->target.receivers == configured.receivers,
              "empty live bank retains every configured receiver");
        if (job) {
            check(tx.complete(T::execute(*job, usb)) == T::Completion::Published,
                  "empty live bank publishes after center readback");
            check(tx.confirmed()->receivingIds.empty() && usb.hardware.centerHz == 105'000'000,
                  "free pan reaches requested capture center with all receivers parked");
        }

        configured.followReceiverId = 3;
        check(bool(tx.submit(configured)), "explicit slice tune can reacquire a parked receiver");
        job = tx.takeWork();
        check(job && job->target.receivingIds == std::vector<int>{3},
              "followed receiver alone determines new capture placement");
        if (job) {
            check(tx.complete(T::execute(*job, usb)) == T::Completion::Published,
                  "followed capture publishes after readback");
            check(tx.confirmed()->receivers == configured.receivers,
                  "following one receiver keeps parked siblings configured");
        }
        configured.followReceiverId.reset();
        configured.hardware.centerHz = 100'000'000;
        check(bool(tx.submit(configured)), "free pan back to parked receivers admitted");
        job = tx.takeWork();
        check(job && job->target.receivingIds == (std::vector<int>{0, 3, 7}),
              "returning capture resumes all fitting receivers without retuning them");
        if (job) { tx.complete(T::execute(*job, usb)); }
    }

    {
        T tx({8, 1}); Device usb; tx.beginSession();
        auto startup = desired(); startup.hardware.centerHz = 105'000'000;
        check(bool(tx.submit(startup)), "first capture follows its initial receiver");
        const auto job = tx.takeWork();
        check(job && job->target.receivingIds == std::vector<int>{0}
            && job->target.hardware.centerHz != 105'000'000,
              "saved offscreen center cannot start with an empty receiver bank");
        if (job) { tx.complete(T::execute(*job, usb)); }
    }

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

    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto configured = desired();
        configured.receivers.push_back({{1, 105'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm});
        tx.submit(configured); auto job = tx.takeWork(); tx.complete(T::execute(*job, usb));
        configured.hardware.centerHz = 105'000'000;
        check(bool(tx.submit(configured)), "parking retune admitted before injected USB failure");
        job = tx.takeWork();
        usb.failAt = usb.writes + 5; // Center setter fails after earlier controls mutate.
        if (job) {
            check(tx.complete(T::execute(*job, usb)) == T::Completion::Failed,
                  "failed parking retune restores prior capture");
            check(tx.confirmed()->hardware.centerHz == 100'000'000
                && tx.confirmed()->receivingIds == std::vector<int>{0}
                && tx.confirmed()->receivers == configured.receivers,
                  "rollback retains the previously receiving and parked identities");
            check(usb.hardware == desired().hardware, "failed parking retune restores USB readback");
        }
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
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto pan = desired(); pan.hardware.centerHz = 105'000'000;
        check(bool(tx.submit(pan)), "first free-pan center accepted");
        auto job = tx.takeWork();
        const auto stale = T::execute(*job, usb);
        for (std::uint32_t center : {106'000'000u, 107'000'000u, 108'000'000u}) {
            pan.hardware.centerHz = center;
            check(bool(tx.submit(pan)) && tx.pendingCount() == 1,
                  "rapid free-pan requests coalesce to one pending center");
        }
        check(tx.complete(stale) == T::Completion::Compensating,
              "superseded free-pan readback cannot publish");
        check(tx.confirmed()->hardware.centerHz == 100'000'000
            && tx.confirmed()->receivingIds == std::vector<int>{0},
              "superseded pan leaves published capture and membership intact");
        auto rollback = tx.takeWork();
        check(rollback && rollback->compensation, "free-pan rollback precedes latest center");
        if (rollback) { tx.complete(T::execute(*rollback, usb)); }
        auto latest = tx.takeWork();
        check(latest && latest->target.hardware.centerHz == 108'000'000
            && latest->target.receivingIds.empty(),
              "only newest free-pan center and its parked membership execute");
        if (latest) {
            check(tx.complete(T::execute(*latest, usb)) == T::Completion::Published,
                  "latest free-pan readback publishes");
        }
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
        next = desired(); next.receivers.push_back(
            {{3, 105'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm});
        next.receivers.back().passband.filterHighHz = -8000;
        check(!tx.submit(next) && !tx.takeWork(),
              "malformed parked passband refused before device work");
        next = desired(); next.receivers.push_back(
            {{3, 105'000'000, -8000, 50'000, 0, 3000, 3000}, T::Mode::Fm});
        check(!tx.submit(next) && !tx.takeWork(),
              "parked FM filter beyond the 48 kHz DSP passband is refused before work");
        next.receivers.back().passband.filterLowHz = 1000;
        next.receivers.back().passband.filterHighHz = 8000;
        check(!tx.submit(next) && !tx.takeWork(),
              "parked FM filter must still straddle its carrier");
        next = desired(); next.receivers.push_back(
            {{9, 105'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm});
        check(!tx.submit(next) && !tx.takeWork(),
              "out-of-range parked identity refused before device work");
        next = desired(); next.receivers.push_back(
            {{3, 1'767'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm});
        check(!tx.submit(next) && !tx.takeWork(),
              "out-of-domain parked carrier refused before device work");
        next = desired(); next.followReceiverId = 3;
        check(!tx.submit(next) && !tx.takeWork(),
              "follow intent must name a configured receiver");
        for (int level : {-1, 101}) {
            next = desired(); next.receivers[0].squelchLevel = level;
            check(!tx.submit(next) && !tx.takeWork(), "out-of-range squelch refused before work");
        }
        next = desired(); next.receivers[0].squelchEnabled = true;
        check(!tx.submit(next) && !tx.takeWork(), "legacy WFM cannot acknowledge an unimplemented squelch");
        next = desired(); tx.submit(next); auto job = tx.takeWork(); auto result = T::execute(*job, usb);
        result.actual->capture.generation++;
        check(tx.complete(result) == T::Completion::Invalidated, "mismatched capture generation never publishes");
    }
    {
        T tx({8, 1}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.receivers.push_back(
            {{3, 105'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm});
        check(!tx.submit(next) && !tx.takeWork(),
              "configured receiver count respects capacity even if one would park");
    }
    {
        T tx({8, 4}); Device usb; if (!establish(tx, usb)) { return 1; }
        auto next = desired(); next.receivers[0].audioGain = 75;
        tx.submit(next); auto job = tx.takeWork(); auto result = T::execute(*job, usb);
        result.actual->receivingIds.clear();
        check(tx.complete(result) == T::Completion::Invalidated,
              "worker readback cannot publish fabricated receiving membership");
        check(!tx.confirmed(), "unverifiable receiving membership withdraws capture");
    }
    // A new FM capture places converter DC away from the wanted RF carrier.
    // Removing capture displacement must fail without changing any DSP samples.
    for (const T::Mode mode : {T::Mode::Fm, T::Mode::Fmn}) {
        T tx({8, 4}); Device usb; tx.beginSession();
        auto next = desired();
        next.receivers = {{{3, 100'000'000, -8000, 8000, 0, 3000, 3000}, mode}};
        check(bool(tx.submit(next)), "initial narrow FM capture admitted");
        auto job = tx.takeWork();
        check(job && job->target.hardware.centerHz == 100'600'000,
              "initial FM capture prefers quarter-rate displacement");
        if (!job) { continue; }
        check(job->target.receivers == next.receivers,
              "DC avoidance preserves absolute RF, passband, and stable identity");
        check(job->target.hardware.offsetTuning == 0,
              "DC avoidance does not enable librtlsdr offset tuning");
        check(tx.complete(T::execute(*job, usb)) == T::Completion::Published,
              "displaced capture waits for verified readback");
        const int before = usb.writes;
        next.hardware = tx.confirmed()->hardware;
        next.receivers[0].passband.carrierHz = 100'600'000;
        check(bool(tx.submit(next)), "ordinary in-window move remains available near DC");
        job = tx.takeWork();
        check(job && !job->hardwareChanged,
              "ordinary in-window move never secretly relocates capture");
        if (job) {
            tx.complete(T::execute(*job, usb));
            check(usb.writes == before, "in-window move preserves USB configuration");
        }
    }
    {
        T tx({8, 4}); Device usb; tx.beginSession();
        auto next = desired(); next.hardware.sampleRateHz = 225'001;
        check(bool(tx.submit(next)), "small legacy capture admitted");
        auto job = tx.takeWork(); tx.complete(T::execute(*job, usb));
        next.receivers = {
            {{0, 99'960'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm},
            {{3, 100'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fmn},
            {{7, 100'040'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        // DC displacement follows the selected FM receiver. Siblings may
        // remain configured even when their own DC exclusion is not clear.
        check(bool(tx.submit(next)), "complete restored receiver set fits existing capture");
        job = tx.takeWork(); tx.complete(T::execute(*job, usb));
        const auto before = *tx.confirmed(); const int writesBefore = usb.writes;
        check(!T::dcClear(before), "DC overlap is reported for crowded receiver set");
        next.followReceiverId = 0;
        next.avoidDc = true;
        check(bool(tx.submit(next)), "followed FM receiver admits DC-clear relocation");
        job = tx.takeWork();
        check(job && job->target.hardware.centerHz != before.hardware.centerHz,
              "selected FM receiver moves converter DC");
        if (job) {
            check(job->target.receivers == before.receivers,
                  "FM placement preserves every configured sibling passband");
            check(job->target.hardware.centerHz >= 100'008'000,
                  "followed FM carrier is outside its DC exclusion");
            check(tx.complete(T::execute(*job, usb)) == T::Completion::Published,
                  "followed FM placement waits for verified readback");
            check(usb.writes > writesBefore, "FM relocation changes USB capture");
        }
    }
    for (const double carrier : {24'000.0, 23'990'000.0, 1'765'950'000.0}) {
        T tx({8, 1}); Device usb; tx.beginSession();
        auto next = desired(); next.hardware.centerHz = static_cast<std::uint32_t>(carrier);
        next.receivers = {{{3, carrier, -8000, 8000, 0, 3000, 3000}, T::Mode::Fmn}};
        check(bool(tx.submit(next)), "DC placement finds a legal band-edge alternative");
        auto job = tx.takeWork();
        check(job && T::dcClear(job->target), "band-edge alternative is DC-clear");
        if (!job) { continue; }
        check(job->target.hardware.centerHz >= 24'000 && job->target.hardware.centerHz <= 1'766'000'000,
              "band-edge alternative remains within hardware tuning limits");
        check((job->target.hardware.directSampling != 0) == (carrier < 24'000'000),
              "DC placement does not switch automatic front-end mode across 24 MHz");
        check(job->target.receivers == next.receivers, "band-edge placement never rewrites RF or filters");
    }
    std::fprintf(stderr, "rtl_capture_transaction_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
