#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

// One predicate for band/segment zoom: whether the "B"/"S" buttons are enabled
// and whether a command path may invoke the declared radio zoom control.
// Flex uses radio-owned mode flags; native-scope radios request a span.
// Every non-button surface goes through MainWindow::togglePanZoomMode /
// setPanZoomMode. Capability:
// RadioCapabilities::panZoomModes.has_value(), not a family (#5554). On
// NotDeclared callers warn and call showUnsupportedControlNotice();
// NotConnected/NoPan stay silent. Receive-only: no TX path (keysTx false).

namespace AetherSDR {

// Smallest declared width covering both edges around the radio-owned center,
// saturating at the widest detent. Widths are positive and sorted ascending.
[[nodiscard]] inline std::optional<int> nativePanZoomWidthHz(
    double centerMhz, double lowMhz, double highMhz,
    std::span<const int> widthsHz)
{
    if (widthsHz.empty()) {
        return std::nullopt;
    }
    // Native scope centers and band-plan edges resolve to whole Hz. Round
    // each before subtracting so MHz representation noise cannot skip an
    // exactly fitting detent. Leave room for both subtraction and doubling.
    const auto frequencyHz = [](double mhz) -> std::optional<std::int64_t> {
        const double hz = mhz * 1.0e6;
        constexpr std::int64_t kMaxHz = std::numeric_limits<std::int64_t>::max() / 4;
        if (!std::isfinite(hz) || hz < 1.0 || hz >= static_cast<double>(kMaxHz)) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::round(hz));
    };
    const auto centerHz = frequencyHz(centerMhz);
    const auto lowHz = frequencyHz(lowMhz);
    const auto highHz = frequencyHz(highMhz);
    if (!centerHz || !lowHz || !highHz || *lowHz >= *highHz) {
        return std::nullopt;
    }
    const std::int64_t neededHz = 2 * std::max(*centerHz - *lowHz,
                                             *highHz - *centerHz);
    for (int widthHz : widthsHz) {
        if (widthHz >= neededHz) {
            return widthHz;
        }
    }
    return widthsHz.back();
}

// Why a band/segment-zoom write is refused, or None if it is admissible.
// Ordered from the most general refusal to the most specific so a caller that
// wants to explain itself names the outermost reason. NotDeclared is the one
// the caller must announce -- see "A REFUSAL MUST SAY SO" above.
enum class PanZoomModeRefusal {
    None,
    NotConnected,
    NotDeclared,
    NoPan,
};

// `panZoomModesDeclared` is RadioCapabilities::panZoomModes.has_value() off
// RadioModel::backendCapabilities() -- a per-feature record the backend
// engages, NOT a family string. Absent means UNDECLARED, and undeclared
// refuses: the failure it describes is a control that moves while the write is
// dropped.
// `panKnown` is "a non-empty pan id that RadioModel::panadapter() resolves".
[[nodiscard]] constexpr PanZoomModeRefusal panZoomModeRefusal(
    bool connected, bool panZoomModesDeclared, bool panKnown) noexcept
{
    if (!connected) {
        return PanZoomModeRefusal::NotConnected;
    }
    if (!panZoomModesDeclared) {
        return PanZoomModeRefusal::NotDeclared;
    }
    if (!panKnown) {
        return PanZoomModeRefusal::NoPan;
    }
    return PanZoomModeRefusal::None;
}

// May this client request band/segment zoom for this pan right now?
// Every command path asks this; none of them may ask anything narrower.
[[nodiscard]] constexpr bool panZoomModeWritable(
    bool connected, bool panZoomModesDeclared, bool panKnown) noexcept
{
    return panZoomModeRefusal(connected, panZoomModesDeclared, panKnown)
           == PanZoomModeRefusal::None;
}

// Should the "B"/"S" buttons be enabled? The same question with the pan taken
// as present: a pan applet exists before the radio hands back its id, and the
// buttons are enabled per radio rather than per pan.
[[nodiscard]] constexpr bool bandSegmentZoomAvailable(
    bool connected, bool panZoomModesDeclared) noexcept
{
    return panZoomModeWritable(connected, panZoomModesDeclared,
                               /*panKnown=*/true);
}

}  // namespace AetherSDR
