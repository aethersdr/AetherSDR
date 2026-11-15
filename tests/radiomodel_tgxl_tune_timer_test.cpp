// TGXL autotune vs the operator TX timer (#6458, PR #6459 review).
//
// Two reviewer orderings that the pure-predicate tests can't reach:
//  1. A relayed autotune the tuner refuses (no tuning edge ever arrives), then
//     the operator keys inside the 5 s window: the over must become the
//     operator's once the confirm deadline passes.
//  2. The radio unkeys a real tune before the tuner sends tuning=0, the operator
//     re-keys while isTuning() is still true: that over is NOT a tune.

#include "models/RadioModel.h"
#include "models/TunerModel.h"
#include "models/TransmitModel.h"
#include "core/backends/TunerDelta.h"

#include <QCoreApplication>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static void tunerState(RadioModel& m, bool operate, bool tuning)
{
    TunerDelta d;
    d.operate = operate;
    d.tuning = tuning;
    m.tunerModel().applyChanges(d);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {   // 1. Refused autotune, then operator key.
        RadioModel m;
        m.tunerModel().setHandle(QStringLiteral("0x1234"));
        tunerState(m, true, false);
        m.tunerModel().autoTune();                       // arms; tuner never reports tuning
        m.transmitModel().setTransmitting(true);         // operator PTT inside the window
        check(!m.isOperatorTransmitting(), "refused tune: over is provisional at the key edge");
        QTest::qWait(RadioStatusOwnership::kTgxlTuneConfirmMs + 300);
        check(m.isOperatorTransmitting(),
              "refused tune: over becomes the operator's after the confirm deadline");
        m.transmitModel().setTransmitting(false);
        check(!m.isOperatorTransmitting(), "unkey clears the operator over");
    }
    {   // 2. Stale completion then operator re-key.
        RadioModel m;
        m.tunerModel().setHandle(QStringLiteral("0x1234"));
        tunerState(m, true, false);
        m.tunerModel().autoTune();
        m.transmitModel().setTransmitting(true);
        tunerState(m, true, true);                       // tuning=1 confirms
        check(!m.isOperatorTransmitting(), "real tune is not an operator over");
        m.transmitModel().setTransmitting(false);        // radio unkeys before tuning=0
        m.transmitModel().setTransmitting(true);         // operator re-keys
        check(m.isOperatorTransmitting(),
              "stale completion: re-key while the tuner still reports tuning is the operator's");
        tunerState(m, true, false);                      // late tuning=0
        check(m.isOperatorTransmitting(),
              "stale completion: late tuning=0 leaves the operator over running");
        m.transmitModel().setTransmitting(false);
    }

    {   // An ATU tag survives a late, unkeyed tuning edge.
        RadioModel m;
        m.tunerModel().setHandle(QStringLiteral("0x1234"));
        tunerState(m, true, true);
        m.transmitModel().noteActivePttSource(TransmitModel::PttSource::Atu);
        tunerState(m, true, false);
        m.transmitModel().setTransmitting(true);
        check(!m.isOperatorTransmitting(), "ATU tag survives an unkeyed tuning edge");
        m.transmitModel().setTransmitting(false);
    }

    {   // A new backend cannot inherit the previous tuner's arm window.
        RadioModel m;
        m.tunerModel().setHandle(QStringLiteral("0x1234"));
        tunerState(m, true, false);
        m.tunerModel().autoTune();
        m.setBackendForTest({}, QStringLiteral("flex"));
        m.transmitModel().setTransmitting(true);
        check(m.isOperatorTransmitting(), "backend replacement clears the tune arm window");
        m.transmitModel().setTransmitting(false);
    }

    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("OK\n");
    return 0;
}
