#pragma once

// C interface to the Swift live-preview session (AppleLivePreview.swift).
// SpeechAnalyzer/SpeechTranscriber are Swift-only APIs, so the Swift side
// exports these with @_cdecl and C++ calls them through this header.
//
// Sample positions are 16 kHz frame indices on the caller's timeline: feed()
// says where each chunk starts, and results report where their text starts
// and ends on that same timeline.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// state: 0 = preparing (message may say what, e.g. a model download),
//        1 = ready (audio fed before this was dropped),
//        2 = failed (message says why; the session produces nothing more).
typedef void (*aether_lp_state_cb)(void* ctx, int32_t state, const char* utf8Message);
// isFinal: 1 for text the engine will not revise, 0 for provisional text that
// a later result starting at the same position replaces.
typedef void (*aether_lp_text_cb)(void* ctx, const char* utf8Text, int64_t startSample,
                                  int64_t endSample, int32_t isFinal);

// 1 when this OS and machine can run the session at all (macOS 26+ and
// SpeechTranscriber.isAvailable); 0 otherwise. Cheap; no model work.
int32_t aether_lp_is_supported(void);

// Starts preparing a session for a BCP-47 language ("en", "en-US"; empty
// means the system language). Returns NULL when unsupported. Callbacks run on
// arbitrary threads and never after aether_lp_destroy() returns.
void* aether_lp_create(const char* language, void* ctx, aether_lp_text_cb onText,
                       aether_lp_state_cb onState);

// Mono float samples at 16 kHz in [-1, 1]. Dropped until the session is ready.
void aether_lp_feed(void* session, const float* samples, int32_t count, int64_t startSample);

// Ask the engine to finalise everything up to this position (async).
void aether_lp_finalize_through(void* session, int64_t sample);

// Stops the session. No callback runs after this returns.
void aether_lp_destroy(void* session);

#ifdef __cplusplus
}
#endif
