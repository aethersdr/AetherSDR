#include "audio/audio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace data2g::audio {

namespace {

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

double scale(SampleFormat f) {
    switch (f) {
        case SampleFormat::Float: return 1.0;
        case SampleFormat::Int16: return 32768.0;
        case SampleFormat::Int32: return 2147483648.0;
        case SampleFormat::UInt8: return 128.0;
    }
    return 1.0;
}

double read_sample(const std::byte* p, SampleFormat f) {
    switch (f) {
        case SampleFormat::Float: {
            float v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        case SampleFormat::Int16: {
            std::int16_t v;
            std::memcpy(&v, p, sizeof v);
            return v / 32768.0;
        }
        case SampleFormat::Int32: {
            std::int32_t v;
            std::memcpy(&v, p, sizeof v);
            return v / 2147483648.0;
        }
        case SampleFormat::UInt8: return (std::to_integer<int>(*p) - 128) / 128.0;
    }
    return 0.0;
}

void write_sample(std::byte* p, SampleFormat f, float x) {
    if (f == SampleFormat::Float) {
        const float v = std::clamp(x, -1.0f, 1.0f);
        std::memcpy(p, &v, sizeof v);
        return;
    }
    const double s = scale(f);
    const double v = std::clamp(std::nearbyint(x * (s - 1.0)), -s, s - 1.0);
    switch (f) {
        case SampleFormat::Int16: {
            const auto i = static_cast<std::int16_t>(v);
            std::memcpy(p, &i, sizeof i);
            return;
        }
        case SampleFormat::Int32: {
            const auto i = static_cast<std::int32_t>(v);
            std::memcpy(p, &i, sizeof i);
            return;
        }
        case SampleFormat::UInt8: *p = static_cast<std::byte>(static_cast<int>(v) + 128); return;
        case SampleFormat::Float: return;
    }
}

}  // namespace

void print_devices(std::span<const DeviceInfo> devices) {
    for (std::size_t i = 0; i < devices.size(); ++i) std::printf("%3zu  %2d ch  %s\n", i, devices[i].channels, devices[i].name.c_str());
}

std::optional<std::size_t> select_device(std::span<const DeviceInfo> devices, std::string_view want,
                                         std::string_view kind) {
    if (want.empty()) return std::nullopt;
    if (std::all_of(want.begin(), want.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const std::size_t i = std::stoul(std::string(want));
        if (i >= devices.size()) throw std::invalid_argument("no " + std::string(kind) + " device " + std::string(want));
        return i;
    }
    const std::string w = lowered(want);
    for (std::size_t i = 0; i < devices.size(); ++i)
        if (devices[i].channels > 0 && lowered(devices[i].name).find(w) != std::string::npos) return i;
    throw std::invalid_argument("no " + std::string(kind) + " device matching '" + std::string(want) +
                                "' (see --list-audio-devices)");
}

int bytes_per_sample(SampleFormat f) {
    switch (f) {
        case SampleFormat::Float: return 4;
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int32: return 4;
        case SampleFormat::UInt8: return 1;
    }
    return 0;
}

std::vector<double> channel0(std::span<const std::byte> raw, SampleFormat fmt, int channels) {
    const std::size_t frame = static_cast<std::size_t>(bytes_per_sample(fmt) * channels);
    std::vector<double> out(raw.size() / frame);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = read_sample(raw.data() + i * frame, fmt);
    return out;
}

void to_device(std::span<const float> x, SampleFormat fmt, int channels, std::vector<std::byte>& out) {
    const std::size_t bps = static_cast<std::size_t>(bytes_per_sample(fmt));
    out.resize(x.size() * bps * static_cast<std::size_t>(channels));
    std::byte* p = out.data();
    for (const float v : x)
        for (int c = 0; c < channels; ++c, p += bps) write_sample(p, fmt, v);
}

CapturePipeline::CapturePipeline(SampleFormat fmt, int channels, int device_rate, CaptureFifo& fifo)
    : fmt_(fmt), channels_(channels), dec_(device_rate), fifo_(fifo) {
    if (channels < 1) throw std::invalid_argument("CapturePipeline: channels < 1");
}

void CapturePipeline::operator()(std::span<const std::byte> raw) {
    const std::size_t frame = static_cast<std::size_t>(bytes_per_sample(fmt_) * channels_);
    std::vector<double> x;
    if (carry_.empty() && raw.size() % frame == 0) {
        x = channel0(raw, fmt_, channels_);
    } else {
        carry_.insert(carry_.end(), raw.begin(), raw.end());
        const std::size_t whole = carry_.size() / frame * frame;
        x = channel0(std::span<const std::byte>(carry_).first(whole), fmt_, channels_);
        carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(whole));
    }
    if (x.empty()) return;
    frames_in_.fetch_add(x.size(), std::memory_order_relaxed);
    fifo_.write(dec_(x));
}

}  // namespace data2g::audio
