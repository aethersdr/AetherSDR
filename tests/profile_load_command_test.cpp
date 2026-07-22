#include "models/ProfileLoadCommand.h"
#include "models/ProfileLoadPanWriteQueue.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

void checkProfileLoad(const QString& command,
                      const QString& expectedType,
                      const QString& expectedName,
                      const char* name)
{
    const ProfileLoadCommand profileLoad = parseProfileLoadCommand(command);
    check(profileLoad.valid, name);
    check(profileLoad.type == expectedType, "profile load type capture");
    check(profileLoad.name == expectedName, "profile load name capture");
}

} // namespace

int main()
{
    checkProfileLoad(QStringLiteral("profile global load \"SO2R\""),
                     QStringLiteral("global"),
                     QStringLiteral("SO2R"),
                     "global profile load command parses");
    check(profileLoadMayRebuildRadioTopology(QStringLiteral("global")),
          "global profile load is topology-changing");

    checkProfileLoad(QStringLiteral(" profile TX load \"Low Power\" "),
                     QStringLiteral("tx"),
                     QStringLiteral("Low Power"),
                     "TX profile load command parses case-insensitively");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("tx")),
          "TX profile load is not topology-changing");

    checkProfileLoad(QStringLiteral("profile mic load \"Studio Mic\""),
                     QStringLiteral("mic"),
                     QStringLiteral("Studio Mic"),
                     "mic profile load command parses");
    check(!profileLoadMayRebuildRadioTopology(QStringLiteral("mic")),
          "mic profile load is not topology-changing");

    check(!parseProfileLoadCommand(QStringLiteral("profile global save \"SO2R\"")).valid,
          "non-load profile command is ignored");
    check(kProfileLoadDeferredPanFlushDelayMs > kProfileLoadStateWriteHoldMs,
          "deferred pan flush runs after profile-load write hold");
    check(kProfileLoadPostHoldRecoveryDelayMs > kProfileLoadDeferredPanFlushDelayMs,
          "post-hold recovery runs after deferred pan flush");

    // ── isProfileOwnedRadioStateWrite() — the classification contract (#4142) ──
    //
    // sendCmd() DROPS every command this returns true for while the profile-load
    // hold is armed: it returns before a sequence number is allocated, so the
    // command never reaches the wire. These cases pin down exactly which writes
    // are lost, and therefore which ones MUST be deferred by
    // RadioModel::requestPanCenter() rather than handed to sendCmd() and hoped for.

    // A pan center is profile-owned — this is the write that #4142 lost. Direct
    // frequency entry emits an atomic pair; the `slice tune` half survives and the
    // `center=` half was silently dropped, so the slice retuned and the pan did not.
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 center=7.086000")),
          "pan center write is profile-owned (the #4142 drop)");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 center=7.086000 bandwidth=0.200000")),
          "coupled pan center+bandwidth write is profile-owned (zoom/drag also dropped)");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 bandwidth=0.200000")),
          "pan bandwidth write is profile-owned");

    // xpixels/ypixels are the ONE exemption, and only because the client alone
    // knows its pixel geometry — the display is broken until they are sent.
    // MainWindow defers and coalesces them (requestPanDimensionsForRadio); this
    // exemption is a necessity, NOT evidence that early pan writes are safe.
    check(!isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920 ypixels=800")),
          "pan pixel dimensions are exempt (client-owned display geometry)");
    check(!isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920")),
          "xpixels alone is exempt");

    // A single non-pixel field anywhere in the argument list re-arms the guard:
    // the exemption is all-or-nothing, so center can never ride in on a dimension
    // write's coat-tails.
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display pan set 0x40000000 xpixels=1920 center=7.086000")),
          "pixel dimensions mixed with a center write are NOT exempt");

    // Reads are never suppressed — only writes.
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("display pan info 0x40000000")),
          "pan info read is not a profile-owned write");
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("sub slice all")),
          "subscription is not a profile-owned write");

    // `slice tune` is NOT profile-owned: it is the half of the typed-frequency
    // pair that survived the hold. That asymmetry is the bug — the slice moved
    // and the pan could not follow.
    check(!isProfileOwnedRadioStateWrite(QStringLiteral("slice tune 0 7.086000 autopan=0")),
          "slice tune survives the hold (the surviving half of the #4142 pair)");
    check(isProfileOwnedRadioStateWrite(QStringLiteral("slice set 0 rfgain=20")),
          "slice set write is profile-owned");

    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display panafall set 0x40000000 center=7.086000")),
          "panafall write is profile-owned");
    check(isProfileOwnedRadioStateWrite(
              QStringLiteral("display waterfall set 0x42000000 color_gain=50")),
          "waterfall write is profile-owned");

    // ── ProfileLoadPanWriteQueue — deferred-write store semantics (#4142) ──
    //
    // The queue holds the writes sendCmd() would otherwise silently destroy
    // during the profile-load hold. Coalescing is FIELD-WISE per pan: a later
    // center write must never erase a queued bandwidth (whole-struct
    // last-write-wins loses a user's zoom — the exact class of bug #4142 set
    // out to fix).
    {
        const QString pan = QStringLiteral("0x40000000");

        // (a) field-wise last-write-wins
        ProfileLoadPanWriteQueue q;
        q.deferCenter(pan, 14.1);
        q.deferBandwidth(pan, 0.2);
        q.deferCenter(pan, 7.086);
        check(q.size() == 1, "coalescing keeps one entry per pan");
        check(q.pendingCenter(pan).has_value()
                  && qFuzzyCompare(*q.pendingCenter(pan), 7.086),
              "later center supersedes the earlier center (last write wins)");
        check(q.pendingBandwidth(pan).has_value()
                  && qFuzzyCompare(*q.pendingBandwidth(pan), 0.2),
              "a later center write does NOT erase a queued bandwidth");

        // paired center+bandwidth defers both fields atomically
        q.deferCenterBandwidth(pan, 14.250, 0.4);
        check(qFuzzyCompare(*q.pendingCenter(pan), 14.250)
                  && qFuzzyCompare(*q.pendingBandwidth(pan), 0.4),
              "paired center+bandwidth defer updates both fields");

        // band is an independent field with its own last-write-wins
        q.deferBand(pan, QStringLiteral("40"));
        q.deferBand(pan, QStringLiteral("20"));
        check(q.pendingBand(pan).has_value()
                  && *q.pendingBand(pan) == QStringLiteral("20"),
              "later band supersedes the earlier band");

        // (b) supersedeCenterBandwidth erases only those fields
        q.supersedeCenterBandwidth(pan);
        check(!q.pendingCenter(pan).has_value()
                  && !q.pendingBandwidth(pan).has_value(),
              "supersedeCenterBandwidth erases pending center and bandwidth");
        check(q.pendingBand(pan).has_value(),
              "supersedeCenterBandwidth leaves a pending band untouched");
        check(q.size() == 1, "entry survives while any field is still pending");

        // supersedeBand erases only the band
        q.deferCenter(pan, 7.0);
        q.supersedeBand(pan);
        check(!q.pendingBand(pan).has_value(),
              "supersedeBand erases the pending band");
        check(q.pendingCenter(pan).has_value(),
              "supersedeBand leaves a pending center untouched");

        // field-precise supersede: a center-only send must not erase a queued
        // zoom's bandwidth, and a bandwidth corrective must not swallow a
        // queued center (the same F-trap as coalescing, on the erase side)
        q.deferBandwidth(pan, 0.2);
        q.supersedeCenter(pan);
        check(!q.pendingCenter(pan).has_value(),
              "supersedeCenter erases the pending center");
        check(q.pendingBandwidth(pan).has_value(),
              "supersedeCenter leaves a pending bandwidth untouched");
        q.deferCenter(pan, 7.0);
        q.supersedeBandwidth(pan);
        check(!q.pendingBandwidth(pan).has_value(),
              "supersedeBandwidth erases the pending bandwidth");
        check(q.pendingCenter(pan).has_value(),
              "supersedeBandwidth leaves a pending center untouched");

        // (e) an entry whose last field is superseded does not linger
        q.supersedeCenterBandwidth(pan);
        check(q.isEmpty() && q.size() == 0,
              "superseding the last pending field erases the entry");
        check(!q.cancel(pan).has_value(),
              "cancel on a pan with no pending writes reports nothing to void");

        // (c) cancel returns the voided entry for logging and empties it
        q.deferBand(pan, QStringLiteral("40"));
        q.deferCenterBandwidth(pan, 7.086, 0.2);
        const auto voided = q.cancel(pan);
        check(voided.has_value(), "cancel returns the voided entry");
        check(voided
                  && voided->bandKey == QStringLiteral("40")
                  && voided->centerMhz.has_value()
                  && voided->bandwidthMhz.has_value(),
              "the voided entry carries every pending field for the log line");
        check(q.isEmpty(), "cancel removes the pan's whole entry");

        // (d) takeAll drains the queue
        const QString otherPan = QStringLiteral("0x40000001");
        q.deferCenter(pan, 14.1);
        q.deferBand(otherPan, QStringLiteral("15"));
        check(q.size() == 2, "distinct pans queue independently");
        const auto all = q.takeAll();
        check(all.size() == 2, "takeAll returns every pan's entry");
        check(all.contains(pan) && all.contains(otherPan),
              "takeAll keys entries by pan id");
        check(q.isEmpty(), "takeAll leaves the queue empty");

        // clear() is the disconnect path
        q.deferCenter(pan, 14.1);
        q.clear();
        check(q.isEmpty(), "clear empties the queue");

        // PanWrites::isEmpty reflects field occupancy
        PanWrites w;
        check(w.isEmpty(), "a default PanWrites is empty");
        w.centerMhz = 7.086;
        check(!w.isEmpty(), "a PanWrites with any field set is not empty");
    }

    return failures == 0 ? 0 : 1;
}
