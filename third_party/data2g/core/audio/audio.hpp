// Sound card plumbing that is not the sound card: device selection, sample
// formats, and the capture conversion chain. Qt-free and device-free so it
// is tested against a fake device (SSTVAE's lesson: the audio bugs lived in
// conversion and matching, not in the driver calls). The device layer is
// core/audio/qt/ or core/audio/miniaudio/ (data2g_audio, picked by
// audio/card.hpp); the FIFOs are in fifo.hpp.
//
// Lifted from SSTVAE's core/audio/audio.hpp (sample formats, the capture
// pipeline), with data2g/audio.py's semantics where they differ: channel 0
// rather than a mixdown, first substring match rather than a unique one,
// integer rate ratios through Decimator rather than resample_poly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio/fifo.hpp"
#include "audio/filters.hpp"

namespace data2g::audio {

struct DeviceInfo {
    std::string name;
    int channels;  // the most this device offers in its direction
};

// audio.find_device: an all-digit `want` is an index; otherwise the first device
// with channels whose name contains `want`, ignoring case. Empty `want`:
// nothing (the system default). Throws std::invalid_argument when nothing
// matches or the index is out of range.
std::optional<std::size_t> select_device(std::span<const DeviceInfo> devices, std::string_view want,
                                         std::string_view kind);

// --list-audio-devices' lines (the apps'): index, channels, name, to stdout.
void print_devices(std::span<const DeviceInfo> devices);

// audio.channels: 2 where the device has two or more, else 1. Stereo
// because PortAudio's ALSA backend corrupted the heap on mono over
// PipeWire; kept so the device sees the same streams as the Python host.
inline int open_channels(const DeviceInfo& d) { return d.channels >= 2 ? 2 : 1; }

// Device sample formats, named as QtMultimedia names them.
enum class SampleFormat { Float, Int16, Int32, UInt8 };
int bytes_per_sample(SampleFormat f);

// Interleaved device bytes -> channel 0 as double in [-1, 1].
std::vector<double> channel0(std::span<const std::byte> raw, SampleFormat fmt, int channels);

// Mono -> interleaved device bytes, the same sample on every channel.
// Integer formats scale by full scale - 1 and clip, so +1.0 cannot wrap.
void to_device(std::span<const float> x, SampleFormat fmt, int channels, std::vector<std::byte>& out);

// One capture device's bytes -> channel 0 -> Decimator -> CaptureFifo, on
// the capture thread. Bytes arriving in pieces that split a frame are
// carried over rather than dropped (a dropped partial frame misaligns
// every sample after it).
class CapturePipeline {
public:
    CapturePipeline(SampleFormat fmt, int channels, int device_rate, CaptureFifo& fifo);
    void operator()(std::span<const std::byte> raw);
    std::uint64_t frames_in() const { return frames_in_.load(std::memory_order_relaxed); }

private:
    SampleFormat fmt_;
    int channels_;
    Decimator dec_;
    CaptureFifo& fifo_;
    std::vector<std::byte> carry_;
    std::atomic<std::uint64_t> frames_in_{0};
};

}  // namespace data2g::audio
