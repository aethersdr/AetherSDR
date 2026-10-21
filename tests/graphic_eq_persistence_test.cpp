// The 8-band graphic EQ survives a restart on a radio whose EQ runs on this host
// (#5611): State round-trips through the "EqApplet" settings object, a restore
// issues no command, a restored EQ holds a layout to re-push only when something
// is in it, an untouched EQ writes nothing, damaged settings give defaults, and
// a Flex session in between does not save its EQ over the saved layout.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/HostVoiceChainPolicy.h"
#include "models/EqualizerModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

const QString kKey = QStringLiteral("EqApplet");

void setStored(const QString& json)
{
    AppSettings::instance().setValue(kKey, json);
    AppSettings::instance().save();
}

void clearStored()
{
    AppSettings::instance().remove(kKey);
    AppSettings::instance().save();
}

QJsonObject storedObject()
{
    return QJsonDocument::fromJson(
        AppSettings::instance().value(kKey, "{}").toString().toUtf8()).object();
}

// The state as the database file holds it, past AppSettings' in-memory cache.
EqualizerModel::State onDisk()
{
    QString json;
    if (!AppSettings::instance().readAppRowFromDisk(kKey, json))
        return {};
    return EqualizerModel::stateFromJson(QJsonDocument::fromJson(json.toUtf8()).object());
}

bool holdsLayout(const EqualizerModel::State& s)
{
    return restoredGraphicEqHoldsLayout(s.anyBandNonZero(), s.anyEnabled());
}

// Sixteen distinct band values, so a dropped or swapped band cannot hide.
EqualizerModel::State distinctState()
{
    EqualizerModel::State s;
    s.txEnabled = true;
    s.rxEnabled = false;
    s.txBands = {-10, -7, -3, -1, 2, 4, 6, 10};
    s.rxBands = {9, 8, 5, 3, 1, -2, -4, -6};
    return s;
}

// (i) Operator sets every band and an enable; a fresh model gets them back.
void roundTrip()
{
    clearStored();
    const EqualizerModel::State want = distinctState();

    EqualizerModel first;
    first.setTxEnabled(want.txEnabled);
    first.setRxEnabled(want.rxEnabled);
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        const auto band = static_cast<EqualizerModel::Band>(i);
        first.setTxBand(band, want.txBands[i]);
        first.setRxBand(band, want.rxBands[i]);
    }
    check(first.state() == want, "round trip: state() reports what the setters set");
    check(EqualizerModel::savePersistedState(first.state()),
          "round trip: a changed state is written");
    check(onDisk() == want, "round trip: the saved state is in the database");

    EqualizerModel second;
    second.restoreState(EqualizerModel::loadPersistedState());
    check(second.txEnabled() == want.txEnabled && second.rxEnabled() == want.rxEnabled,
          "round trip: both enables come back");
    bool txSame = true;
    bool rxSame = true;
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        const auto band = static_cast<EqualizerModel::Band>(i);
        txSame = txSame && second.txBand(band) == want.txBands[i];
        rxSame = rxSame && second.rxBand(band) == want.rxBands[i];
    }
    check(txSame, "round trip: all eight TX bands come back");
    check(rxSame, "round trip: all eight RX bands come back");

    // The other enable, so neither is pinned only at its default.
    EqualizerModel::State flipped = want;
    flipped.txEnabled = false;
    flipped.rxEnabled = true;
    EqualizerModel::savePersistedState(flipped);
    check(EqualizerModel::loadPersistedState() == flipped,
          "round trip: RX enable on with TX enable off comes back");
}

// The EqApplet object is shared with the applet's own view selection.
void sharedObjectIsPreserved()
{
    setStored(QStringLiteral("{\"showTx\":false}"));
    EqualizerModel::savePersistedState(distinctState());
    const QJsonObject cfg = storedObject();
    check(cfg.contains(QStringLiteral("showTx"))
              && !cfg.value(QStringLiteral("showTx")).toBool(true),
          "shared object: saving the EQ keeps the applet's showTx");
    check(cfg.contains(QStringLiteral("graphicEq")),
          "shared object: the EQ sits beside it as graphicEq");
}

