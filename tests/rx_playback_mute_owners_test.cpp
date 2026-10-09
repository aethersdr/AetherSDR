// Live RX leaves the shared sink while something plays audio back through it.
// The guard these cases protect is overlap: the QSO recorder, the PUDU monitor
// and a voice keyer preview ask independently, and the one that finishes first
// must not unmute under the others.
//
// The edges matter as much as the state: the caller drops a Qt connection on a
// mute edge and restores it on an unmute edge, and Qt permits duplicates, so a
// repeated request that reported an edge would stack another copy of the feed.
//
// Run: ./build/rx_playback_mute_owners_test

#include "gui/RxPlaybackMuteOwners.h"

#include <QString>

#include <cstdio>
#include <string>

using namespace AetherSDR;
using Edge = RxPlaybackMuteOwners::Edge;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

const QString kRecorder = QStringLiteral("qso-recorder");
const QString kMonitor  = QStringLiteral("pudu-monitor");
const QString kKeyer    = QStringLiteral("voice-keyer-preview");

} // namespace

int main()
{
    // Nothing playing: live RX reaches the sink.
    {
        RxPlaybackMuteOwners owners;
        report("fresh_owner_set_is_not_muted", !owners.muted());
    }

    // One producer: the first request is the mute edge, the release the unmute.
    {
        RxPlaybackMuteOwners owners;
        const Edge on = owners.set(kRecorder, true);
        const bool muted = owners.muted();
        const Edge off = owners.set(kRecorder, false);
        report("one_owner_mutes_and_unmutes",
               on == Edge::Mute && muted && off == Edge::Unmute && !owners.muted());
    }

    // The defect this replaces: the keyer's preview ending must not restore
    // live RX while the recorder is still playing back.
    {
        RxPlaybackMuteOwners owners;
        owners.set(kRecorder, true);
        const Edge second = owners.set(kKeyer, true);
        const Edge keyerDone = owners.set(kKeyer, false);
        const bool stillMuted = owners.muted();
        const Edge recorderDone = owners.set(kRecorder, false);
        report("the_last_owner_out_lifts_the_mute",
               second == Edge::None && keyerDone == Edge::None && stillMuted
                   && recorderDone == Edge::Unmute && !owners.muted());
    }

    // Order is irrelevant: whoever leaves last lifts it.
    {
        RxPlaybackMuteOwners owners;
        owners.set(kKeyer, true);
        owners.set(kRecorder, true);
        const Edge first = owners.set(kRecorder, false);
        const Edge last = owners.set(kKeyer, false);
        report("release_order_does_not_matter",
               first == Edge::None && last == Edge::Unmute && !owners.muted());
    }

    // A repeated request from the same owner is not an edge, so the caller
    // never re-drops or re-adds the connection.
    {
        RxPlaybackMuteOwners owners;
        owners.set(kMonitor, true);
        const Edge again = owners.set(kMonitor, true);
        const Edge off = owners.set(kMonitor, false);
        const Edge offAgain = owners.set(kMonitor, false);
        report("repeated_requests_report_no_edge",
               again == Edge::None && off == Edge::Unmute && offAgain == Edge::None
                   && !owners.muted());
    }

    // An unmute from an owner that never asked leaves the others alone.
    {
        RxPlaybackMuteOwners owners;
        owners.set(kRecorder, true);
        const Edge stranger = owners.set(kKeyer, false);
        report("unmute_from_an_owner_that_never_asked_is_ignored",
               stranger == Edge::None && owners.muted());
    }

    // All three at once, the contest case: two keyer previews around a
    // recorder playback, with the monitor joining in the middle.
    {
        RxPlaybackMuteOwners owners;
        owners.set(kRecorder, true);
        owners.set(kKeyer, true);
        owners.set(kMonitor, true);
        owners.set(kKeyer, false);
        owners.set(kRecorder, false);
        const bool stillMuted = owners.muted();
        const Edge last = owners.set(kMonitor, false);
        report("three_owners_hold_until_the_last_releases",
               stillMuted && last == Edge::Unmute && !owners.muted());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
