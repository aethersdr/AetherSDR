// Tearing down a TX channel must wait for WDSP's PureSignal correction thread
// (doPSCorrChange) before freeing what it uses (#6179, WDSP patch 22).
//
// The thread is held in PSRestoreCorr()'s fopen() on a FIFO that has no writer
// yet, which no fixed timeout can outlast. The channel is destroyed on another
// thread; the writer opens 1 s later. CloseChannel() must still be waiting
// then, and return once the thread is released. Upstream gave up after 500 ms
// and freed the CALCC under the blocked thread.

#include "core/dsp/WdspChannel.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool value, const char* message)
{
    std::cout << (value ? "[ OK ] " : "[FAIL] ") << message << '\n';
    if (!value) {
        ++failures;
    }
}

}   // namespace

int main()
{
#ifdef _WIN32
    std::cout << "[SKIP] needs a POSIX FIFO\n";
    return 77;
#else
    WdspChannel::Config tx;
    tx.direction = WdspChannel::Direction::Transmit;
    tx.inputSampleRate = 48000;
    tx.dspSampleRate = 48000;
    tx.outputSampleRate = 48000;
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(tx, &error);
    check(channel != nullptr, "a transmit channel opens");
    if (!channel) {
        std::cerr << error << '\n';
        return 1;
    }

    std::string fifo = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
        + "/wdsp-calcc-teardown-" + std::to_string(::getpid());
    ::unlink(fifo.c_str());
    check(::mkfifo(fifo.c_str(), 0600) == 0, "premise: the FIFO is created");

    // The correction thread now blocks in fopen(fifo, "r") until a writer opens.
    channel->restorePureSignalCorrectionForTest(fifo.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::atomic<bool> closed{false};
    std::thread closer([&] {
        channel.reset();
        closed.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    check(!closed.load(),
          "teardown is still waiting 1 s later, past upstream's 500 ms limit");

    // Release the thread: a line that is no correction file, so the restore
    // fails and it moves on. Held open briefly so the reader sees a writer.
    // Non-blocking, so a thread that never opened the FIFO fails here (ENXIO)
    // instead of hanging until the ctest timeout.
    const int writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
    if (writer < 0 && errno == ENXIO) {
        check(false, "premise: the correction thread has the FIFO open for reading");
    }
    check(writer >= 0, "premise: the writer end opens");
    if (writer >= 0) {
        const char junk[] = "not-a-correction-file x\n";
        check(::write(writer, junk, sizeof(junk) - 1) > 0, "premise: the junk line is written");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::close(writer);
    }
    closer.join();
    check(closed.load(), "teardown completes once the correction thread is released");
    ::unlink(fifo.c_str());

    std::cout << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
#endif
}
