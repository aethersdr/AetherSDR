// AmpModel unit test — the power-amplifier state machine (#4094). Exercises
// AmpModel::applyChanges(AmpDelta): presence latch, operate change-gating,
// telemetry/handle matching, removal, reset, and the operate-command relay.
// The wire→AmpDelta translation is covered separately by aetherd_amp_decode_test.

#include "models/AmpModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// A "detected power amp" status delta (as FlexBackend would decode it).
static AmpDelta detected(const QString& handle, const QString& model,
                         const QString& ip, std::optional<bool> operate,
                         QMap<QString, QString> telem = {})
{
    AmpDelta d;
    d.handle = handle;
    d.detectedModel = model;
    if (!ip.isEmpty()) d.ip = ip;
    d.operate = operate;
    d.telemetry = std::move(telem);
    return d;
}

// A follow-up status for an already-detected amp (no model, same handle).
static AmpDelta update(const QString& handle, std::optional<bool> operate,
                       QMap<QString, QString> telem = {})
{
    AmpDelta d;
    d.handle = handle;
    d.operate = operate;
    d.telemetry = std::move(telem);
    return d;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AmpDelta>();

    // ---- presence latch: model/ip captured once, on the first detect ----
    {
        AmpModel amp;
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        QSignalSpy state(&amp, &AmpModel::stateChanged);
        bool operateAtPresence = false;
        bool stateWasDeferred = false;
        QObject::connect(&amp, &AmpModel::presenceChanged, &amp,
                         [&amp, &state, &operateAtPresence, &stateWasDeferred](bool present) {
            if (present) {
                operateAtPresence = amp.operate();
                stateWasDeferred = state.count() == 0;
            }
        });
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "192.168.1.50", true));
        CHECK(amp.present());
        CHECK(amp.handle() == "0x1000");
        CHECK(amp.ip() == "192.168.1.50");
        CHECK(amp.modelName() == "PowerGeniusXL");
        CHECK(amp.operate() && operateAtPresence && stateWasDeferred);
        CHECK(presence.count() == 1 && presence.takeFirst().at(0).toBool() == true);
        CHECK(state.count() == 1);
        // A second detect does not re-latch ip/model or re-emit presence.
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "10.0.0.9", true));
        CHECK(amp.ip() == "192.168.1.50");            // unchanged
        CHECK(presence.count() == 0);
    }

    // ---- unidentified detection cannot adopt a model-less TGXL handle ----
    {
        AmpModel amp;
        bool operateAtPresence = false;
        QObject::connect(&amp, &AmpModel::presenceChanged, &amp,
                         [&amp, &operateAtPresence](bool present) {
            if (present) {
                operateAtPresence = amp.operate();
            }
        });
        // FlexBackend normalizes the SmartSDR placeholder to an empty handle.
        amp.applyChanges(detected(QString(), "PowerGeniusXL",
                                  "192.168.1.50", true));
        CHECK(amp.present() && amp.handle().isEmpty());
        CHECK(amp.operate() && operateAtPresence);

        QSignalSpy state(&amp, &AmpModel::stateChanged);
        QSignalSpy telemetry(&amp, &AmpModel::telemetryUpdated);
        amp.applyChanges(update("0x2000", false, {{"state", "STANDBY"}}));
        CHECK(amp.handle().isEmpty());
        CHECK(amp.operate() && state.count() == 0 && telemetry.count() == 0);

        // A later model-bearing PGXL status safely establishes identity and
        // applies its state; model-less updates can only match after that.
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", QString(), false,
                                  {{"state", "STANDBY"}}));
        CHECK(amp.handle() == "0x1000");
        CHECK(!amp.operate() && state.count() == 1 && telemetry.count() == 1);
    }

    // ---- a delta with no detectedModel + unknown handle is a no-op (TGXL case) ----
    {
        AmpModel amp;
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        QSignalSpy tel(&amp, &AmpModel::telemetryUpdated);
        amp.applyChanges(update("0x2000", true, {{"state", "OPERATE"}}));
        CHECK(!amp.present());
        CHECK(presence.count() == 0 && tel.count() == 0);
    }

    // ---- operate change-gating; absent operate leaves it as-is ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", false));
        QSignalSpy st(&amp, &AmpModel::stateChanged);
        amp.applyChanges(update("0x1000", true));     // off→on
        CHECK(amp.operate() && st.count() == 1);
        amp.applyChanges(update("0x1000", true));     // on→on: no re-emit
        CHECK(amp.operate() && st.count() == 1);
        amp.applyChanges(update("0x1000", std::nullopt, {{"temp", "40"}}));  // no state key
        CHECK(amp.operate() && st.count() == 1);      // operate unchanged
        amp.applyChanges(update("0x1000", false));    // on→off
        CHECK(!amp.operate() && st.count() == 2);
    }

    // ---- telemetry forwarded for a matching handle, ignored otherwise ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        QSignalSpy tel(&amp, &AmpModel::telemetryUpdated);
        amp.applyChanges(update("0x1000", std::nullopt, {{"temp", "42"}, {"id", "3.1"}}));
        CHECK(tel.count() == 1);
        amp.applyChanges(update("0x9999", std::nullopt, {{"temp", "99"}}));  // foreign handle
        CHECK(tel.count() == 1);
    }

    // ---- removal clears presence for our handle only ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        AmpDelta rmOther; rmOther.handle = "0x9999"; rmOther.removed = true;
        amp.applyChanges(rmOther);                    // not ours → no-op
        CHECK(amp.present() && presence.count() == 0);
        AmpDelta rm; rm.handle = "0x1000"; rm.removed = true;
        amp.applyChanges(rm);
        CHECK(!amp.present() && amp.handle().isEmpty());
        CHECK(presence.count() == 1 && presence.takeFirst().at(0).toBool() == false);
    }

    // ---- setOperate emits the neutral operate intent; no-op without a handle ----
    // The SmartSDR string is FlexBackend's job now (#4094); the model only signals
    // operate on/off. (See aetherd_amp_encode_test for the wire translation.)
    {
        AmpModel amp;
        QSignalSpy req(&amp, &AmpModel::operateRequested);
        amp.setOperate(true);                         // no handle yet → no intent
        CHECK(req.count() == 0);
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", false));
        amp.setOperate(true);
        CHECK(req.count() == 1);
        CHECK(req.takeFirst().at(0).toBool() == true);
        amp.setOperate(false);
        CHECK(req.takeFirst().at(0).toBool() == false);
    }

    // ---- reset clears present/handle/operate ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        amp.reset();
        CHECK(!amp.present() && amp.handle().isEmpty() && !amp.operate());
    }

    // ---- and everything else the radio told us about it ----
    //
    // reset() is the bulk clear on radio disconnect. A state word or an
    // antenna map left standing outlives the radio that relayed it — the same
    // reason the direct connection's own disconnect handler clears them.
    {
        AmpModel amp;
        QSignalSpy words(&amp, &AmpModel::ampStateChanged);
        QSignalSpy antennas(&amp, &AmpModel::antennaMapChanged);
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "10.0.0.5", true,
                                  {{"state", "TRANSMIT_A"},
                                   {"ant", "ANT1:PORTA,ANT2:PORTB"}}));
        CHECK(amp.stateText() == QStringLiteral("TRANSMIT_A"));
        CHECK(amp.outputForAntenna("ANT1") == QStringLiteral("PORTA"));

        amp.reset();
        CHECK(amp.stateText().isEmpty());
        CHECK(amp.outputForAntenna("ANT1").isEmpty());
        // And it says so, rather than clearing quietly and leaving the panel
        // drawing what it last heard.
        CHECK(words.count() == 2);
        CHECK(antennas.count() == 2);

        // Idempotent: a second reset has nothing to announce.
        amp.reset();
        CHECK(words.count() == 2);
        CHECK(antennas.count() == 2);
    }

    // ---- the antenna → output map, off the radio-relayed status ----
    //
    // "ANT1:PORTA,ANT2:PORTB" — verbatim from the radio's amplifier object.
    // It is the only thing that says which amplifier port a given radio
    // antenna is wired to, and so the only thing that can say where transmit
    // is about to go: `state` distinguishes the two ports only once RF is
    // already flowing.
    //
    // Split exactly as FlexLib's Amplifier.ParseAntennaSettings does — a pair
    // with no colon, or with more than two fields, is skipped rather than
    // treated as an error, so one malformed pair cannot lose the good ones.
    {
        AmpModel amp;
        QSignalSpy antennas(&amp, &AmpModel::antennaMapChanged);
        CHECK(amp.outputForAntenna("ANT1").isEmpty());   // nothing reported yet

        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "10.0.0.5", true,
                                  {{"ant", "ANT1:PORTA,ANT2:PORTB"}}));
        CHECK(antennas.count() == 1);
        CHECK(amp.outputForAntenna("ANT1") == QStringLiteral("PORTA"));
        CHECK(amp.outputForAntenna("ANT2") == QStringLiteral("PORTB"));
        // An antenna the amplifier does not name is not a port — outlining
        // one would claim RF passes through an amplifier it bypasses.
        CHECK(amp.outputForAntenna("XVTR").isEmpty());

        // Unchanged map → no re-announce; the status repeats on every poll.
        amp.applyChanges(update("0x1000", true, {{"ant", "ANT1:PORTA,ANT2:PORTB"}}));
        CHECK(antennas.count() == 1);

        // Malformed pairs are skipped, not fatal.
        amp.applyChanges(update("0x1000", true, {{"ant", "ANT1,ANT2:PORTB:EXTRA,ANT3:PORTA"}}));
        CHECK(antennas.count() == 2);
        CHECK(amp.outputForAntenna("ANT3") == QStringLiteral("PORTA"));
        CHECK(amp.outputForAntenna("ANT1").isEmpty());
        CHECK(amp.outputForAntenna("ANT2").isEmpty());
    }

    // ---- the state word reaches the model on the relayed path too ----
    //
    // The keying lamps are derived from it, and operate() cannot stand in:
    // it is a boolean derived FROM the word, so it cannot say TRANSMIT_A.
    {
        AmpModel amp;
        QSignalSpy words(&amp, &AmpModel::ampStateChanged);
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "10.0.0.5", true,
                                  {{"state", "IDLE"}}));
        CHECK(words.count() == 1);
        CHECK(amp.stateText() == QStringLiteral("IDLE"));
        amp.applyChanges(update("0x1000", true, {{"state", "TRANSMIT_A"}}));
        CHECK(amp.stateText() == QStringLiteral("TRANSMIT_A"));
        CHECK(words.count() == 2);
        // Repeated on every poll — announced once.
        amp.applyChanges(update("0x1000", true, {{"state", "TRANSMIT_A"}}));
        CHECK(words.count() == 2);
        // A bare "state=" must not blank it: applyChanges gates operate on a
        // non-empty value, and the word follows the same rule.
        amp.applyChanges(update("0x1000", std::nullopt, {{"state", ""}}));
        CHECK(amp.stateText() == QStringLiteral("TRANSMIT_A"));
    }

    if (g_failures == 0) {
        std::printf("amp_model_test: all checks passed\n");
        return 0;
    }
    std::printf("amp_model_test: %d failure(s)\n", g_failures);
    return 1;
}
