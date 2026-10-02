// Socket-free CI-V stall policy: the IC-9700 targeted 0x04 data-pipe restart
// ladder, and the warn-only behaviour every other model keeps. The serial
// stream is a never-bound IcomStream whose test writer captures the restart.
// The stall threshold is measured on the backend's own monotonic clock, so the
// test waits once for that clock to pass it; every later step back-dates state.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "core/backends/icom/IcomStream.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include <cstdio>
#include <memory>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {

struct IcomStreamTestAccess {
    static void attach(IcomStream& stream, std::vector<std::vector<std::uint8_t>>& sink)
    {
        stream.m_testWriter = [&sink](std::span<const std::uint8_t> packet) {
            sink.emplace_back(packet.begin(), packet.end());
        };
        stream.m_ready = true;
    }
};

struct IcomCivBackendTestAccess {
    static qint64 stallThresholdMs() { return IcomCivBackend::kCivStallMs; }
    static void prepare(IcomCivBackend& b, const char* modelName,
                        std::vector<std::vector<std::uint8_t>>& serialSink)
    {
        b.m_model = modelForName(modelName);
        b.m_session = std::make_unique<IcomSession>();
        b.m_session->setCivAddress(b.m_model->civAddress);
        b.m_session->m_serial = new IcomStream(b.m_session.get());
        IcomStreamTestAccess::attach(*b.m_session->m_serial, serialSink);
        b.m_civReported = b.m_model->civAddress;
        b.m_civModelId = b.m_model->civAddress;
        b.m_connected = true;
        b.m_sessionGeneration = 1;
    }
    static qint64 now(const IcomCivBackend& b) { return b.nowMs(); }
    static void lastInboundAgo(IcomCivBackend& b, qint64 ageMs)
    {
        b.m_lastInboundCivAtMs = b.nowMs() - ageMs;
    }
    static void tick(IcomCivBackend& b) { b.onLinkTick(); }
    static bool recovering(const IcomCivBackend& b) { return b.m_civRecoveryStartedAtMs > 0; }
    static int attempts(const IcomCivBackend& b) { return b.m_civRecoveryAttempts; }
    static void retryDue(IcomCivBackend& b)
    {
        b.m_lastCivRecoveryAttemptAtMs -= profileFor(*b.m_model).civRecovery->retryIntervalMs + 1;
    }
    static void frequencyFrom(IcomCivBackend& b, std::uint8_t from)
    {
        CivFrame f;
        f.to = kControllerAddress;
        f.from = from;
        f.cmd = cmd::kReadFreq;
        f.data = {0x00, 0x00, 0x10, 0x44, 0x01};
        b.onCivFrame(f, 1);
    }
    static void queueRead(IcomCivBackend& b)
    {
        const std::vector<std::uint8_t> frame = cmdReadFrequency(b.m_model->civAddress);
        b.queueRead(frame, "stall.fixture", IcomCivScheduler::Priority::Maintenance,
                    b.nowMs() + 60'000);
    }
    static std::size_t queued(const IcomCivBackend& b) { return b.m_civScheduler.m_queue.size(); }
    static std::uint64_t failedRequests(const IcomCivBackend& b)
    {
        return b.m_schedulerFailedRequests;
    }
};

}  // namespace AetherSDR::icom

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using Access = IcomCivBackendTestAccess;
using Packets = std::vector<std::vector<std::uint8_t>>;

std::size_t restarts(const Packets& sent)
{
    std::size_t n = 0;
    for (const auto& p : sent) {
        n += p.size() > 0x15 && p[0x10] == 0xc0 && p[0x15] == 0x04;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Packets ic9700Serial;
    Packets ic705Serial;
    Packets exhaustSerial;
    IcomCivBackend ic9700;
    IcomCivBackend ic705;
    IcomCivBackend exhaust;
    Access::prepare(ic9700, "IC-9700", ic9700Serial);
    Access::prepare(ic705, "IC-705", ic705Serial);
    Access::prepare(exhaust, "IC-9700", exhaustSerial);

    const qint64 stallMs = Access::stallThresholdMs();
    while (Access::now(ic9700) <= stallMs + 100 || Access::now(ic705) <= stallMs + 100
           || Access::now(exhaust) <= stallMs + 100) {
        QThread::msleep(50);
    }

    // ---- IC-9700: restart only after a real stall, verified by this radio ----
    Access::lastInboundAgo(ic9700, stallMs - 1000);
    Access::tick(ic9700);
    check(!Access::recovering(ic9700) && restarts(ic9700Serial) == 0,
          "IC-9700: four seconds of silence is not a stall");

    Access::queueRead(ic9700);
    Access::lastInboundAgo(ic9700, stallMs + 1);
    Access::tick(ic9700);
    check(Access::recovering(ic9700) && Access::attempts(ic9700) == 1
              && restarts(ic9700Serial) == 1,
          "IC-9700: a real stall sends one targeted 0x04 data restart");
    check(Access::failedRequests(ic9700) >= 1,
          "IC-9700: the stall explicitly fails queued scheduler work");

    Access::frequencyFrom(ic9700, 0x94);
    check(Access::recovering(ic9700),
          "IC-9700: another bus device's frequency frame cannot verify the restart");
    Access::frequencyFrom(ic9700, 0xA2);
    check(!Access::recovering(ic9700),
          "IC-9700: the selected radio's frequency frame verifies the restart");

    // ---- IC-9700: bounded retries, then session replacement ----------------
    QSignalSpy lost(&exhaust, &IRadioBackend::connectionError);
    Access::lastInboundAgo(exhaust, stallMs + 1);
    Access::tick(exhaust);
    check(Access::attempts(exhaust) == 1, "IC-9700 exhaustion: attempt 1 on the stall");
    Access::tick(exhaust);
    check(Access::attempts(exhaust) == 1 && restarts(exhaustSerial) == 1,
          "IC-9700 exhaustion: no retry before the one-second cadence");
    for (int attempt = 2; attempt <= 3; ++attempt) {
        Access::retryDue(exhaust);
        Access::tick(exhaust);
    }
    check(Access::attempts(exhaust) == 3 && restarts(exhaustSerial) == 3 && lost.isEmpty(),
          "IC-9700 exhaustion: exactly three targeted restarts at the retry cadence");
    Access::retryDue(exhaust);
    Access::tick(exhaust);
    check(lost.count() == 1 && restarts(exhaustSerial) == 3 && !exhaust.isConnected(),
          "IC-9700 exhaustion: the fourth deadline replaces the session instead");

    // ---- every other model stays warn-only ---------------------------------
    Access::queueRead(ic705);
    Access::lastInboundAgo(ic705, stallMs + 1);
    Access::tick(ic705);
    check(!Access::recovering(ic705) && restarts(ic705Serial) == 0,
          "IC-705: a stall sends no 0x04 data restart and starts no recovery");
    check(Access::queued(ic705) >= 1 && Access::failedRequests(ic705) == 0
              && ic705.isConnected(),
          "IC-705: the stall neither resets the scheduler nor drops the session");

    if (g_failures == 0) {
        std::printf("icom_civ_stall_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
