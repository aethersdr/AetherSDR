// Golden matrix for the consolidated audio format/rate negotiation policy
// (issue #3306). The whole point of consolidation is that the negotiation
// POLICY becomes testable in one headless place, so the entire class of
// "44.1k device silently fails on sink X / OS Y" bugs is caught on CI rather
// than in the field.
//
// Because TargetOs is DATA (not an #ifdef), every cell below runs on every CI
// runner: a Linux runner proves the Windows and macOS ladders too.
//
// Run: ./build/audio_format_negotiation_test

#include "core/AudioFormatNegotiator.h"
#include "core/TxCaptureBuffer.h"

#include <cstdio>
#include <string>

using namespace AetherSDR::AudioFormatNegotiator;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    ++g_total;
    std::printf("%s %-70s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    if (!ok) ++g_failed;
}

std::string fmtOf(const NegotiatedFormat& n)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ok=%d rate=%d fmt=%s ch=%d resampler=%s fellBack=%d",
                  n.ok ? 1 : 0, n.rate, toString(n.fmt), n.channels,
                  toString(n.resampler), n.fellBack ? 1 : 0);
    return buf;
}

// One golden row: negotiate(os,dir,caps,policy) must equal the expected fields.
struct Row {
    std::string name;
    TargetOs    os;
    Direction   dir;
    ResamplerPolicy policy;
    DeviceCaps  caps;
    // expected
    bool          ok;
    int           rate;
    SampleFmt     fmt;
    ResamplerKind resampler;
    int           channels;
    bool          fellBack;
    FormatPreference pref = FormatPreference::Auto;   // optional caller hint
};

void runRow(const Row& r)
{
    const NegotiatedFormat n = negotiate(r.os, r.dir, r.caps, r.policy, kInternalRate, r.pref);
    const bool ok = n.ok == r.ok
                 && (!r.ok || (n.rate == r.rate && n.fmt == r.fmt
                               && n.resampler == r.resampler && n.channels == r.channels
                               && n.fellBack == r.fellBack));
    std::string detail = "got [" + fmtOf(n) + "]";
    report(r.name, ok, ok ? std::string() : detail);
}

// Helpers to build common device shapes.
DeviceCaps dev(QList<int> rates,
               QList<SampleFmt> fmts = {SampleFmt::Float32, SampleFmt::Int16},
               int channels = 2)
{
    DeviceCaps c;
    c.supportedRates = std::move(rates);
    c.supportedFormats = std::move(fmts);
    c.channels = channels;
    if (!c.supportedRates.isEmpty()) {
        c.preferredRate = c.supportedRates.first();
        c.preferredFormat = c.supportedFormats.contains(SampleFmt::Float32)
                                ? SampleFmt::Float32 : SampleFmt::Int16;
    }
    return c;
}

} // namespace

