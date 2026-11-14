// Operator actions declared once in ControlActionRegistry and published to
// every input surface (keyboard shortcuts, MIDI, and the controller mappers
// that list them). See ControlActionRegistry.h for the contract.

#include "MainWindow.h"
#include "MainWindowHelpers.h"
#include "AppletPanel.h"
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

    // ── Filter width ───────────────────────────────────────────────────
    // A preset lands on the radio's nearest declared width when it has a
    // fixed list (RxApplet::applyNearestFilterWidth), else on the width itself.
    const auto hasSlice = [this] { return m_radioModel.isConnected() && activeSlice(); };
    for (int hz : {100, 250, 400, 3000}) {
        const QString label = hz >= 1000 ? QStringLiteral("%1 kHz").arg(hz / 1000)
                                         : QStringLiteral("%1 Hz").arg(hz);
        reg.add(A::trigger(QStringLiteral("filter_width_%1").arg(hz),
                           QStringLiteral("rx.filterWidth%1").arg(hz),
                           QStringLiteral("Filter Width %1").arg(label), "Filter",
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
    // 10 Hz per key press, as the RX applet's RIT/XIT arrows step.
    static constexpr float kOffsetLimitHz = 5000.0f;
    reg.add(A::level("rit_offset", "rx.ritOffset", "RIT Offset", "RIT/XIT",
                     -kOffsetLimitHz, kOffsetLimitHz, 10,
        [this](float v) { if (auto* s = activeSlice()) s->setRit(s->ritOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->ritFreq() : 0; })
        .inMidiCategory("RX").when(dsp(Feature::Rit)));
    reg.add(A::level("xit_offset", "tx.xitOffset", "XIT Offset", "RIT/XIT",
                     -kOffsetLimitHz, kOffsetLimitHz, 10,
        [this](float v) { if (auto* s = activeSlice()) s->setXit(s->xitOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = activeSlice(); return s ? s->xitFreq() : 0; })
        .inMidiCategory("TX").when([this, canTransmit] { return canTransmit() && activeSlice(); }));

    // ── Slices: select by letter, A/B copy and swap, new, close ────────
    const auto sliceByLetter = [this](const QString& letter) -> SliceModel* {
        for (SliceModel* s : m_radioModel.slices())
            if (s && s->letter() == letter) return s;
        return nullptr;
    };
    for (const char* letter : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
        const QString l = QString::fromLatin1(letter);
        reg.add(A::trigger(QStringLiteral("slice_select_%1").arg(l.toLower()),
                           QStringLiteral("global.sliceSelect%1").arg(l),
                           QStringLiteral("Select Slice %1").arg(l), "Slice",
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
    for (int n = 1; n <= 8; ++n) {
        reg.add(A::trigger(QStringLiteral("pan_select_%1").arg(n),
                           QStringLiteral("global.panSelect%1").arg(n),
                           QStringLiteral("Select Panadapter %1").arg(n), "Display",
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
    for (int khz : {5, 10, 25, 50, 100, 200}) {
        reg.add(A::trigger(QStringLiteral("pan_span_%1k").arg(khz),
                           QStringLiteral("global.panSpan%1k").arg(khz),
                           QStringLiteral("Panadapter Span %1 kHz").arg(khz), "Display",
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
    for (int n = 1; n <= 10; ++n) {
        reg.add(A::trigger(QStringLiteral("memory_recall_%1").arg(n),
                           QStringLiteral("global.memoryRecall%1").arg(n),
                           QStringLiteral("Recall Memory %1").arg(n), "Display",
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
    for (const bool tx : {false, true}) {
        const QString side = tx ? QStringLiteral("TX") : QStringLiteral("RX");
        const QString key = side.toLower();
        const auto sideAvailable = [this, tx, canTransmit] {
            return m_radioModel.isConnected() && activeSlice() && (!tx || canTransmit());
        };
        for (int n = 1; n <= 4; ++n) {
            reg.add(A::trigger(QStringLiteral("%1_antenna_%2").arg(key).arg(n),
                               QStringLiteral("%1.antenna%2").arg(key).arg(n),
                               QStringLiteral("%1 Antenna %2").arg(side).arg(n), tx ? "TX" : "Slice",
                               [antennaChoices, pickAntenna, tx, n](float) {
                                   const QStringList choices = antennaChoices(tx);
                                   if (n <= choices.size()) pickAntenna(tx, choices.at(n - 1));
                               })
                        .inMidiCategory(tx ? "TX" : "RX")
                        .when([sideAvailable, antennaChoices, tx, n] {
                            return sideAvailable() && antennaChoices(tx).size() >= n;
                        }));
        }
        reg.add(A::trigger(QStringLiteral("%1_antenna_next").arg(key),
                           QStringLiteral("%1.antennaNext").arg(key),
                           QStringLiteral("Next %1 Antenna").arg(side), tx ? "TX" : "Slice",
                           [this, antennaChoices, pickAntenna, tx](float) {
                               const QStringList choices = antennaChoices(tx);
                               auto* s = activeSlice();
                               if (!s || choices.isEmpty()) return;
                               const int at = choices.indexOf(tx ? s->txAntenna() : s->rxAntenna());
                               pickAntenna(tx, choices.at((at + 1) % choices.size()));
                           })
                    .inMidiCategory(tx ? "TX" : "RX")
                    .when([sideAvailable, antennaChoices, tx] {
                        return sideAvailable() && antennaChoices(tx).size() > 1;
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
