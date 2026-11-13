#include "audio/pipe.hpp"

#include "audio/thread.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "generated/config.hpp"

namespace data2g::audio {

namespace {

using Clock = std::chrono::steady_clock;
const auto PERIOD_T = std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(static_cast<double>(PipeIo::PERIOD) / config::FS));

bool is_fifo(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_fifo(path, ec);
}

// Open a named pipe's other end for a moment, so a thread waiting in
// fopen() on it returns (then it sees stop_).
void release(const std::string& path, bool reader_waiting) {
#ifndef _WIN32
    if (!is_fifo(path)) return;
    const int fd = ::open(path.c_str(), (reader_waiting ? O_WRONLY : O_RDONLY) | O_NONBLOCK);
    if (fd >= 0) ::close(fd);
#else
    (void)path;
    (void)reader_waiting;
#endif
}

// Sleep to the next period's deadline; a deadline long past (a stall)
// restarts the schedule rather than bursting to catch up.
void pace(Clock::time_point& t) {
    t += PERIOD_T;
    const auto now = Clock::now();
    if (now - t > std::chrono::milliseconds(500)) t = now;
    std::this_thread::sleep_until(t);
}

}  // namespace

PipeIo::PipeIo(std::string in, std::string out, CaptureFifo& capture, PlaybackFifo& playback, Report report)
    : in_(std::move(in)), out_(std::move(out)), capture_(capture), playback_(playback), report_(std::move(report)) {
    playback_.set_output_latency(std::chrono::duration<double>(PERIOD_T).count());  // one period sits in the write
    reader_ = std::thread(&PipeIo::read_loop, this);
    writer_ = std::thread(&PipeIo::write_loop, this);
}

PipeIo::~PipeIo() { stop(); }

void PipeIo::stop() {
    if (stop_.exchange(true)) return;
    if (!in_opened_) release(in_, true);
    if (!out_opened_) release(out_, false);
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
}

void PipeIo::read_loop() {
    thread_init("capture");
    bool paced = !is_fifo(in_);
    std::FILE* f = std::fopen(in_.c_str(), "rb");
    in_opened_ = true;
    if (!f) {
        if (report_ && !stop_) report_("audio input " + in_ + ": can't open; feeding silence");
        paced = true;
    } else {
        std::setvbuf(f, nullptr, _IONBF, 0);  // no read-ahead: it would wait for a buffer's worth
    }
    std::vector<float> buf(PERIOD);
    std::vector<double> x(PERIOD);
    auto t = Clock::now();
    while (!stop_) {
        std::size_t n = f ? std::fread(buf.data(), sizeof(float), PERIOD, f) : 0;
        if (f && n < PERIOD) {
            if (report_ && !stop_) report_("audio input " + in_ + ": ended; feeding silence");
            std::fclose(f);
            f = nullptr;
            paced = true;
        }
        if (!f) {
            for (std::size_t i = n; i < PERIOD; ++i) buf[i] = 0.0f;
            n = PERIOD;
        }
        for (std::size_t i = 0; i < n; ++i) x[i] = buf[i];
        capture_.write({x.data(), n});
        if (paced) pace(t);
    }
    if (f) std::fclose(f);
}

void PipeIo::write_loop() {
    thread_init("playback");
    std::FILE* f = std::fopen(out_.c_str(), "wb");
    out_opened_ = true;
    if (!f) {
        if (report_ && !stop_) report_("audio output " + out_ + ": can't open; discarding");
    } else {
        std::setvbuf(f, nullptr, _IONBF, 0);
    }
    std::vector<float> buf(PERIOD);
    auto t = Clock::now();
    while (!stop_) {
        playback_.pull(buf);  // drains the FIFO even when nothing is written, so drain() returns
        if (f && std::fwrite(buf.data(), sizeof(float), PERIOD, f) < PERIOD) {
            if (report_ && !stop_) report_("audio output " + out_ + ": closed by the reader; discarding");
            std::fclose(f);
            f = nullptr;
        }
        pace(t);
    }
    if (f) std::fclose(f);
}

}  // namespace data2g::audio
