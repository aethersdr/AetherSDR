// Operator actions declared once in ControlActionRegistry and published to
// every input surface (keyboard shortcuts, MIDI, and the controller mappers
// that list them). See ControlActionRegistry.h for the contract.

#include "MainWindow.h"
#include "MainWindowHelpers.h"
#include "AppletPanel.h"
#include "CwxPanel.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "RxApplet.h"
#include "SpectrumWidget.h"

#include <QDebug>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
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

void MainWindow::fireCwxMacro(int index, const TxController::Input& input)
{
    // The CWX panel's F-key rules apply: a CW TX slice and a keyer that can send.
    if (!m_cwxPanel || !m_cwxPanel->fireMacro(index, input)) {
        showShortcutNotice(
            tr("CWX Macro F%1 needs a CW transmit slice and a ready CW keyer").arg(index + 1), 5000);
    }
}

// The menu entry whose text, with the menus above it, ends with `path`
// (untranslated literals from MainWindow_Menus.cpp): {"SpotHub..."} finds it
// in whichever menu it lives; {"Band Plan", "Off"} disambiguates. The title
// bar reparents the window's menu bar, after which menuBar() would hand back a
// new empty one, so every menu bar under the window is searched.
QAction* MainWindow::findMenuAction(const QStringList& path) const
{
    std::function<QAction*(const QList<QAction*>&, QStringList&)> walk =
        [&](const QList<QAction*>& actions, QStringList& above) -> QAction* {
            for (QAction* a : actions) {
                above.append(a->text());
                if (above.size() >= path.size() && above.mid(above.size() - path.size()) == path) {
                    above.removeLast();
                    return a;
                }
                if (a->menu()) {
                    if (QAction* hit = walk(a->menu()->actions(), above)) {
                        above.removeLast();
                        return hit;
                    }
                }
                above.removeLast();
            }
            return nullptr;
        };
    for (const QMenuBar* bar : findChildren<QMenuBar*>()) {
        QStringList above;
        if (QAction* hit = walk(bar->actions(), above)) return hit;
    }
    return nullptr;
}

