// A sound card made of two files: raw float32 mono at FS, in and out, each
// moved on its own thread at real time. With two named pipes (mkfifo) two
// hosts are cross-connected with no sound card (data2g-host --audio-io
// pipe:IN,OUT; the end-to-end test does this). Beside the fake device in
// tests/test_audio.cpp, not in the Qt backend: no Qt, no device.
//
// Output: every PERIOD samples by the clock, the PlaybackFifo is pulled
// (silence when nothing is keyed) and written, so the reader sees a
// continuous real-time stream. Input: a named pipe is read as it arrives
// (the writer's clock paces it, and reading it at once keeps the pipe from
// holding seconds of latency); a regular file is read at real time; after
// its end, or if it can't be opened, silence is fed at real time.
//
// A process writing to a pipe whose reader has gone gets SIGPIPE: the app
// ignores it, and the output then carries on discarding.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

#include "audio/fifo.hpp"

namespace data2g::audio {

class PipeIo {
public:
    using Report = std::function<void(const std::string&)>;  // from the I/O threads
    static constexpr std::size_t PERIOD = 256;                // samples at FS: 32 ms

    // `in`: written to `capture` (rate FS); `out`: pulled from `playback`
    // (rate FS). Opening a named pipe waits for its other end, on the
    // stream's own thread.
    PipeIo(std::string in, std::string out, CaptureFifo& capture, PlaybackFifo& playback, Report report = {});
    ~PipeIo();  // stop()s
    PipeIo(const PipeIo&) = delete;
    PipeIo& operator=(const PipeIo&) = delete;

    // Joins both threads. A thread still waiting to open a named pipe is
    // released by opening the other end ourselves.
    void stop();

private:
    void read_loop();
    void write_loop();

    std::string in_, out_;
    CaptureFifo& capture_;
    PlaybackFifo& playback_;
    Report report_;
    std::atomic<bool> stop_{false}, in_opened_{false}, out_opened_{false};
    std::thread reader_, writer_;
};

}  // namespace data2g::audio
