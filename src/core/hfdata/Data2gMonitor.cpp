#include "core/hfdata/Data2gMonitor.h"

#include "audio/filters.hpp"
#include "config/config.hpp"
#include "monitor/monitor.hpp"
#include "util/pool.hpp"

#include <cstdio>
#include <exception>
#include <optional>
#include <utility>

namespace AetherSDR::hfdata {

struct Data2gMonitor::Impl {
    int sampleRate{0};
    bool valid{false};
    std::optional<data2g::audio::Decimator> decimator;
    std::optional<data2g::monitor::Monitor> monitor;
    std::vector<double> scratch;
    Stamp stamp;
    std::uint64_t faults{0};

    void restart()
    {
        decimator.emplace(sampleRate);
        monitor.emplace();
    }
};

bool Data2gMonitor::isSupportedSampleRate(int sampleRate)
{
    return sampleRate == data2g::config::FS
        || sampleRate == 24000
        || sampleRate == 48000;
}

const char* Data2gMonitor::upstreamVersion()
{
    return DATA2G_VERSION;
}

void Data2gMonitor::setDecodeThreads(int threads)
{
    data2g::pool::set_threads(threads);
}

Data2gMonitor::Data2gMonitor(int sampleRate)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->sampleRate = sampleRate;
    if (!isSupportedSampleRate(sampleRate))
        return;
    try {
        m_impl->restart();
        m_impl->valid = true;
    } catch (const std::exception&) {
        m_impl->decimator.reset();
        m_impl->monitor.reset();
    }
}

Data2gMonitor::~Data2gMonitor() = default;

bool Data2gMonitor::isValid() const
{
    return m_impl->valid;
}

int Data2gMonitor::sampleRate() const
{
    return m_impl->sampleRate;
}

void Data2gMonitor::setStamp(Stamp stamp)
{
    m_impl->stamp = std::move(stamp);
}

void Data2gMonitor::reset()
{
    if (m_impl->valid)
        m_impl->restart();
}

std::uint64_t Data2gMonitor::faultCount() const
{
    return m_impl->faults;
}

std::vector<Data2gBurst> Data2gMonitor::feed(std::span<const float> mono)
{
    std::vector<Data2gBurst> out;
    if (!m_impl->valid || mono.empty())
        return out;

    m_impl->scratch.assign(mono.begin(), mono.end());

    std::vector<double> starts;
    std::vector<data2g::monitor::Dump> dumps;
    bool fault = false;
    try {
        const std::vector<double> atFs = (*m_impl->decimator)(m_impl->scratch);
        dumps = m_impl->monitor->feed(atFs, [this, &starts](double t) {
            starts.push_back(t);
            if (m_impl->stamp)
                return m_impl->stamp(t);
            char stamp[32];
            std::snprintf(stamp, sizeof stamp, "%.1f", t);
            return std::string(stamp);
        });
    } catch (const std::exception&) {
        // The audio is untrusted (Principle VII): a burst the core rejects
        // is dropped, not allowed to take the stream down with it.
        fault = true;
    }
    if (fault) {
        // Whatever the core held is in an unknown state: start clean.
        ++m_impl->faults;
        m_impl->restart();
        return out;
    }

    out.reserve(dumps.size());
    for (std::size_t i = 0; i < dumps.size(); ++i) {
        data2g::monitor::Dump& dump = dumps[i];
        Data2gBurst burst;
        burst.startSeconds = i < starts.size() ? starts[i] : 0.0;
        burst.text = data2g::monitor::render(dump, data2g::monitor::Format::TEXT);
        burst.head = std::move(dump.head);
        burst.payloads.reserve(dump.payloads.size());
        for (data2g::monitor::Payload& payload : dump.payloads) {
            burst.payloads.push_back({std::move(payload.label),
                                      std::move(payload.bytes),
                                      std::move(payload.unknown)});
        }
        out.push_back(std::move(burst));
    }
    return out;
}

} // namespace AetherSDR::hfdata