void MainWindow::registerControlActions()
{
    auto& reg = m_controlActions;
    // The radio lacks it, or the state does not allow it yet (no slice C,
    // an empty band stack, not connected): say so, audibly too.
    reg.setRefusalHandler([this](const QString& name) { refuseControlAction(name); });

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

    // ── Filter width ───────────────────────────────────────────────────
    // A preset lands on the radio's nearest declared width when it has a
    // fixed list (RxApplet::applyNearestFilterWidth), else on the width itself.
    const auto hasSlice = [this] { return m_radioModel.isConnected() && activeSlice(); };
    struct FilterPreset { const char* key; const char* midiId; const char* name; int hz; };
    static constexpr FilterPreset kFilterPresets[] = {
        {"filter_width_100",  "rx.filterWidth100",  "Filter Width 100 Hz", 100},
        {"filter_width_250",  "rx.filterWidth250",  "Filter Width 250 Hz", 250},
        {"filter_width_400",  "rx.filterWidth400",  "Filter Width 400 Hz", 400},
        {"filter_width_3000", "rx.filterWidth3000", "Filter Width 3 kHz",  3000},
    };
    for (const auto& f : kFilterPresets) {
        const int hz = f.hz;
        reg.add(A::trigger(f.key, f.midiId, f.name, "Filter",
                           [this, hz](float) {
                               auto* rx = m_appletPanel ? m_appletPanel->rxApplet() : nullptr;
                               if (!rx || !rx->applyNearestFilterWidth(hz))
                                   showUnsupportedControlNotice();
                           })
                    .inMidiCategory("RX").when(hasSlice));
    }
    // Continuous width for a wheel: 0..100 maps logarithmically to 50 Hz..12 kHz.
    static constexpr double kMinWidth = 50.0, kMaxWidth = 12000.0;
    reg.add(A::level({}, "rx.filterWidth", "Filter Width", "Filter", 0, 100, 2,
        [this](float v) {
            const double hz = kMinWidth * std::pow(kMaxWidth / kMinWidth, v / 100.0);
            if (auto* rx = m_appletPanel ? m_appletPanel->rxApplet() : nullptr)
                rx->applyNearestFilterWidth(static_cast<int>(std::lround(hz)));
        },
        [this]() -> float {
            auto* s = activeSlice();
            const double w = s ? std::max(1, s->filterHigh() - s->filterLow()) : kMinWidth;
            const double v = 100.0 * std::log(std::clamp(w, kMinWidth, kMaxWidth) / kMinWidth)
                           / std::log(kMaxWidth / kMinWidth);
            return static_cast<float>(v);
        })
        .inMidiCategory("RX").when(hasSlice));

    // ── Receive DSP: APF, and NB/NR/ANF/APF levels ─────────────────────
    // Available where the radio declares the feature for the slice's mode;
    // a radio with no ReceiveDspControl record keeps today's behavior
    // (the VFO's own controls use the same rule).
    using Feature = ReceiveDspControl::Feature;
    const auto dsp = [this](Feature feature) {
        return [this, feature] {
            auto* s = activeSlice();
            if (!m_radioModel.isConnected() || !s) return false;
            const auto caps = m_radioModel.backendCapabilities();
            return !caps.receiveDspControl || caps.receiveDspControl->supports(feature, s->mode());
        };
    };
    reg.add(A::toggle("apf_toggle", "rx.apfEnable", "APF", "DSP",
        [this](float v) { if (auto* s = activeSlice()) s->setApf(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->apfOn() ? 1 : 0; })
        .inMidiCategory("RX").when(dsp(Feature::Apf)));
    struct DspLevel {
        const char* key; const char* midiId; const char* name; Feature feature;
        void (SliceModel::*set)(int); int (SliceModel::*get)() const;
    };
    static const DspLevel kLevels[] = {
        {"nb_level",  "rx.nbLevel",  "NB Level",  Feature::Nb,  &SliceModel::setNbLevel,  &SliceModel::nbLevel},
        {"nr_level",  "rx.nrLevel",  "NR Level",  Feature::Nr,  &SliceModel::setNrLevel,  &SliceModel::nrLevel},
        {"anf_level", "rx.anfLevel", "ANF Level", Feature::Anf, &SliceModel::setAnfLevel, &SliceModel::anfLevel},
        {"apf_level", "rx.apfLevel", "APF Level", Feature::Apf, &SliceModel::setApfLevel, &SliceModel::apfLevel},
    };
    for (const auto& l : kLevels) {
        const auto set = l.set;
        const auto get = l.get;
        reg.add(A::level(l.key, l.midiId, l.name, "DSP", 0, 100, 5,
            [this, set](float v) { if (auto* s = activeSlice()) (s->*set)(static_cast<int>(v)); },
            [this, get]() -> float { auto* s = activeSlice(); return s ? (s->*get)() : 0; })
            .inMidiCategory("RX").when(dsp(l.feature)));
    }

    // ── RIT / XIT offsets ──────────────────────────────────────────────
    // 10 Hz per key press, as the RX applet's RIT/XIT arrows step. The range
    // is the model's own limit (kIncrementalTuningMaxHz), so a step never
    // pulls a larger offset back; bind these to a relative encoder on MIDI.
    reg.add(A::level("rit_offset", "rx.ritOffset", "RIT Offset", "RIT/XIT", -99999, 99999, 10,
        [this](float v) { if (auto* s = activeSlice()) s->setRit(s->ritOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->ritFreq() : 0; })
        .inMidiCategory("RX").when(dsp(Feature::Rit)));
    reg.add(A::level("xit_offset", "tx.xitOffset", "XIT Offset", "RIT/XIT", -99999, 99999, 10,
        [this](float v) { if (auto* s = activeSlice()) s->setXit(s->xitOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->xitFreq() : 0; })
        .inMidiCategory("TX").when([this, canTransmit] { return canTransmit() && activeSlice(); }));

    // ── Slices: select by letter, A/B copy and swap, new, close ────────
    const auto sliceByLetter = [this](const QString& letter) -> SliceModel* {
        for (SliceModel* s : m_radioModel.slices())
            if (s && s->letter() == letter) return s;
        return nullptr;
    };
    struct SliceSelect { const char* key; const char* midiId; const char* name; const char* letter; };
    static constexpr SliceSelect kSliceSelect[] = {
        {"slice_select_a", "global.sliceSelectA", "Select Slice A", "A"},
        {"slice_select_b", "global.sliceSelectB", "Select Slice B", "B"},
        {"slice_select_c", "global.sliceSelectC", "Select Slice C", "C"},
        {"slice_select_d", "global.sliceSelectD", "Select Slice D", "D"},
        {"slice_select_e", "global.sliceSelectE", "Select Slice E", "E"},
        {"slice_select_f", "global.sliceSelectF", "Select Slice F", "F"},
        {"slice_select_g", "global.sliceSelectG", "Select Slice G", "G"},
        {"slice_select_h", "global.sliceSelectH", "Select Slice H", "H"},
    };
    for (const auto& sel : kSliceSelect) {
        const QString l = QString::fromLatin1(sel.letter);
        reg.add(A::trigger(sel.key, sel.midiId, sel.name, "Slice",
                           [this, l, sliceByLetter](float) {
                               if (auto* s = sliceByLetter(l)) setActiveSlice(s->sliceId());
                           })
                    .inMidiCategory("Global")
                    .when([this, l, sliceByLetter] {
                        return m_radioModel.isConnected() && sliceByLetter(l);
                    }));
    }
    // Copy or swap frequency and mode between slices A and B, as a VFO A/B
    // pair does. A locked target refuses the change, like any other tune.
    const auto retune = [this](SliceModel* to, double mhz, const QString& mode) {
        if (to->isLocked()) { to->notifyTuneBlockedByLock(); return false; }
        if (!mode.isEmpty() && mode != to->mode()) to->setMode(mode);
        applyTuneRequest(to, mhz, TuneIntent::AbsoluteJump, "slice-ab");
        return true;
    };
    const auto abAvailable = [this, sliceByLetter] {
        return m_radioModel.isConnected() && sliceByLetter("A") && sliceByLetter("B");
    };
    reg.add(A::trigger("slice_a_to_b", "global.sliceAToB", "Copy Slice A to B", "Slice",
        [sliceByLetter, retune](float) {
            auto* a = sliceByLetter("A"); auto* b = sliceByLetter("B");
            if (a && b) retune(b, a->frequency(), a->mode());
        }).inMidiCategory("Global").when(abAvailable));
    reg.add(A::trigger("slice_b_to_a", "global.sliceBToA", "Copy Slice B to A", "Slice",
        [sliceByLetter, retune](float) {
            auto* a = sliceByLetter("A"); auto* b = sliceByLetter("B");
            if (a && b) retune(a, b->frequency(), b->mode());
        }).inMidiCategory("Global").when(abAvailable));
    reg.add(A::trigger("slice_swap_ab", "global.sliceSwapAB", "Swap Slices A and B", "Slice",
        [sliceByLetter, retune](float) {
            auto* a = sliceByLetter("A"); auto* b = sliceByLetter("B");
            if (!a || !b) return;
            if (a->isLocked() || b->isLocked()) {
                (a->isLocked() ? a : b)->notifyTuneBlockedByLock();
                return;
            }
            const double fa = a->frequency(), fb = b->frequency();
            const QString ma = a->mode(), mb = b->mode();
            retune(a, fb, mb);
            retune(b, fa, ma);
        }).inMidiCategory("Global").when(abAvailable));
    reg.add(A::trigger("slice_new", "global.sliceNew", "New Slice", "Slice",
        [this](float) {
            if (!m_radioModel.addSlice()) showPanadapterSliceCapacityMessage();
        })
        .inMidiCategory("Global")
        .when([this] {
            return m_radioModel.isConnected() && m_radioModel.backendCapabilities().canCreateSlices;
        }));
    reg.add(A::trigger("slice_close", "global.sliceClose", "Close Active Slice", "Slice",
        [this](float) { if (auto* s = activeSlice()) requestSliceClose(s->sliceId()); })
        .inMidiCategory("Global")
        .when([this] {
            return m_radioModel.isConnected() && activeSlice()
                && m_radioModel.backendCapabilities().canCreateSlices;
        }));

    // ── Panadapters: select by position, add, close, span presets ──────
    const auto panAt = [this](int index) -> PanadapterApplet* {
        if (!m_panStack) return nullptr;
        const auto applets = m_panStack->allApplets();
        return index < applets.size() ? applets.at(index) : nullptr;
    };
    struct PanSelect { const char* key; const char* midiId; const char* name; int n; };
    static constexpr PanSelect kPanSelect[] = {
        {"pan_select_1", "global.panSelect1", "Select Panadapter 1", 1},
        {"pan_select_2", "global.panSelect2", "Select Panadapter 2", 2},
        {"pan_select_3", "global.panSelect3", "Select Panadapter 3", 3},
        {"pan_select_4", "global.panSelect4", "Select Panadapter 4", 4},
        {"pan_select_5", "global.panSelect5", "Select Panadapter 5", 5},
        {"pan_select_6", "global.panSelect6", "Select Panadapter 6", 6},
        {"pan_select_7", "global.panSelect7", "Select Panadapter 7", 7},
        {"pan_select_8", "global.panSelect8", "Select Panadapter 8", 8},
    };
    for (const auto& ps : kPanSelect) {
        const int n = ps.n;
        reg.add(A::trigger(ps.key, ps.midiId, ps.name, "Display",
                           [this, n, panAt](float) {
                               auto* applet = panAt(n - 1);
                               if (!applet) return;
                               // Activating a slice on the pan makes it the active pan
                               // too; an empty pan is activated directly.
                               for (SliceModel* s : m_radioModel.slices()) {
                                   if (s && s->panId() == applet->panId()) {
                                       setActiveSlice(s->sliceId());
                                       return;
                                   }
                               }
                               m_panStack->setActivePan(applet->panId());
                           })
                    .inMidiCategory("Global")
                    .when([this, n, panAt] { return m_radioModel.isConnected() && panAt(n - 1); }));
    }
    reg.add(A::trigger("pan_add", "global.panAdd", "Add Panadapter", "Display",
                       [this](float) { showAddPanadapterDialog(); })
        .inMidiCategory("Global")
        .when([this] { return m_radioModel.isConnected() && m_radioModel.maxPanadapters() > 1; }));
    reg.add(A::trigger("pan_close", "global.panClose", "Close Active Panadapter", "Display",
        [this](float) {
            auto* s = activeSlice();
            const QString panId = s && !s->panId().isEmpty() ? s->panId()
                                : (m_panStack ? m_panStack->activePanId() : QString());
            if (panId.isEmpty() || !m_panStack || m_panStack->count() <= 1) return;
            clearCenterLockForPan(panId, true);
            m_radioModel.removePanadapter(panId);
        })
        .inMidiCategory("Global")
        .when([this] { return m_radioModel.isConnected() && m_panStack && m_panStack->count() > 1; }));
    struct PanSpan { const char* key; const char* midiId; const char* name; int khz; };
    static constexpr PanSpan kPanSpans[] = {
        {"pan_span_5k",   "global.panSpan5k",   "Panadapter Span 5 kHz",   5},
        {"pan_span_10k",  "global.panSpan10k",  "Panadapter Span 10 kHz",  10},
        {"pan_span_25k",  "global.panSpan25k",  "Panadapter Span 25 kHz",  25},
        {"pan_span_50k",  "global.panSpan50k",  "Panadapter Span 50 kHz",  50},
        {"pan_span_100k", "global.panSpan100k", "Panadapter Span 100 kHz", 100},
        {"pan_span_200k", "global.panSpan200k", "Panadapter Span 200 kHz", 200},
    };
    for (const auto& sp : kPanSpans) {
        const int khz = sp.khz;
        reg.add(A::trigger(sp.key, sp.midiId, sp.name, "Display",
                           [this, khz](float) {
                               if (!setActivePanSpan(khz / 1000.0)) showUnsupportedControlNotice();
                           })
                    .inMidiCategory("Global")
                    .when([this] { return m_radioModel.isConnected() && !panSpanFixedByBackend(); }));
    }

    // ── Memories: recall by position, step through them ────────────────
    // Position N is the Nth stored memory in slot order, so the binding stays
    // useful on radios whose slot numbers are sparse.
    const auto memorySlot = [this](int position) -> int {
        const auto keys = m_radioModel.memories().keys();
        return position >= 0 && position < keys.size() ? keys.at(position) : -1;
    };
    const auto recallPosition = [this, memorySlot](int position) {
        const int slot = memorySlot(position);
        if (slot < 0) return;
        auto* s = activeSlice();
        if (activateMemorySpot(slot, s ? s->panId() : QString()))
            m_lastRecalledMemoryPosition = position;
    };
    struct MemoryRecall { const char* key; const char* midiId; const char* name; int n; };
    static constexpr MemoryRecall kMemoryRecall[] = {
        {"memory_recall_1",  "global.memoryRecall1",  "Recall Memory 1",  1},
        {"memory_recall_2",  "global.memoryRecall2",  "Recall Memory 2",  2},
        {"memory_recall_3",  "global.memoryRecall3",  "Recall Memory 3",  3},
        {"memory_recall_4",  "global.memoryRecall4",  "Recall Memory 4",  4},
        {"memory_recall_5",  "global.memoryRecall5",  "Recall Memory 5",  5},
        {"memory_recall_6",  "global.memoryRecall6",  "Recall Memory 6",  6},
        {"memory_recall_7",  "global.memoryRecall7",  "Recall Memory 7",  7},
        {"memory_recall_8",  "global.memoryRecall8",  "Recall Memory 8",  8},
        {"memory_recall_9",  "global.memoryRecall9",  "Recall Memory 9",  9},
        {"memory_recall_10", "global.memoryRecall10", "Recall Memory 10", 10},
    };
    for (const auto& mr : kMemoryRecall) {
        const int n = mr.n;
        reg.add(A::trigger(mr.key, mr.midiId, mr.name, "Display",
                           [recallPosition, n](float) { recallPosition(n - 1); })
                    .inMidiCategory("Global")
                    .when([this, n] {
                        return m_radioModel.isConnected() && m_radioModel.memories().size() >= n;
                    }));
    }
    const auto memoryStep = [this, recallPosition](int direction) {
        const int count = m_radioModel.memories().size();
        if (count == 0) return;
        const int from = m_lastRecalledMemoryPosition < 0 ? (direction > 0 ? -1 : 0)
                                                           : m_lastRecalledMemoryPosition;
        recallPosition(((from + direction) % count + count) % count);
    };
    const auto hasMemories = [this] {
        return m_radioModel.isConnected() && !m_radioModel.memories().isEmpty();
    };
    reg.add(A::trigger("memory_next", "global.memoryNext", "Next Memory", "Display",
                       [memoryStep](float) { memoryStep(+1); })
        .inMidiCategory("Global").when(hasMemories));
    reg.add(A::trigger("memory_prev", "global.memoryPrev", "Previous Memory", "Display",
                       [memoryStep](float) { memoryStep(-1); })
        .inMidiCategory("Global").when(hasMemories));

    // ── Global profiles: load by position in the radio's own list ──────
    // The radio stores them; Profile Manager and the Profiles menu load them
    // through the same typed request. Offered only when the radio reports
    // at least that many.
    struct GlobalProfile { const char* key; const char* midiId; const char* name; int n; };
    static constexpr GlobalProfile kGlobalProfiles[] = {
        {"global_profile_1", "global.globalProfile1", "Load Global Profile 1", 1},
        {"global_profile_2", "global.globalProfile2", "Load Global Profile 2", 2},
        {"global_profile_3", "global.globalProfile3", "Load Global Profile 3", 3},
        {"global_profile_4", "global.globalProfile4", "Load Global Profile 4", 4},
        {"global_profile_5", "global.globalProfile5", "Load Global Profile 5", 5},
    };
    for (const auto& gp : kGlobalProfiles) {
        const int n = gp.n;
        reg.add(A::trigger(gp.key, gp.midiId, gp.name, "Display",
                           [this, n](float) {
                               const QStringList profiles = m_radioModel.globalProfiles();
                               if (n <= profiles.size())
                                   m_radioModel.loadGlobalProfile(profiles.at(n - 1));
                           })
                    .inMidiCategory("Global")
                    .when([this, n] {
                        return m_radioModel.isConnected() && m_radioModel.globalProfiles().size() >= n;
                    }));
    }

    // ── Antennas: pick by position in the radio's own list, or step ────
    // Position N is the Nth choice the RX/TX antenna menu offers, so the
    // binding means the same thing on any radio that lists antennas.
    const auto rxApplet = [this]() -> RxApplet* {
        return m_appletPanel ? m_appletPanel->rxApplet() : nullptr;
    };
    const auto antennaChoices = [rxApplet](bool tx) {
        auto* rx = rxApplet();
        return rx ? (tx ? rx->txAntennaChoices() : rx->rxAntennaChoices()) : QStringList{};
    };
    const auto pickAntenna = [rxApplet](bool tx, const QString& token) {
        auto* rx = rxApplet();
        return rx && (tx ? rx->selectTxAntenna(token) : rx->selectRxAntenna(token));
    };
    // n = 0 is "Next": step to the following choice.
    struct AntennaPick { const char* key; const char* midiId; const char* name;
                         const char* category; const char* midiCategory; bool tx; int n; };
    static constexpr AntennaPick kAntennas[] = {
        {"rx_antenna_1",    "rx.antenna1",    "RX Antenna 1",    "Slice", "RX", false, 1},
        {"rx_antenna_2",    "rx.antenna2",    "RX Antenna 2",    "Slice", "RX", false, 2},
        {"rx_antenna_3",    "rx.antenna3",    "RX Antenna 3",    "Slice", "RX", false, 3},
        {"rx_antenna_4",    "rx.antenna4",    "RX Antenna 4",    "Slice", "RX", false, 4},
        {"rx_antenna_next", "rx.antennaNext", "Next RX Antenna", "Slice", "RX", false, 0},
        {"tx_antenna_1",    "tx.antenna1",    "TX Antenna 1",    "TX",    "TX", true,  1},
        {"tx_antenna_2",    "tx.antenna2",    "TX Antenna 2",    "TX",    "TX", true,  2},
        {"tx_antenna_3",    "tx.antenna3",    "TX Antenna 3",    "TX",    "TX", true,  3},
        {"tx_antenna_4",    "tx.antenna4",    "TX Antenna 4",    "TX",    "TX", true,  4},
        {"tx_antenna_next", "tx.antennaNext", "Next TX Antenna", "TX",    "TX", true,  0},
    };
    for (const auto& ap : kAntennas) {
        const bool tx = ap.tx;
        const int n = ap.n;
        reg.add(A::trigger(ap.key, ap.midiId, ap.name, ap.category,
                           [this, antennaChoices, pickAntenna, tx, n](float) {
                               const QStringList choices = antennaChoices(tx);
                               auto* s = activeSlice();
                               if (!s || choices.isEmpty()) return;
                               if (n > 0) {
                                   if (n <= choices.size()) pickAntenna(tx, choices.at(n - 1));
                                   return;
                               }
                               const int at = choices.indexOf(tx ? s->txAntenna() : s->rxAntenna());
                               pickAntenna(tx, choices.at((at + 1) % choices.size()));
                           })
                    .inMidiCategory(ap.midiCategory)
                    .when([this, antennaChoices, canTransmit, tx, n] {
                        return m_radioModel.isConnected() && activeSlice() && (!tx || canTransmit())
                            && antennaChoices(tx).size() >= (n == 0 ? 2 : n);
                    }));
    }

    // ── Radio-side receive features ────────────────────────────────────
    // WNB: the panadapter's wideband blanker, offered where the overlay menu
    // offers it (radio-side DSP).
    const auto wnbAvailable = [this] {
        return m_radioModel.isConnected() && m_radioModel.activePanadapter()
            && m_radioModel.hasRadioSideDsp();
    };
    reg.add(A::toggle("wnb_toggle", "rx.wnbEnable", "WNB", "DSP",
        [this](float v) {
            m_radioModel.requestPanDisplay(m_radioModel.activePanadapter(),
                                           {PanDisplayField::Wnb, v > 0.5f});
        },
        [this]() -> float {
            auto* pan = m_radioModel.activePanadapter();
            return pan && pan->wnbActive() ? 1 : 0;
        })
        .inMidiCategory("RX").when(wnbAvailable));
    reg.add(A::level("wnb_level", "rx.wnbLevel", "WNB Level", "DSP", 0, 100, 5,
        [this](float v) {
            m_radioModel.requestPanDisplay(m_radioModel.activePanadapter(),
                                           {PanDisplayField::WnbLevel, static_cast<int>(v)});
        },
        [this]() -> float {
            auto* pan = m_radioModel.activePanadapter();
            return pan ? pan->wnbLevel() : 0;
        })
        .inMidiCategory("RX").when(wnbAvailable));
    // Binaural: a radio setup field, offered where the radio declares it.
    reg.add(A::toggle("binaural_toggle", "rx.binaural", "Binaural RX", "Audio",
        [this](float v) {
            m_radioModel.requestRadioSetup({RadioSetupField::BinauralReceive, v > 0.5f});
        },
        [this]() -> float { return m_radioModel.binauralRx() ? 1 : 0; })
        .inMidiCategory("RX")
        .when([this] {
            const auto control = m_radioModel.backendCapabilities().radioSetupControl;
            return m_radioModel.isConnected() && control
                && control->supports(RadioSetupField::BinauralReceive);
        }));
    // Diversity: offered where the VFO's DIV button is (a diversity-capable radio).
    reg.add(A::toggle("diversity_toggle", "rx.diversity", "Diversity RX", "Slice",
        [this](float v) { if (auto* s = activeSlice()) s->setDiversity(v > 0.5f); },
        [this]() -> float { auto* s = activeSlice(); return s && s->diversity() ? 1 : 0; })
        .inMidiCategory("RX")
        .when([this] {
            return m_radioModel.isConnected() && activeSlice() && m_radioModel.isDiversityAllowed();
        }));

    // ── ATU memories (not keying; ATU tune and bypass stay on the TX path) ─
    reg.add(A::toggle("atu_memories_toggle", "tx.atuMemories", "ATU Memories", "TX",
        [this](float v) { m_radioModel.transmitModel().setAtuMemories(v > 0.5f); },
        [this]() -> float { return m_radioModel.transmitModel().memoriesEnabled() ? 1 : 0; })
        .when([this] {
            const auto& tx = m_radioModel.transmitModel();
            return m_radioModel.isConnected() && tx.hasTunerMemories() && tx.atuAvailable();
        }));

    // ── Band stack: step to the nearest bookmark above or below ────────
    const auto hasBandStack = [this] {
        return m_radioModel.isConnected() && activeSlice() && hasBandStackEntries();
    };
    const auto bandStackStep = [this](int direction) { stepBandStack(direction); };
    reg.add(A::trigger("band_stack_up", "global.bandStackUp", "Band Stack Up", "Band",
                       [bandStackStep](float) { bandStackStep(+1); })
        .inMidiCategory("Global").when(hasBandStack));
    reg.add(A::trigger("band_stack_down", "global.bandStackDown", "Band Stack Down", "Band",
                       [bandStackStep](float) { bandStackStep(-1); })
        .inMidiCategory("Global").when(hasBandStack));

    // ── Windows: trigger the menu entry, so every path opens it alike ──
    const auto menuEntry = [this](const QStringList& path) {
        return [this, path](float) {
            if (QAction* act = findMenuAction(path)) act->trigger();
        };
    };
    reg.add(A::trigger("open_spothub", "global.openSpotHub", "Open SpotHub", "Display",
                       menuEntry({"SpotHub..."})).inMidiCategory("Global"));
    reg.add(A::trigger("open_aethermodem", "global.openAetherModem", "Open AetherModem", "Display",
                       menuEntry({"AetherModem..."})).inMidiCategory("Global"));
    reg.add(A::trigger("band_plan_toggle", "global.bandPlanToggle", "Band Plan Toggle", "Display",
                       [this](float) { toggleBandPlanStrip(); }).inMidiCategory("Global"));

    // ── CWX: cancel and the panel (not keying) ─────────────────────────
    const auto cwxAvailable = [this] { return m_radioModel.isConnected(); };
    reg.add(A::trigger("cwx_cancel", "cwx.cancel", "Stop CWX / Macros", "CW",
                       [this](float) { m_radioModel.cwxModel().clearBuffer(); })
        .inMidiCategory("Phone/CW").when(cwxAvailable));
    reg.add(A::toggle("cw_decode_toggle", "cw.decodeRx", "CW Decoder", "CW",
                      [this](float v) { setCwDecodeRxEnabled(v > 0.5f); },
                      [this]() -> float { return cwDecodeRxEnabled() ? 1 : 0; })
        .inMidiCategory("Phone/CW"));
    reg.add(A::trigger("cwx_panel_toggle", "cwx.panelToggle", "Show/Hide CWX Panel", "CW",
                       [this](float) { toggleCwKeyerPanel(); })
        .inMidiCategory("Phone/CW"));

    // ── TX outputs (ACC TX, TX1–TX3) for the current TX band ───────────
    // The radio's own per-band output enables, as TX Band Settings edits
    // them, through the typed TxBandRequest every backend handles or refuses.
    // Offered only where the radio reported a TX band table for this band.
    using BandField = TxBandRequest::Field;
    const auto txBandId = [this] {
        return m_radioModel.txBandIdForFrequency(m_radioModel.transmitModel().transmitFreq());
    };
    struct TxOutput { const char* key; const char* midiId; const char* name; BandField field; };
    static constexpr TxOutput kOutputs[] = {
        {"tx_output_acc", "tx.outputAcc", "ACC TX Output", BandField::AccessoryTx},
        {"tx_output_1",   "tx.output1",   "TX1 Output",    BandField::Tx1},
        {"tx_output_2",   "tx.output2",   "TX2 Output",    BandField::Tx2},
        {"tx_output_3",   "tx.output3",   "TX3 Output",    BandField::Tx3},
    };
    for (const auto& o : kOutputs) {
        const auto field = o.field;
        reg.add(A::toggle(o.key, o.midiId, o.name, "TX",
            [this, txBandId, field](float v) {
                m_radioModel.requestTxBand({txBandId(), field, v > 0.5f});
            },
            [this, txBandId, field]() -> float {
                const auto band = m_radioModel.txBandSettings().value(txBandId());
                const bool on = field == BandField::AccessoryTx ? band.accTx
                              : field == BandField::Tx1 ? band.tx1
                              : field == BandField::Tx2 ? band.tx2 : band.tx3;
                return on ? 1 : 0;
            })
            .when([this, txBandId, canTransmit] {
                return canTransmit() && m_radioModel.txBandSettings().contains(txBandId());
            }));
    }

    // ── Keying actions: on the TX path, like MOX and TUNE ──────────────
    // Keyboard side registers as a keysTx action with a TxController handler
    // (the registerTxShortcut shape); the MIDI side is consumed by
    // dispatchScopedMidiTx with the device's own captured input.
    const auto txShortcut = [this](const QString& id, const QString& name, const char* category,
                                   TxController::Activity activity,
                                   std::function<void(const TxController::Input&)> handler) {
        m_shortcutManager.registerAction(id, name, category, QKeySequence(),
            [this, activity, handler] {
                if (m_radioModel.isConnected())
                    handler(m_radioModel.localTxController()->capture(activity));
            }, /*autoRepeat=*/false, /*keysTx=*/true);
        ShortcutManager::Action* action = m_shortcutManager.action(id);
        action->txActivity = activity;
        action->txHandler = std::move(handler);
    };
    txShortcut("atu_bypass", "ATU Bypass", "TX", TxController::Activity::Atu,
               [](const TxController::Input& input) { (void)input.bypassAtu(); });
    reg.addKeyingMidi("tx.atuBypass", "ATU Bypass", "TX");
    reg.addPair("atu_bypass", "tx.atuBypass");
    struct CwxMacro { const char* key; const char* midiId; const char* name; int index; };
    static constexpr CwxMacro kCwxMacros[] = {
        {"cwx_macro_1",  "cwx.macro1",  "CWX Macro F1",  0},
        {"cwx_macro_2",  "cwx.macro2",  "CWX Macro F2",  1},
        {"cwx_macro_3",  "cwx.macro3",  "CWX Macro F3",  2},
        {"cwx_macro_4",  "cwx.macro4",  "CWX Macro F4",  3},
        {"cwx_macro_5",  "cwx.macro5",  "CWX Macro F5",  4},
        {"cwx_macro_6",  "cwx.macro6",  "CWX Macro F6",  5},
        {"cwx_macro_7",  "cwx.macro7",  "CWX Macro F7",  6},
        {"cwx_macro_8",  "cwx.macro8",  "CWX Macro F8",  7},
        {"cwx_macro_9",  "cwx.macro9",  "CWX Macro F9",  8},
        {"cwx_macro_10", "cwx.macro10", "CWX Macro F10", 9},
        {"cwx_macro_11", "cwx.macro11", "CWX Macro F11", 10},
        {"cwx_macro_12", "cwx.macro12", "CWX Macro F12", 11},
    };
    for (const auto& cm : kCwxMacros) {
        const int index = cm.index;
        txShortcut(cm.key, cm.name, "CW", TxController::Activity::Cwx,
                   [this, index](const TxController::Input& input) { fireCwxMacro(index, input); });
        reg.addKeyingMidi(cm.midiId, cm.name, "Phone/CW");
        reg.addPair(cm.key, cm.midiId);
    }

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
