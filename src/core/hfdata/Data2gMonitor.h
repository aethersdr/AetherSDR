#pragma once

// Receive-only Data2G monitor (RFC #7159, Stage 1).
//
// Feeds receive audio to the vendored Data2G core's promiscuous receiver and
// returns every burst it completes as a packet dump, whoever the burst was
// addressed to. Data2G puts nothing on the air encrypted, so any station can
// follow any session.
//
// This class cannot transmit. It has no audio output and no PTT: the only
// thing that leaves it is decoded text. Keep it that way — transmit belongs
// to a later stage behind TxCoordinator admission.
//
// Qt-free on purpose, and the Data2G headers stay out of this header: the
// core's include root has directories named audio/, tnc/, host/ and util/
// that must not reach the rest of the tree.
//
// Threading: an instance is single-threaded. feed() runs the search and the
// decode on the calling thread and can block for a noticeable fraction of a
// second on a long burst, so call it from a worker, never from the audio or
// GUI thread.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AetherSDR::hfdata {

struct Data2gPayload {
    std::string label;                 // e.g. "frame 1 (AX.25 W1AW>APRS, UI)"
    std::vector<std::uint8_t> bytes;
    // Per byte: true where the monitor could not know the value (history it
    // never heard). Empty means every byte is known.
    std::vector<bool> unknown;
};

struct Data2gBurst {
    double startSeconds{0.0};          // burst start, seconds of audio fed so far
    std::string head;                  // description lines, each ending in '\n'
    std::vector<Data2gPayload> payloads;
    std::string text;                  // head plus payloads, rendered for a log view
};

class Data2gMonitor {
public:
    // The rates the core's decimator accepts: its 8 kHz working rate and the
    // multiples AetherSDR produces.
    static bool isSupportedSampleRate(int sampleRate);

    // The vendored Data2G release, e.g. "0.4.3".
    static const char* upstreamVersion();

    // The core runs one process-wide decode pool. Set its size (threads
    // including the caller) once, before the first feed() anywhere; the
    // core's own default is min(4, cores / 2).
    static void setDecodeThreads(int threads);

    // Unsupported rates leave the monitor invalid: feed() then returns
    // nothing.
    explicit Data2gMonitor(int sampleRate = 24000);
    ~Data2gMonitor();
    Data2gMonitor(const Data2gMonitor&) = delete;
    Data2gMonitor& operator=(const Data2gMonitor&) = delete;

    bool isValid() const;
    int sampleRate() const;

    // Mono float32 at sampleRate(), in chunks of any size. Level does not
    // need to be normalized. Returns the bursts completed by this chunk.
    std::vector<Data2gBurst> feed(std::span<const float> mono);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR::hfdata
