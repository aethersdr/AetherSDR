#pragma once

namespace AetherSDR {

// Releases a connected radio from inside a termination-signal handler, for any
// family: a signal skips every destructor, and a radio never told to stop can
// stay held (an HL2 streams at the dead host until power-cycled). A backend
// registers an async-signal-safe release; the app shell installs the handlers.

using FatalSignalReleaseFn = void (*)() noexcept;

// Add `fn` to the fixed table the handler calls. Idempotent; never call it from
// a handler. Returns false only when the table is full.
bool registerFatalSignalRelease(FatalSignalReleaseFn fn) noexcept;

// Whether `fn` is in the table.
[[nodiscard]] bool isFatalSignalReleaseRegistered(FatalSignalReleaseFn fn) noexcept;

// Call every registered release. ASYNC-SIGNAL-SAFE; a no-op with none.
void fireFatalSignalRelease() noexcept;

// Fire the releases, restore the default disposition and re-raise `sig`, so
// the process dies exactly as it would have without a handler.
// ASYNC-SIGNAL-SAFE.
void fireFatalSignalReleaseAndReraise(int sig) noexcept;

// Install `handler` for `sig` unless the process inherited SIG_IGN for it:
// nohup ignores SIGHUP, and a handler would make it fatal again. Returns false
// when the signal was left ignored.
bool installSignalHandlerUnlessIgnored(int sig, void (*handler)(int)) noexcept;

// Install handlers for SIGTERM/SIGINT (and SIGHUP/SIGQUIT on POSIX) that call
// fireFatalSignalReleaseAndReraise(). Crash signals are left to crash
// reporting. Call once, early in main(); safe with nothing registered.
void installFatalSignalRelease() noexcept;

}  // namespace AetherSDR
