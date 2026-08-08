#pragma once

// Waiting out the connect's asynchronous WDSP channel opens, for the HL2 tests.
//
// Hl2Backend::connectRadio() hands the opens to the I/O thread and returns
// (beginDspSetup), so the wire does not start until they finish. Every one of
// these tests runs on an isolated HOME — TestSettingsProfile re-roots it, and
// WdspChannel's wisdomPath() falls back to $HOME/.cache — which means an empty
// FFTW wisdom cache and a first open that genuinely spends tens of seconds
// measuring plans. That cost belongs to neither the radio nor anything under
// test, so waiting a fixed number of milliseconds for it would be measuring the
// host's CPU. The timing assertions AFTER a connect still use each test's own
// spin(), because those ARE the assertion.
//
// Shared rather than pasted into each test because the number below is a
// judgement about the slowest machine we expect to run on, and five copies of a
// judgement drift.

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace AetherSDR::test {

// A BACKSTOP, not a budget: reaching it means something is wrong, not that the
// machine was slow. Sized from measurement rather than taste — one cold
// hl2_backend_test spent ~70 s of its 88 s wall time on plan measurement alone,
// on a 32-core box under load ~26, and `ctest -j8` puts several cold-cache tests
// through the planner at once. 120 s left too little between "slow host" and
// "hung".
constexpr int kDspBuildTimeoutMs = 240000;

inline bool spinUntil(const std::function<bool()>& done,
                      int timeoutMs = kDspBuildTimeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return done();
}

// Wait for the connect to get through its DSP build, and SAY SO if it does not.
// Without this the timeout is silent: the test carries on and fails on whatever
// assertion comes next, which then reads as a defect in the wire or in the
// publish cadence rather than as a build that never finished.
inline bool awaitDspBuild(const char* testName, const std::function<bool()>& done)
{
    if (spinUntil(done))
        return true;
    std::fprintf(stderr,
                 "%s: the connect's DSP build did not finish within %d ms — the "
                 "checks below fail for that reason, not the one they name\n",
                 testName, kDspBuildTimeoutMs);
    return false;
}

} // namespace AetherSDR::test
