// Operator actions declared once in ControlActionRegistry and published to
// every input surface (keyboard shortcuts, MIDI, and the controller mappers
// that list them). See ControlActionRegistry.h for the contract.

#include "MainWindow.h"
#include "MainWindowHelpers.h"
#include "SpectrumWidget.h"

#include <QDebug>
#include <QStatusBar>
#include <array>
#include <cmath>

namespace AetherSDR {

void MainWindow::nudgeActiveSliceSteps(int steps)
{
    if (!m_radioModel.isConnected()) return;
    auto* s = activeSlice();
    if (!s) return;
    if (s->isLocked()) {
        s->notifyTuneBlockedByLock();
        return;
    }
    const int stepHz = spectrum() ? spectrum()->stepSize() : 100;
    const double newMhz = s->frequency() + steps * stepHz / 1e6;
    applyTuneRequest(s, newMhz, TuneIntent::IncrementalTune, "keyboard-step");
}

void MainWindow::cycleBandAction(int direction)
{
    // The bands that have band_<name> actions, with a frequency inside each so
    // the nearest one identifies the current band.
    struct CycleBand { const char* shortcutId; double mhz; };
    static constexpr std::array<CycleBand, 12> kCycle{{
        {"band_160m", 1.900}, {"band_80m", 3.800},  {"band_60m", 5.357},
        {"band_40m", 7.200},  {"band_30m", 10.125}, {"band_20m", 14.225},
        {"band_17m", 18.118}, {"band_15m", 21.300}, {"band_12m", 24.940},
        {"band_10m", 28.400}, {"band_6m", 50.125},  {"band_2m", 146.000},
    }};
    const int count = static_cast<int>(kCycle.size());
    int current = -1;
    if (auto* s = activeSlice()) {
        double best = 1e9;
        for (int i = 0; i < count; ++i) {
            const double d = std::abs(s->frequency() - kCycle[i].mhz);
            if (d < best) { best = d; current = i; }
        }
    }
    const int next = ((current < 0 ? 0 : current) + direction + count) % count;
    if (auto* a = m_shortcutManager.action(QLatin1String(kCycle[next].shortcutId)))
        if (a->handler) a->handler();
}

void MainWindow::registerControlActions()
{
    auto& reg = m_controlActions;
    reg.setRefusalHandler([this](const QString& name) {
        statusBar()->showMessage(tr("%1 is not available on this radio").arg(name), 4000);
    });

    // Runs an existing keyboard action's handler — for MIDI twins of actions
    // whose keyboard side predates the registry.
    const auto fire = [this](const char* shortcutId) {
        return [this, id = QString::fromLatin1(shortcutId)](float) {
            if (auto* a = m_shortcutManager.action(id))
                if (a->handler) a->handler();
        };
    };
    const auto canTransmit = [this] {
        return m_radioModel.isConnected() && m_radioModel.backendCapabilities().canTransmit;
    };

    using A = ControlAction;

    // ── Band / mode stepping ───────────────────────────────────────────
    reg.add(A::trigger("band_up", "global.bandUp", "Band Up", "Band",
                       [this](float) { cycleBandAction(+1); }).inMidiCategory("Global"));
    reg.add(A::trigger("band_down", "global.bandDown", "Band Down", "Band",
                       [this](float) { cycleBandAction(-1); }).inMidiCategory("Global"));
    reg.add(A::trigger("mode_up", "global.modeUp", "Mode Up", "Mode",
                       [this](float) { cycleModeAction(+1); }).inMidiCategory("Global"));
    reg.add(A::trigger("mode_down", "global.modeDown", "Mode Down", "Mode",
                       [this](float) { cycleModeAction(-1); }).inMidiCategory("Global"));

    // ── Frequency ──────────────────────────────────────────────────────
    reg.add(A::trigger("tune_up_100", "global.tuneUp100", "Tune Up (100 steps)", "Frequency",
                       [this](float) { nudgeActiveSliceSteps(100); }).repeating());
    reg.add(A::trigger("tune_down_100", "global.tuneDown100", "Tune Down (100 steps)", "Frequency",
                       [this](float) { nudgeActiveSliceSteps(-100); }).repeating());
    struct Mirror { const char* shortcutId; const char* midiId; const char* name; const char* cat; };
    static constexpr Mirror kMirrors[] = {
        {"tune_up_1",    "global.tuneUp1",    "Tune Up (1 step)",     "Frequency"},
        {"tune_down_1",  "global.tuneDown1",  "Tune Down (1 step)",   "Frequency"},
        {"tune_up_10",   "global.tuneUp10",   "Tune Up (10 steps)",   "Frequency"},
        {"tune_down_10", "global.tuneDown10", "Tune Down (10 steps)", "Frequency"},
        {"go_to_freq",   "global.goToFreq",   "Go to Frequency",      "Frequency"},
    };
    for (const auto& m : kMirrors) {
        reg.add(A::trigger({}, m.midiId, m.name, m.cat, fire(m.shortcutId)));
        reg.addPair(m.shortcutId, m.midiId);
    }
    reg.add(A::trigger({}, "global.callsignLookup", "Callsign Lookup", "Display",
                       [this](float) { showCallsignLookupDialog(); }));

    // ── DSP ────────────────────────────────────────────────────────────
    // rx.nrEnable predates the registry; its id is what saved MIDI maps hold.
    reg.add(A::toggle("nr_toggle", "rx.nrEnable", "Noise Reduction", "DSP",
        [this](float v) {
            if (auto* s = activeSlice()) {
                if (!m_radioModel.requestRadioNoiseReduction(s, v > 0.5f))
                    showUnsupportedControlNotice();
            }
        },
        [this]() -> float { auto* s = activeSlice(); return s && s->nrOn() ? 1 : 0; })
        .inMidiCategory("RX"));

    // ── TX levels (not keying) ─────────────────────────────────────────
    reg.add(A::level("rf_power", "tx.rfPower", "RF Power", "TX", 0, 100, 5,
        [this](float v) { m_radioModel.transmitModel().setRfPower(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().rfPower(); })
        .when(canTransmit));
    reg.add(A::level("mic_level", "phone.micLevel", "Mic Level", "TX", 0, 100, 5,
        [this](float v) { m_radioModel.transmitModel().setMicLevel(static_cast<int>(v)); },
        [this]() -> float { return m_radioModel.transmitModel().micLevel(); })
        .inMidiCategory("Phone/CW").when(canTransmit));

    // ── Slice ──────────────────────────────────────────────────────────
    // TX assignment is not keying (docs/agents/backends.md).
    reg.add(A::trigger("slice_set_tx", "global.sliceSetTx", "Make Active Slice TX", "Slice",
                       [this](float) { if (auto* s = activeSlice()) s->setTxSlice(true); })
        .inMidiCategory("Global")
        .when([this, canTransmit] { return canTransmit() && activeSlice(); }));

    // ── CW keyer mode (direct set; the toggles already exist) ──────────
    const auto keyer = [this](bool iambic, int mode) {
        return [this, iambic, mode](float) {
            if (!m_radioModel.isConnected()) return;
            auto& tx = m_radioModel.transmitModel();
            tx.setCwIambic(iambic);
            if (iambic)
                tx.setCwIambicMode(mode);
        };
    };
    reg.add(A::trigger("cw_keyer_iambic_a", "cw.keyerIambicA", "CW Keyer: Iambic A", "CW",
                       keyer(true, 0)).inMidiCategory("Phone/CW"));
    reg.add(A::trigger("cw_keyer_iambic_b", "cw.keyerIambicB", "CW Keyer: Iambic B", "CW",
                       keyer(true, 1)).inMidiCategory("Phone/CW"));
    reg.add(A::trigger("cw_keyer_straight", "cw.keyerStraight", "CW Keyer: Straight Key", "CW",
                       keyer(false, 0)).inMidiCategory("Phone/CW"));

    // ── Pairs declared before the registry (keyboard id ↔ MIDI id) ─────
    static constexpr std::pair<const char*, const char*> kLegacyPairs[] = {
        {"split_toggle", "global.splitToggle"},   {"filter_widen", "global.filterWiden"},
        {"filter_narrow", "global.filterNarrow"}, {"tune_up_1mhz", "global.tuneUp1mhz"},
        {"tune_down_1mhz", "global.tuneDown1mhz"}, {"band_zoom", "global.bandZoom"},
        {"segment_zoom", "global.segmentZoom"},   {"pan_zoom_in", "global.panZoomIn"},
        {"pan_zoom_out", "global.panZoomOut"},     {"open_memories", "global.openMemories"},
        {"nr_cycle", "global.nrCycle"},           {"agc_cycle", "global.agcCycle"},
        {"two_tone_tune", "global.twoToneTune"},  {"next_slice", "global.nextSlice"},
        {"prev_slice", "global.prevSlice"},       {"mox_toggle", "tx.mox"},
        {"tune_toggle", "tx.tune"},               {"atu_start", "tx.atuStart"},
        {kPttHoldActionId, "cw.ptt"},             {kCwStraightKeyActionId, kCwStraightKeyActionId},
        {kCwLeftPaddleActionId, kCwLeftPaddleActionId},
        {kCwRightPaddleActionId, kCwRightPaddleActionId},
        {"nb_toggle", "rx.nbEnable"},             {"anf_toggle", "rx.anfEnable"},
        {"mute_toggle", "rx.mute"},               {"lock_toggle", "rx.tuneLock"},
        {"center_lock_toggle", "rx.centerLock"},  {"rit_toggle", "rx.ritEnable"},
        {"xit_toggle", "rx.xitEnable"},           {"nr2_toggle", "rx.nr2Enable"},
        {"rn2_toggle", "rx.rn2Enable"},           {"nr4_toggle", "rx.nr4Enable"},
        {"dfnr_toggle", "rx.dfnrEnable"},         {"step_up", "rx.stepUp"},
        {"step_down", "rx.stepDown"},             {"vox_toggle", "phone.voxEnable"},
        {"speech_proc_toggle", "phone.procEnable"}, {"dax_toggle", "phone.daxEnable"},
        {"tx_monitor_toggle", "phone.monEnable"}, {"squelch_toggle", "rx.squelchEnable"},
        {"master_mute_toggle", "global.masterMute"}, {"tnf_toggle", "global.tnfEnable"},
        {"cw_sidetone_toggle", "cw.sidetoneEnable"}, {"cw_iambic_toggle", "cw.iambicEnable"},
        {"cw_iambic_mode_toggle", "cw.iambicMode"}, {"cw_swap_paddles_toggle", "cw.swapPaddles"},
        {"cwl_toggle", "cw.cwlEnable"},           {"cw_breakin_toggle", "cw.breakInEnable"},
        {"tx_eq_toggle", "eq.txEnable"},          {"rx_eq_toggle", "eq.rxEnable"},
        {"af_gain_up", "rx.afGain"},              {"af_gain_down", "rx.afGain"},
        {"master_volume_up", "global.masterVolume"}, {"master_volume_down", "global.masterVolume"},
        {"agct_up", "rx.agcThreshold"},           {"agct_down", "rx.agcThreshold"},
        {"cw_speed_up", "cw.speed"},              {"cw_speed_down", "cw.speed"},
    };
    for (const auto& [shortcutId, midiId] : kLegacyPairs)
        reg.addPair(QString::fromLatin1(shortcutId), QString::fromLatin1(midiId));
    // Mode actions are registered from modeActionModes() just before this.
    for (const auto& a : m_shortcutManager.actions()) {
        if (a.category == QLatin1String("Mode") && a.id.startsWith(QLatin1String("mode_")))
            reg.addPair(a.id, QStringLiteral("global.mode%1").arg(a.displayName));
    }
    for (const char* band : {"160m", "80m", "60m", "40m", "30m", "20m",
                             "17m", "15m", "12m", "10m", "6m", "2m"})
        reg.addPair(QStringLiteral("band_%1").arg(QLatin1String(band)),
                    QStringLiteral("global.band%1").arg(QLatin1String(band)));

    for (const QString& err : reg.errors())
        qWarning() << "ControlActionRegistry:" << err;
    Q_ASSERT_X(reg.errors().isEmpty(), "registerControlActions", "malformed control action");
}

} // namespace AetherSDR
