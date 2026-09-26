// The three-state noise blanker, at the MODEL boundary.
//
// WDSP has two impulse blankers and at most one may run, so the operator's
// control is Off -> NB -> NB2 -> Off rather than a toggle. That turns SliceModel
// into the place where three things have to stay consistent at once, and each of
// them is a way the feature can look finished and not be:
//
//   1. `nbOn()` still answers the question every OTHER caller asks. rigctl's
//      NB, SmartCat's NB, TCI's rx_nb_enable, the MIDI mapping, the keyboard
//      shortcut and the band-stack entry all read a bool, and none of them was
//      touched by NB2. If `nbOn()` stopped meaning "a blanker is running" they
//      would all be quietly wrong.
//   2. ONE intent carries the whole state. The seam verb takes kind, level and
//      fill together precisely so no ordering between them can exist; a model
//      that emitted the kind and the level as separate intents would reach a
//      backend as two requests, and the first of them would be incoherent.
//   3. A radio's echo cannot DOWNGRADE a host kind. `SliceDelta::nb` is a bool
//      because a radio-side blanker has one blanker; applying it naively turns
//      NB2 into NB the next time the radio says anything about the blanker at
//      all. That is the bug that would have survived every test in this tree.
//
// No radio, no backend, no DSP: a SliceModel, its command sink and its signals.

#include "models/SliceModel.h"
#include "core/backends/NoiseBlankerKind.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include <cstdio>

using AetherSDR::NoiseBlankerFill;
using AetherSDR::NoiseBlankerKind;
using AetherSDR::SliceModel;

namespace {

int g_failed = 0;

void check(const char* what, bool ok)
{
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The wire text a Flex would receive. Captured rather than sent, so this
    // test can assert what the model SAYS as well as what it holds.
    QStringList commands;
    SliceModel slice(0);
    QObject::connect(&slice, &SliceModel::commandReady, &slice,
                     [&commands](const QString& text) { commands.append(text); });

    // ── 1. The bool door still works, and still means what it meant ────────
    check("a fresh slice has no blanker",
          !slice.nbOn() && slice.nbKind() == NoiseBlankerKind::Off);
    check("and the default fill is WDSP's own zero",
          slice.nbFill() == AetherSDR::kDefaultNoiseBlankerFill);

    slice.setNb(true);
    check("setNb(true) selects the FIRST blanker, which is what every existing "
          "caller meant",
          slice.nbOn() && slice.nbKind() == NoiseBlankerKind::Impulse);
    slice.setNb(false);
    check("setNb(false) turns the blanker off",
          !slice.nbOn() && slice.nbKind() == NoiseBlankerKind::Off);

    // ── 2. The cycle, and nbOn() through all of it ─────────────────────────
    QSignalSpy intents(&slice, &SliceModel::noiseBlankerCommandIssued);
    QSignalSpy bools(&slice, &SliceModel::nbChanged);
    QSignalSpy kinds(&slice, &SliceModel::nbKindChanged);

    slice.setNbLevel(80);
    slice.setNbFill(NoiseBlankerFill::Interpolate);
    intents.clear();
    bools.clear();
    kinds.clear();

    slice.setNbKind(NoiseBlankerKind::Advanced);
    check("NB2 is on as far as every bool consumer is concerned", slice.nbOn());
    check("while the kind says which one it is",
          slice.nbKind() == NoiseBlankerKind::Advanced);
    check("ONE intent carries the whole state", intents.count() == 1);
    if (intents.count() == 1) {
        const QList<QVariant> args = intents.takeFirst();
        check("the intent's kind is Advanced",
              args.at(0).value<NoiseBlankerKind>() == NoiseBlankerKind::Advanced);
        check("its level is the one already set, not a default",
              args.at(1).toInt() == 80);
        check("and it carries the fill, so the backend never has to remember it "
              "separately",
              args.at(2).value<NoiseBlankerFill>() == NoiseBlankerFill::Interpolate);
    }
    check("both signals fire for one change, so a bool consumer and a kind "
          "consumer see the same event",
          bools.count() == 1 && kinds.count() == 1);
    check("and the bool that fired is true",
          bools.count() == 1 && bools.first().at(0).toBool());

    // The FILL is a live parameter, not a build-time one: changing it while NB2
    // runs must reach the seam on its own.
    intents.clear();
    slice.setNbFill(NoiseBlankerFill::MeanHold);
    check("a fill change emits its own intent while NB2 is running",
          intents.count() == 1);
    check("and the kind rides along unchanged",
          intents.count() == 1
              && intents.first().at(0).value<NoiseBlankerKind>()
                     == NoiseBlankerKind::Advanced);
    intents.clear();
    slice.setNbFill(NoiseBlankerFill::MeanHold);
    check("setting the same fill again emits nothing", intents.count() == 0);

    // ── 3. The fill survives the rest of the cycle ─────────────────────────
    slice.setNbKind(NoiseBlankerKind::Off);
    check("Off leaves the operator's fill choice alone",
          slice.nbFill() == NoiseBlankerFill::MeanHold);
    slice.setNbKind(NoiseBlankerKind::Impulse);
    check("and so does the first blanker, whose window is always zeroed",
          slice.nbFill() == NoiseBlankerFill::MeanHold);
    check("the level survives the cycle too", slice.nbLevel() == 80);

    // ── 4. The Flex wire text: a bool, and still sent ──────────────────────
    commands.clear();
    slice.setNbKind(NoiseBlankerKind::Advanced);
    check("a kind change still sends the Flex command, because a Flex has one "
          "blanker and 'on' is the whole truth it can carry",
          commands.contains(QStringLiteral("slice set 0 nb=1")));
    commands.clear();
    slice.setNbKind(NoiseBlankerKind::Off);
    check("and Off sends nb=0",
          commands.contains(QStringLiteral("slice set 0 nb=0")));
    commands.clear();
    slice.setNbFill(NoiseBlankerFill::Zero);
    check("a fill change sends NO wire text — there is no Flex command for a "
          "stage a Flex does not have",
          commands.isEmpty());

    // ── 5. A radio's echo must not downgrade a host kind ───────────────────
    slice.setNbKind(NoiseBlankerKind::Advanced);
    {
        AetherSDR::SliceDelta d;
        d.nb = true;   // a Flex-shaped echo: "the blanker is on"
        slice.applyChanges(d);
    }
    check("nb=true from a radio does NOT turn NB2 into NB",
          slice.nbKind() == NoiseBlankerKind::Advanced && slice.nbOn());
    {
        AetherSDR::SliceDelta d;
        d.nb = false;
        slice.applyChanges(d);
    }
    check("nb=false from a radio does turn the blanker off",
          slice.nbKind() == NoiseBlankerKind::Off && !slice.nbOn());
    {
        AetherSDR::SliceDelta d;
        d.nb = true;
        slice.applyChanges(d);
    }
    check("and from Off, nb=true means the first blanker",
          slice.nbKind() == NoiseBlankerKind::Impulse);
    {
        // A host backend publishes the kind itself — retained state after an
        // identity change, where the slice model is new and the backend is not.
        AetherSDR::SliceDelta d;
        d.nb = true;
        d.nbKind = NoiseBlankerKind::Advanced;
        d.nbFill = NoiseBlankerFill::HoldSample;
        slice.applyChanges(d);
    }
    check("a backend's own kind wins over the bool beside it",
          slice.nbKind() == NoiseBlankerKind::Advanced
              && slice.nbFill() == NoiseBlankerFill::HoldSample);

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
