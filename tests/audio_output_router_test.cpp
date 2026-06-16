// Unit test for AudioOutputRouter — the registry that keeps output-following
// playback sinks (Pudu monitor, QSO playback, …) pinned to the user-selected
// output device, so a newly-added sink can't silently "uncouple" (#3306).
//
// Hardware-independent: exercises seeding, fan-out, and the QPointer guard with
// fake followers; the actual device value is incidental.
//
// Run: ./build/audio_output_router_test

#include "core/AudioOutputRouter.h"

#include <QAudioDevice>
#include <QCoreApplication>
#include <QMediaDevices>

#include <cstdio>
#include <string>

using AetherSDR::AudioOutputRouter;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    ++g_total;
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    if (!ok) ++g_failed;
}

// Minimal sink exposing the setOutputDevice(const QAudioDevice&) contract.
// QObject-derived so the router's QPointer<T> guard applies, matching the real
// sinks (ClientPuduMonitor, QsoRecorder). No Q_OBJECT needed — QPointer tracks
// lifetime via QObject without moc.
struct FakeSink : QObject {
    int calls = 0;
    QAudioDevice last;
    void setOutputDevice(const QAudioDevice& d) { ++calls; last = d; }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A real device if the runner has one; null otherwise — either is fine.
    const QAudioDevice devA = QMediaDevices::defaultAudioOutput();

    {
        AudioOutputRouter router;
        FakeSink s1, s2;

        // Seed device before registering — followers should pick it up on add.
        router.setCurrentDevice(devA);

        router.addFollower(&s1);
        report("addFollower seeds immediately (s1)", s1.calls == 1 && s1.last.id() == devA.id());

        router.addFollower(&s2);
        report("second follower seeded too (s2)", s2.calls == 1);
        report("followerCount == 2", router.followerCount() == 2);

        // A device change fans out to every follower exactly once.
        router.setCurrentDevice(QAudioDevice{});
        report("change re-pushes to s1", s1.calls == 2 && s1.last.isNull());
        report("change re-pushes to s2", s2.calls == 2 && s2.last.isNull());
        report("currentDevice reflects last set", router.currentDevice().isNull());
    }

    {
        // std::function overload + ordering: a follower added AFTER a change is
        // seeded with the latest device, not the original.
        AudioOutputRouter router;
        int calls = 0;
        bool sawNull = false;
        router.setCurrentDevice(QAudioDevice{});
        router.addFollower([&](const QAudioDevice& d) { ++calls; sawNull = d.isNull(); });
        report("std::function follower seeded with latest device", calls == 1 && sawNull);
    }

    {
        // QPointer guard: a follower destroyed before the router must not crash
        // or be invoked on the next change.
        AudioOutputRouter router;
        auto* heapSink = new FakeSink;
        router.addFollower(heapSink);
        report("heap follower seeded", heapSink->calls == 1);
        delete heapSink;                 // QPointer in the router goes null
        router.setCurrentDevice(devA);   // must be a no-op for the dead follower
        report("destroyed follower skipped (no crash)", true);
    }

    {
        // Reentrancy: a follower that registers another follower *during* the
        // fan-out must not crash (the loop iterates a snapshot) and the new
        // follower is seeded by its own addFollower().
        AudioOutputRouter router;
        int aCalls = 0, bCalls = 0;
        router.addFollower([&](const QAudioDevice&) {
            ++aCalls;
            if (aCalls == 2)   // fires on the setCurrentDevice fan-out, mid-loop
                router.addFollower([&](const QAudioDevice&) { ++bCalls; });
        });
        report("seed before fan-out", aCalls == 1 && bCalls == 0);
        router.setCurrentDevice(QAudioDevice{});   // a mutates m_followers mid-fan-out
        report("add-during-fan-out: no crash, new follower seeded",
               aCalls == 2 && bCalls == 1 && router.followerCount() == 2);
    }

    std::printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
