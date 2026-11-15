#pragma once

#include <QWidget>
#include <QPointer>

class QCheckBox;
class QComboBox;

namespace AetherSDR {

class SliceModel;

// AdaptiveFilterControls — the reusable Adaptive-RX-filter control group
// (enable checkbox + Min low-cut, Max high-cut, Min SNR, Response, Splatter).
// Bound to a SliceModel as the single source of truth, so any number of
// instances (the VFO flag, the RX applet, ...) stay in sync live: editing one
// calls the slice setter, whose *Changed signal updates the others. RFC #3878.
//
// The controls carry no fixed width — each row is label + stretched combo, so
// they flex to the host column. `withHeader` toggles the leading separator;
// `compact` tightens spacing to match a dense applet.
class AdaptiveFilterControls : public QWidget {
    Q_OBJECT

public:
    // Which controls this instance renders — lets the group be split across hosts
    // (e.g. checkbox+bounds in one column, presets in another), all bound to the
    // same slice so they stay in sync.
    enum Section {
        SecCheckbox = 1 << 0,
        SecBounds   = 1 << 1,   // Min low-cut + Max high-cut
        SecPresets  = 1 << 2,   // Min SNR + Response + Splatter
        SecAll      = SecCheckbox | SecBounds | SecPresets,
    };

    // twoColumn lays the group out in two sub-columns (left: checkbox + filter
    // bounds, right: behaviour presets) instead of a single stack — the compact
    // applet-style arrangement.
    explicit AdaptiveFilterControls(int sections = SecAll, bool withHeader = true,
                                    bool compact = false, bool twoColumn = false,
                                    QWidget* parent = nullptr);

    // Re-bind to the active slice (nullptr to detach). Reflects the slice's
    // current values; does NOT load persisted prefs (see loadPrefs).
    void setSlice(SliceModel* slice);

    // Per-slice JSON persistence (AppSettings key "AdaptiveFilter"), shared so
    // every host writes one schema. loadPrefs pushes saved values INTO the slice
    // (call once when a slice is set up); savePrefs writes the slice's current
    // values back (called on each user edit).
    static void loadPrefs(SliceModel* slice);
    static void savePrefs(SliceModel* slice);

signals:
    // Emitted when the visible row set changes (checkbox toggled), so a host can
    // relayout its container/flag around the new height.
    void sizeChanged();

private:
    void updateVisibility();

    QPointer<SliceModel> m_slice;
    QCheckBox* m_chk{nullptr};
    QComboBox* m_minLow{nullptr};
    QComboBox* m_maxHigh{nullptr};
    QComboBox* m_minSnr{nullptr};
    QComboBox* m_response{nullptr};
    QComboBox* m_splatter{nullptr};
    QComboBox* m_hetReject{nullptr};
    QWidget*   m_loRow{nullptr};
    QWidget*   m_hiRow{nullptr};
    QWidget*   m_snrRow{nullptr};
    QWidget*   m_responseRow{nullptr};
    QWidget*   m_splatterRow{nullptr};
    QWidget*   m_hetRow{nullptr};
};

} // namespace AetherSDR
