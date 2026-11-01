#pragma once

#include <QString>

#include <functional>

namespace AetherSDR {

// The macOS install (RFC #5304 Phase 4): checks a staged AetherSDR.app (its
// code signature and self-test) while the app runs, then exchanges it for the
// installed bundle in one rename as AetherSDR exits. No helper. See
// docs/architecture/update-manifest.md, "The macOS install (Phase 4)".
class MacInstall {
public:
    // The bundle the binary in appDir (<bundle>/Contents/MacOS) belongs to.
    // Empty when it isn't in one: a build folder's bare binary.
    static QString bundleOf(const QString& appDir);

    // Run from a path macOS translocated: a quarantined app started where it
    // was downloaded. That copy is read-only and gone at the next launch.
    static bool translocated(const QString& bundle);

    // Whether a and b are on one volume, so a rename can move between them.
    static bool sameVolume(const QString& a, const QString& b);

    // The Developer ID team that signed bundle ("944M585CW5"); empty when it
    // has none (an ad-hoc signed local build) or can't be read.
    static QString teamOf(const QString& bundle);

    // /usr/bin/codesign --verify --deep --strict on tree, also requiring, when
    // team is given, a Developer ID signature from that team; problem is what
    // codesign said when it refuses.
    static bool verifySignature(const QString& tree, const QString& team, QString* problem);

    // Whether a process other than this one runs bundle's binary: another
    // AetherSDR, started with open -n, which the swap would pull the bundle
    // out from under.
    static bool otherInstanceRunning(const QString& bundle);

    // Runs <tree>/Contents/MacOS/AetherSDR --update-selftest and waits for it
    // (60 s at most): true when it wrote version as its marker.
    static bool selfTest(const QString& tree, const QString& version);

    // The checks prepare() runs, injected by tests.
    struct Checks {
        std::function<bool(const QString& tree, QString* problem)> signature;
        std::function<bool(const QString& tree, const QString& version)> selfTest;
    };
    // codesign pinned to the team that signed runningBundle, and the self-test.
    static Checks real(const QString& runningBundle);

    enum class Prepared {
        Ready,    // *ready is the bundle to swap in as AetherSDR exits
        Failed,   // it failed a check and was removed (only when !retry, or
                  // for the signature, which no second try changes)
        Kept,     // the self-test failed; stagedDir is left to try again
        Refused,  // nothing to swap: not a bundle, or no room beside the app
    };
    // Gets stagedDir ready to replace bundle. A staged tree on another volume
    // than the bundle is copied beside it first, under a hidden name, so the
    // swap is still one rename. Then the signature and the self-test.
    // retry: a failed self-test keeps the tree (Kept) rather than removing
    // it, for a test that may have failed for want of a display, as AetherSDR
    // quits during a logout.
    static Prepared prepare(const QString& bundle, const QString& stagedDir, const QString& version,
                            const Checks& checks, bool retry, QString* ready, QString* problem);

    // As AetherSDR exits: trades ready for bundle in one step. The bundle
    // swapped out goes to previousDir, replacing what was there, when that is
    // on the same volume; otherwise it is removed. False, with nothing
    // changed, when the swap could not be made.
    static bool swap(const QString& bundle, const QString& ready, const QString& previousDir, QString* problem);
};

} // namespace AetherSDR
