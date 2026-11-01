#pragma once

#include <QString>

#include <functional>

namespace AetherSDR {

// The macOS install the in-app updater keeps current (RFC #5304 Phase 4): the
// AetherSDR.app bundle wherever the user put it, /Applications usually.
//
// A new release is staged as a whole bundle in the update state folder. While
// AetherSDR still runs, it is checked: its code signature, which catches a
// tree that does not match what was signed (the manifest's Ed25519 signature
// is what keeps a hostile payload out), and its self-test. Then, as the very
// last thing before AetherSDR exits, one renamex_np(RENAME_SWAP) trades it for
// the installed bundle, so nothing at the bundle's path is ever half in place,
// and nothing the running app loads late comes from the new release. There is
// no helper. The bundle swapped out is kept as the previous version in the
// state folder, not beside the app, so the Applications folder shows one
// AetherSDR (a maintainer ruling).
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

    // /usr/bin/codesign --verify --deep --strict on tree; problem is what it
    // said when it refuses.
    static bool verifySignature(const QString& tree, QString* problem);

    // Runs <tree>/Contents/MacOS/AetherSDR --update-selftest and waits for it
    // (60 s at most): true when it wrote version as its marker.
    static bool selfTest(const QString& tree, const QString& version);

    // The checks prepare() runs, injected by tests.
    struct Checks {
        std::function<bool(const QString& tree, QString* problem)> signature;
        std::function<bool(const QString& tree, const QString& version)> selfTest;
    };
    static Checks real();

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