// (ii) A restored value is not a command: nothing leaves for the radio.
void restoreIsSilent()
{
    EqualizerModel model;
    QSignalSpy commands(&model, &EqualizerModel::commandReady);
    QSignalSpy edits(&model, &EqualizerModel::operatorEdited);
    QSignalSpy txState(&model, &EqualizerModel::txStateChanged);
    QSignalSpy rxState(&model, &EqualizerModel::rxStateChanged);

    model.restoreState(distinctState());
    check(model.state() == distinctState(), "silent restore: the model holds the state");
    check(txState.size() == 1, "silent restore: txStateChanged fires once");
    check(rxState.size() == 1, "silent restore: rxStateChanged fires once");
    check(commands.isEmpty(), "silent restore: NO commandReady is emitted");
    check(edits.isEmpty(), "silent restore: NO operatorEdited is emitted");

    // The positive control for the two spies above.
    model.setTxBand(EqualizerModel::B1k, 3);
    check(commands.size() == 1, "control: an operator setter does emit commandReady");
    check(edits.size() == 1, "control: an operator setter does emit operatorEdited");

    // Radio status is not an operator edit either, so it is never saved.
    QMap<QString, QString> status;
    status.insert(QStringLiteral("mode"), QStringLiteral("1"));
    status.insert(QStringLiteral("63Hz"), QStringLiteral("4"));
    model.applyRxEqStatus(status);
    check(edits.size() == 1, "radio status does not emit operatorEdited");
}

// (iii) Ownership follows what was restored, one term at a time.
void layoutIsDerived()
{
    check(!holdsLayout(EqualizerModel::State{}),
          "layout: all bands 0 dB and both enables off holds none");

    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        EqualizerModel::State tx;
        tx.txBands[i] = (i % 2 == 0) ? 1 : -1;
        EqualizerModel::State rx;
        rx.rxBands[i] = (i % 2 == 0) ? -1 : 1;
        if (!holdsLayout(tx) || !holdsLayout(rx)) {
            std::printf("[FAIL] layout: band %d alone off 0 dB holds none\n", i);
            ++g_failures;
        }
    }
    check(true, "layout: every single band was tried on both sides");

    EqualizerModel::State txOn;
    txOn.txEnabled = true;
    check(holdsLayout(txOn), "layout: TX enable alone holds one");
    EqualizerModel::State rxOn;
    rxOn.rxEnabled = true;
    check(holdsLayout(rxOn), "layout: RX enable alone holds one");

    // Through the store, as MainWindow reads it.
    clearStored();
    EqualizerModel::savePersistedState(distinctState());
    check(holdsLayout(EqualizerModel::loadPersistedState()),
          "layout: a saved EQ with bands set holds one on the next start");
}

// (iv) An operator who never touched the graphic EQ keeps their strip.
void untouchedWritesNothing()
{
    clearStored();
    EqualizerModel model;
    QSignalSpy txState(&model, &EqualizerModel::txStateChanged);
    QSignalSpy rxState(&model, &EqualizerModel::rxStateChanged);

    const EqualizerModel::State restored = EqualizerModel::loadPersistedState();
    check(restored == EqualizerModel::State{}, "untouched: nothing stored loads as defaults");
    model.restoreState(restored);
    check(txState.isEmpty() && rxState.isEmpty(),
          "untouched: restoring defaults emits no state change (no ClientEq write)");
    check(!holdsLayout(restored), "untouched: no layout is held");

    check(!EqualizerModel::savePersistedState(model.state()),
          "untouched: saving the default state reports no write");
    check(!AppSettings::instance().contains(kKey),
          "untouched: the EqApplet key is still absent");

    setStored(QStringLiteral("{\"showTx\":true}"));
    EqualizerModel::savePersistedState(model.state());
    check(!storedObject().contains(QStringLiteral("graphicEq")),
          "untouched: no graphicEq member is added beside showTx");

    // Back to flat after use is a real state and must replace the old one.
    EqualizerModel::savePersistedState(distinctState());
    check(EqualizerModel::savePersistedState(EqualizerModel::State{}),
          "flat again: defaults ARE written over a stored non-default EQ");
    check(!holdsLayout(EqualizerModel::loadPersistedState()),
          "flat again: no layout is held on the next start");
}