int main()
{
    const auto F = SampleFmt::Float32;
    const auto I = SampleFmt::Int16;
    const auto Pan = ResamplerPolicy::PreservePan;
    const auto Mono = ResamplerPolicy::MonoCollapse;
    const auto Regen = ResamplerPolicy::RegenerateAtRate;
    const auto Out = Direction::Output;
    const auto In = Direction::Input;
    const auto Win = TargetOs::Windows;
    const auto Mac = TargetOs::MacOS;
    const auto Lin = TargetOs::Linux;

    // ── Standard 48k-only DAC, RX speaker (PreservePan) ───────────────────────
    runRow({"std 48k DAC / Win / RX",  Win, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"std 48k DAC / Mac / RX",  Mac, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    // Linux prefers 24k first; 48k-only device -> falls back to 48k (fellBack).
    runRow({"std 48k DAC / Linux / RX", Lin, Out, Pan, dev({48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, true});

    // ── 24k-capable device: documents the INTENDED per-OS divergence ──────────
    runRow({"24k+48k dev / Win / RX -> forces 48k (#2120)", Win, Out, Pan, dev({24000, 48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"24k+48k dev / Mac / RX -> 48k (A2DP #1705)",   Mac, Out, Pan, dev({24000, 48000}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    runRow({"24k+48k dev / Linux / RX -> native 24k (no resample)", Lin, Out, Pan, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── 44.1k-ONLY device: the regression guard. Today RX/Quindar FAIL this;
    //    the unified ladder must succeed on every OS (#3385 / #3306). ──────────
    runRow({"44.1k-only / Win / RX",   Win, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});
    runRow({"44.1k-only / Mac / RX",   Mac, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});
    runRow({"44.1k-only / Linux / RX", Lin, Out, Pan, dev({44100}),
            true, 44100, F, ResamplerKind::PreservePan, 2, true});

    // ── WASAPI Int16-only device: must skip the Float rungs (#2669). The
    //    skipped 48k Float rung counts as a fall-back (fellBack=true). ────────
    runRow({"Int16-only WASAPI / Win / RX", Win, Out, Pan, dev({48000}, {I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, true});

    // ── WASAPI false-negative isFormatSupported: nothing probeable, probe at
    //    open -> take the preferred 48k rung (#2120 / #3231) ──────────────────
    {
        DeviceCaps c;                       // supportedRates empty
        c.isFormatSupportedReliable = false;
        runRow({"WASAPI false-negative / Win / RX -> force 48k probe-at-open",
                Win, Out, Pan, c,
                true, 48000, F, ResamplerKind::PreservePan, 2, false});
    }

    // ── CW sidetone (RegenerateAtRate): no resampler, generator retunes ───────
    runRow({"sidetone 48k dev / Mac -> regenerate, no resampler", Mac, Out, Regen, dev({48000}),
            true, 48000, F, ResamplerKind::None, 2, false});
    runRow({"sidetone 24k-cap / Linux -> 24k, no resampler", Lin, Out, Regen, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── Int16-first output (Pudu/QSO playback are Int16-native, #3306 6b). On a
    //    normal device they get Int16 — no conversion — at the per-OS rate;
    //    Win/Mac prefer 48k (dodges the WASAPI 24k artifacts #2120, same as RX),
    //    Linux stays native 24k. On a Float-only device they fall back to Float
    //    (then the sink's Int16->Float guard #3231 converts). ───────────────────
    runRow({"Int16-native / Mac / playback -> 48k Int16", Mac, Out, Pan, dev({24000, 48000}),
            true, 48000, I, ResamplerKind::PreservePan, 2, false, FormatPreference::Int16First});
    runRow({"Int16-native / Linux / playback -> native 24k Int16", Lin, Out, Pan, dev({24000, 48000}),
            true, 24000, I, ResamplerKind::None, 2, false, FormatPreference::Int16First});
    runRow({"Int16-native / Mac / Float-only dev -> Float fallback (#3231 guard)",
            Mac, Out, Pan, dev({48000}, {F}),
            true, 48000, F, ResamplerKind::PreservePan, 2, true, FormatPreference::Int16First});

    // ── macOS Bluetooth-HFP mic: open the native low rate first (#2615).
    //    preferred-first puts 16k ahead of the 48k device ladder; downstream
    //    voice DSP still normalizes the captured signal to its own 48k domain. ─
    {
        DeviceCaps c = dev({8000, 16000, 24000}, {I});
        c.isBluetoothHfp = true;
        c.preferredRate = 16000;
        c.preferredFormat = I;
        // preferred-first means the native rate is the PRIMARY choice, not a
        // fallback -> fellBack=false.
        runRow({"BT-HFP mic / Mac / TX -> native 16k (#2615)", Mac, In, Pan, c,
                true, 16000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── macOS 16k-native mic that lies about 48k support (#2930): preferred
    //    rate first avoids the silent-48k-open trap. ──────────────────────────
    {
        DeviceCaps c = dev({16000, 48000}, {I});
        c.preferredRate = 16000;
        c.preferredFormat = I;
        runRow({"16k-native mic / Mac / TX -> preferred 16k first (#2930)", Mac, In, Pan, c,
                true, 16000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── Linux mic: 48k first so the TX voice strip skips its ingress SRC ──────
    // The 48 kHz DSP domain makes 48k capture the conversion-free choice; a
    // 24k-capable device must no longer win the first rung.
    // fellBack=true now: Float32 is rung 0 for the float voice strip, so an
    // Int16-only mic takes rung 1. It still gets the preferred RATE, and the
    // engine's own fallback note keys off rate/channels, not this flag.
    runRow({"std mic / Linux / TX -> 48k native", Lin, In, Pan, dev({24000, 48000}, {I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, true});
    // Same inversion applied to the FORMAT axis: the voice strip is a float
    // island, so a device that offers both formats must lead with Float32.
    runRow({"float-capable mic / Linux / TX -> Float32 first", Lin, In, Pan,
            dev({24000, 48000}, {F, I}),
            true, 48000, F, ResamplerKind::PreservePan, 2, false});
    // ...and an Int16-only mic must still negotiate exactly as it did before.
    runRow({"int16-only mic / Linux / TX -> still opens Int16", Lin, In, Pan,
            dev({48000}, {I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, true});
    // A mic that cannot do 48k still falls back to 24k rather than failing.
    runRow({"24k-only mic / Linux / TX -> falls back to 24k", Lin, In, Pan, dev({24000}, {I}),
            true, 24000, I, ResamplerKind::None, 2, true});

    // ── Windows mic: probe-at-open, 48k first (#2929) ─────────────────────────
    {
        DeviceCaps c;
        c.isFormatSupportedReliable = false;
        // Float32, not Int16, is now the first rung. WASAPI's shared mix is
        // float32, so this is the format that needs no conversion on ingress
        // into the 48 kHz float voice strip. Probe-at-open is unchanged: the
        // engine's open-failure ladder walks Int16 if Float is refused.
        runRow({"std mic / Win / TX -> force 48k Float32 probe-at-open", Win, In, Pan, c,
                true, 48000, F, ResamplerKind::PreservePan, 2, false});
    }
    // A Windows mic whose shared format really is Int16-only still lands on
    // Int16 rather than failing to negotiate.
    runRow({"int16-only mic / Win / TX -> falls back to Int16", Win, In, Pan,
            dev({48000}, {I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, true});
    // macOS keeps Int16 capture, and now says so in ONE place. The preferred
    // rung exists for the RATE (#2615 / #2930); it used to carry the device's
    // preferredFormat as well, which for any float-capable CoreAudio mic is
    // Float32 — so while AudioEngine discarded non-Int16 rungs itself the ladder
    // said "Float" and the behaviour was "Int16". Once the engine honours the
    // rung, that contradiction becomes real behaviour, so the rung now leads
    // with the per-OS format order instead (review of #5017).
    runRow({"float-capable mic / Mac / TX -> Int16 still leads at the preferred rate",
            Mac, In, Pan, dev({48000}, {F, I}),
            true, 48000, I, ResamplerKind::PreservePan, 2, false});
    // ...and a Float32-ONLY Mac mic still opens Float, via the preferredFormat
    // catch-all rung. This is the case the old engine-side "skip every non-Int16
    // rung" filter could not serve at all, so the pure ladder is strictly more
    // capable than what it replaces — as a fallback (fellBack=true), not as the
    // macOS default.
    runRow({"float-only mic / Mac / TX -> Float fallback still opens",
            Mac, In, Pan, dev({48000}, {F}),
            true, 48000, F, ResamplerKind::PreservePan, 2, true});
    // A mac mic with no preferred rate falls to formatOrder(), which is
    // Int16-first on macOS.
    {
        DeviceCaps c = dev({48000}, {F, I});
        c.preferredRate = 0;
        runRow({"no-preference mic / Mac / TX -> Int16 first", Mac, In, Pan, c,
                true, 48000, I, ResamplerKind::PreservePan, 2, false});
    }

    // ── Mono-only output device: open at device channel count, downmix ────────
    runRow({"mono-only 48k / Win / RX -> ch=1 downmix", Win, Out, Pan, dev({48000}, {F, I}, 1),
            true, 48000, F, ResamplerKind::PreservePan, 1, false});

    // ── TCI DAX TX / RADE use MonoCollapse, never PreservePan ─────────────────
    runRow({"TCI-TX 48k client / Linux -> MonoCollapse to 24k", Lin, Out, Mono, dev({48000}),
            true, 48000, F, ResamplerKind::MonoCollapse, 2, true});
    // TCI client negotiates 24k then transmits: rate already internal -> None
    // (NOT a hardcoded 48000->24000 mis-resample; the TciServer.cpp:1296 bug).
    runRow({"TCI-TX 24k client / Linux -> no resample", Lin, Out, Mono, dev({24000, 48000}),
            true, 24000, F, ResamplerKind::None, 2, false});

    // ── Total failure: device supports nothing usable ────────────────────────
    runRow({"empty reliable device -> negotiation fails", Lin, Out, Pan, dev({}, {F, I}),
            false, 0, F, ResamplerKind::None, 0, false});

    // ── WASAPI silent-open recovery ladder (#2929, extended by #5017) ─────────
    // The watchdog's decision is a pure function, so the whole state machine —
    // "a non-null open that then delivers no bytes, repeated until something
    // works or the ladder runs out" — is walkable headlessly on every runner,
    // including the Linux ones that compile none of the Windows engine code.
    {
        // ── The ONE ordered open ladder (round-3 review of PR #5017) ───────
        //
        // This used to be two sequences: a (format, channels) silent-open
        // ladder at 48 kHz, and a separate rate x channels x format loop in
        // AudioEngine entered only on a null open. They could interleave into
        // a permanently silent mic. There is one device, so there is now one
        // ladder, and both failure shapes advance the same cursor.
        const auto ladder = txOpenLadder(2);
        report("open ladder / stereo device: 4 rates x 2 formats x 2 channel counts",
               ladder.size() == 16, "size=" + std::to_string(ladder.size()));

        // The 48 kHz prefix is the historical #2929 ladder, unchanged, because
        // a mono-only mic is the common cause of a silent open and must still
        // recover in ONE reopen.
        report("stage 0 == the initial 48k Float32 stereo open",
               ladder.size() > 0 && ladder.at(0).rate == 48000
                   && ladder.at(0).fmt == F && ladder.at(0).channels == 2);
        report("stage 1 == 48k Float32 mono (#2929 behaviour preserved)",
               ladder.size() > 1 && ladder.at(1).rate == 48000
                   && ladder.at(1).fmt == F && ladder.at(1).channels == 1);
        report("stage 2 == 48k Int16 stereo (the format dimension #5017 added)",
               ladder.size() > 2 && ladder.at(2).rate == 48000
                   && ladder.at(2).fmt == I && ladder.at(2).channels == 2);
        report("stage 3 == 48k Int16 mono",
               ladder.size() > 3 && ladder.at(3).rate == 48000
                   && ladder.at(3).fmt == I && ladder.at(3).channels == 1);
        // Stage 4 is the rung the OLD code could never reach from a silent
        // stage 3 — it restarted the other loop at 48k Float stereo instead.
        report("stage 4 == 44100 Float32 stereo, not a restart at 48k",
               ladder.size() > 4 && ladder.at(4).rate == 44100
                   && ladder.at(4).fmt == F && ladder.at(4).channels == 2);

        bool anyDuplicate = false;
        for (int a = 0; a < ladder.size(); ++a) {
            for (int b = a + 1; b < ladder.size(); ++b) {
                if (ladder.at(a) == ladder.at(b)) anyDuplicate = true;
            }
        }
        report("the ladder contains no duplicate (rate, format, channels)",
               !anyDuplicate);
    }
    {
        // A device already clamped to mono by maximumChannelCount() must not
        // burn a rung reopening the identical tuple.
        const auto ladder = txOpenLadder(1);
        report("open ladder / mono-clamped device dedups to 8 rungs",
               ladder.size() == 8, "size=" + std::to_string(ladder.size()));
        report("mono-clamped stage 0 == 48k Float32 mono",
               ladder.size() > 0 && ladder.at(0).rate == 48000
                   && ladder.at(0).fmt == F && ladder.at(0).channels == 1);
        report("mono-clamped stage 1 == 48k Int16 mono",
               ladder.size() > 1 && ladder.at(1).rate == 48000
                   && ladder.at(1).fmt == I && ladder.at(1).channels == 1);
    }
    {
        // ── The state machine, driven through the SHIPPED cursor ───────────
        //
        // The previous version of this block walked a copy of the state
        // machine written inside the test, and modelled every open as
        // non-null — so it could not express the interaction that produced
        // the round-3 P1 and passed against the bug. walkTxOpen() drives
        // TxOpenCursor, the same object AudioEngine drives, and the probe
        // returns a real per-attempt outcome: Null, SilentNonNull, Delivers.
        using Outcome = TxOpenOutcome;

        // A device that ACCEPTS every open but only ever delivers bytes for
        // one tuple — the original #2929/#5017 shape.
        const auto silentExcept = [](int rate, SampleFmt fmt, int ch,
                                     int initialChannels) -> int {
            TxOpenCursor cursor(initialChannels);
            return walkTxOpen(cursor, [&](const TxOpenAttempt& a) {
                return (a.rate == rate && a.fmt == fmt && a.channels == ch)
                           ? Outcome::Delivers
                           : Outcome::SilentNonNull;
            });
        };
        report("silent open / Int16-only STEREO mic is reached (the round-2 P1)",
               silentExcept(48000, I, 2, 2) == 2,
               "stage=" + std::to_string(silentExcept(48000, I, 2, 2)));
        report("silent open / Int16-only MONO mic is reached",
               silentExcept(48000, I, 1, 2) == 3);
        report("silent open / mono-only Float mic still recovers in ONE reopen",
               silentExcept(48000, F, 1, 2) == 1);
        report("silent open / mono-clamped Int16-only mic is reached",
               silentExcept(48000, I, 1, 1) == 1);
        report("silent open / a mic that never delivers terminates instead of looping",
               silentExcept(8000, F, 7, 2) == -1);

        // ── rfoust's exact round-3 scenario ────────────────────────────────
        //
        // 48k Float stereo is observed SILENT at stage 0. The three remaining
        // 48 kHz rungs then return null. Under the old code that null at the
        // last 48 kHz stage dropped into the separate fallback loop, which
        // restarted at 48k Float stereo — the known-silent tuple — accepted
        // the non-null open, and armed no watchdog because the silent ladder
        // was already terminal. Permanently silent, no error anywhere.
        //
        // With one forward-only cursor the walk simply continues to 44100.
        {
            QList<TxOpenAttempt> probed;
            TxOpenCursor cursor(2);
            const int stage = walkTxOpen(cursor, [&](const TxOpenAttempt& a) {
                probed.append(a);
                if (a.rate == 48000 && a.fmt == F && a.channels == 2)
                    return Outcome::SilentNonNull;   // observed silent
                if (a.rate == 48000)
                    return Outcome::Null;            // the rest of 48k refuses
                if (a.rate == 44100 && a.fmt == F && a.channels == 2)
                    return Outcome::Delivers;
                return Outcome::Null;
            });
            report("mixed null/silent walk reaches 44100 instead of stranding",
                   stage == 4, "stage=" + std::to_string(stage));

            int reprobes = 0;
            for (int a = 0; a < probed.size(); ++a) {
                for (int b = a + 1; b < probed.size(); ++b) {
                    if (probed.at(a) == probed.at(b)) ++reprobes;
                }
            }
            // The structural claim: a forward-only cursor CANNOT re-accept a
            // tuple it already observed silent, because that tuple is behind
            // it. Re-probing is the symptom the old two-loop code showed.
            report("no tuple is attempted twice in one walk",
                   reprobes == 0, "reprobes=" + std::to_string(reprobes));
        }

        // Null and SilentNonNull must be interchangeable — that equivalence is
        // the fix, so it gets its own row rather than being implied.
        {
            const auto walkWith = [](Outcome badOutcome) {
                TxOpenCursor cursor(2);
                return walkTxOpen(cursor, [&](const TxOpenAttempt& a) {
                    if (a.rate == 24000 && a.fmt == I && a.channels == 1)
                        return Outcome::Delivers;
                    return badOutcome;
                });
            };
            report("a null open and a no-data open advance the cursor identically",
                   walkWith(Outcome::Null) == walkWith(Outcome::SilentNonNull)
                       && walkWith(Outcome::Null) == 11,
                   "null=" + std::to_string(walkWith(Outcome::Null))
                       + " silent=" + std::to_string(walkWith(Outcome::SilentNonNull)));
        }

        // Interleaved outcomes across the first rungs.
        {
            TxOpenCursor cursor(2);
            const int stage = walkTxOpen(cursor, [&](const TxOpenAttempt& a) {
                if (a.rate == 48000 && a.fmt == F && a.channels == 2)
                    return Outcome::Null;
                if (a.rate == 48000 && a.fmt == F && a.channels == 1)
                    return Outcome::SilentNonNull;
                if (a.rate == 48000 && a.fmt == I && a.channels == 2)
                    return Outcome::Delivers;
                return Outcome::Null;
            });
            report("null at stage 0, silent at stage 1, delivering at stage 2",
                   stage == 2, "stage=" + std::to_string(stage));
        }

        // The cursor a watchdog restart rebuilds from a persisted stage must
        // resume where it left off, not restart the ladder.
        {
            TxOpenCursor resumed(2, 3);
            report("a cursor rebuilt at a persisted stage resumes there",
                   resumed.stage() == 3 && resumed.attempt().rate == 48000
                       && resumed.attempt().fmt == I
                       && resumed.attempt().channels == 1);
            TxOpenCursor clamped(1, 99);
            report("a persisted stage past the end of a shorter ladder is clamped",
                   clamped.stage() == clamped.size() - 1 && !clamped.hasNext());
        }

        // hasNext() is what arms the watchdog, and it must be false exactly at
        // the terminal rung — otherwise the engine arms a retry that has
        // nowhere to go, or fails to arm one that does.
        {
            TxOpenCursor cursor(2);
            int steps = 0;
            while (cursor.hasNext()) { cursor.advance(); ++steps; }
            report("hasNext() is false exactly at the terminal rung",
                   steps == cursor.size() - 1 && cursor.stage() == cursor.size() - 1,
                   "steps=" + std::to_string(steps));
        }
    }

    // -- The TCI-suppressed drain vs the silent-open watchdog (round 4 of #5017).
    //
    // While a TCI client owns TX audio, AudioEngine::onTxAudioReady() drains
    // the local mic and throws the bytes away. Those discarded bytes are the
    // only evidence the silent-open watchdog can have that the endpoint works,
    // because nothing else on that path touches the capture device. If they do
    // not count, a working mic reads as silent for the whole TCI session and
    // the watchdog walks the recovery ladder off a rung that was never broken.
    //
    // The evidence rule below is the SHIPPED one -- BoundedRead::deliveredBytes(),
    // the same call the suppressed branch makes -- and the cursor is the shipped
    // one. What the test supplies is the watchdog's expiry condition: advance
    // iff the endpoint has not been proven to deliver. (The engine's own
    // `if (m_txReceivedAnyBytes) return;` sits in a Qt timer lambda inside
    // Q_OS_WIN and is not reachable from a headless binary; this pins the two
    // rules it composes, not the lambda itself.)
    {
        using AetherSDR::TxCaptureBuffer::BoundedRead;

        const auto drain = [](int blockBytes, qint64 discarded) {
            BoundedRead r;
            r.block = QByteArray(blockBytes, '\0');
            r.discardedBytes = discarded;
            return r;
        };

        // Every shape a suppressed drain can take, against the shipped rule.
        report("suppressed drain carrying bytes proves the endpoint delivers",
               drain(4096, 0).deliveredBytes());
        report("suppressed drain that only DISCARDED proves the endpoint delivers",
               drain(0, 262144).deliveredBytes());
        report("suppressed drain that read nothing proves nothing",
               !drain(0, 0).deliveredBytes());

        // A TCI session's worth of suppressed callbacks, then the watchdog fires.
        const auto stageAfterWatchdog = [](const QList<BoundedRead>& session) {
            bool delivered = false;
            for (const BoundedRead& d : session)
                if (d.deliveredBytes()) delivered = true;  // engine: m_txReceivedAnyBytes = true
            TxOpenCursor cursor(2);
            if (!delivered && cursor.hasNext()) cursor.advance();  // engine: watchdog retry
            return cursor.stage();
        };

        report("a mic delivering under TCI suppression does NOT advance the ladder",
               stageAfterWatchdog({drain(4096, 0), drain(4096, 0)}) == 0);
        report("a discard-only session under TCI suppression does NOT advance the ladder",
               stageAfterWatchdog({drain(0, 262144)}) == 0);
        report("a mic that is genuinely silent under TCI suppression still advances",
               stageAfterWatchdog({drain(0, 0), drain(0, 0)}) == 1);
        report("no capture callbacks at all under TCI suppression still advances",
               stageAfterWatchdog({}) == 1);
    }

    // -- resamplerKindFor unit checks (the two stereo strategies stay distinct).
    report("resamplerKindFor 24k Pan == None",
           resamplerKindFor(24000, Pan) == ResamplerKind::None);
    report("resamplerKindFor 48k Pan == PreservePan",
           resamplerKindFor(48000, Pan) == ResamplerKind::PreservePan);
    report("resamplerKindFor 48k Mono == MonoCollapse",
           resamplerKindFor(48000, Mono) == ResamplerKind::MonoCollapse);
    report("resamplerKindFor 8k Regen == None",
           resamplerKindFor(8000, Regen) == ResamplerKind::None);

    std::printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