// (v) Whatever is in the store, loading yields a usable state.
void damagedSettingsRestoreDefaults()
{
    const EqualizerModel::State defaults;

    setStored(QStringLiteral("this is not json {{{"));
    check(EqualizerModel::loadPersistedState() == defaults, "damaged: not JSON -> defaults");

    setStored(QStringLiteral("[1,2,3]"));
    check(EqualizerModel::loadPersistedState() == defaults, "damaged: a JSON array -> defaults");

    setStored(QStringLiteral("{\"graphicEq\":\"oops\"}"));
    check(EqualizerModel::loadPersistedState() == defaults,
          "damaged: graphicEq of the wrong type -> defaults");

    setStored(QStringLiteral(
        "{\"graphicEq\":{\"txEnabled\":\"yes\",\"tx\":[1,2,3],\"rx\":{\"a\":1}}}"));
    check(EqualizerModel::loadPersistedState() == defaults,
          "damaged: a short band array and wrong types -> defaults");

    // Partial: what is well-formed is kept, the rest defaults.
    setStored(QStringLiteral("{\"graphicEq\":{\"rxEnabled\":true,\"tx\":[1,2,3,4,5,6,7,8]}}"));
    EqualizerModel::State partial;
    partial.rxEnabled = true;
    partial.txBands = {1, 2, 3, 4, 5, 6, 7, 8};
    check(EqualizerModel::loadPersistedState() == partial,
          "partial: present members restore, absent ones default");

    // Out-of-range and non-numeric entries cannot reach the filters.
    setStored(QStringLiteral(
        "{\"graphicEq\":{\"tx\":[99,-99,\"x\",null,10,-10,0,2.5],\"rx\":[0,0,0,0,0,0,0,0]}}"));
    const EqualizerModel::State got = EqualizerModel::loadPersistedState();
    check(got.txBands[0] == 10 && got.txBands[1] == -10,
          "bounds: bands beyond +/-10 dB are clamped");
    check(got.txBands[2] == 0 && got.txBands[3] == 0,
          "bounds: non-numeric bands read as 0 dB");
    check(got.txBands[4] == 10 && got.txBands[5] == -10 && got.txBands[6] == 0,
          "bounds: in-range bands are kept");
    check(got.txBands[7] == 0, "bounds: a fractional band reads as 0 dB");

    EqualizerModel model;
    model.restoreState(got);
    check(model.txBand(EqualizerModel::B63) == 10, "bounds: the model takes the clamped value");
    clearStored();
}

// (vi) HL2, then a Flex, then an HL2 again in one process. The Flex's status
// fills the model; one HL2 edit must not save those values over the saved
// layout. Replays MainWindow's connect-edge order through the same policy.
void flexSessionLeavesSavedLayout()
{
    clearStored();
    const EqualizerModel::State saved = distinctState();
    EqualizerModel::savePersistedState(saved);

    EqualizerModel model;
    model.restoreState(EqualizerModel::loadPersistedState());

    // A Flex connects and reports its own EQ.
    bool fromRadio = true;
    model.applyTxEqStatus({{"mode", "0"}, {"63Hz", "4"}, {"125Hz", "4"}, {"250Hz", "4"},
                           {"500Hz", "4"}, {"1000Hz", "4"}, {"2000Hz", "4"},
                           {"4000Hz", "4"}, {"8000Hz", "4"}});
    model.applyRxEqStatus({{"mode", "1"}, {"63Hz", "-4"}, {"125Hz", "-4"}, {"250Hz", "-4"},
                           {"500Hz", "-4"}, {"1000Hz", "-4"}, {"2000Hz", "-4"},
                           {"4000Hz", "-4"}, {"8000Hz", "-4"}});
    check(!(model.state() == saved), "flex session: the radio's status replaced the model");
    check(!graphicEqPersistAllowed(true, false, fromRadio),
          "flex session: nothing is saved while the Flex is connected");

    // The HL2 connects: the saved layout comes back before any edit.
    if (graphicEqReloadRequired(true, true, fromRadio)) {
        model.restoreState(EqualizerModel::loadPersistedState());
        fromRadio = false;
    }
    check(model.state() == saved, "flex session: the HL2 connect reloads the saved layout");

    // One band moved on the HL2 is saved, and only that band changes.
    model.setTxBand(EqualizerModel::B1k, 7);
    if (graphicEqPersistAllowed(true, true, fromRadio))
        EqualizerModel::savePersistedState(model.state());
    EqualizerModel::State want = saved;
    want.txBands[EqualizerModel::B1k] = 7;
    check(onDisk() == want,
          "flex session: an HL2 edit saves the saved layout plus that band, no Flex value");
    clearStored();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("graphic-eq-persistence-test"));
    QCoreApplication app(argc, argv);
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] no private settings profile\n");
        return 1;
    }
    AppSettings::instance().load();

    roundTrip();
    sharedObjectIsPreserved();
    restoreIsSilent();
    layoutIsDerived();
    untouchedWritesNothing();
    damagedSettingsRestoreDefaults();
    flexSessionLeavesSavedLayout();

    if (g_failures == 0)
        std::printf("graphic_eq_persistence_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
