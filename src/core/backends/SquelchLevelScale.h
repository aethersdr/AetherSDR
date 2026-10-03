#pragma once

// Where a slice's squelch level (0..100) opens, on the panadapter's vertical
// axis: offsetDb + dbPerStep * level, read as the pan peak of a steady carrier.
// Published as RadioCapabilities::squelchLevelScale. ABSENT means the backend
// publishes no mapping, so the client draws no SQL line and offers no Auto SQL
// (#6092). Kiwi's server-relative squelch is not described by this record.

#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace AetherSDR {

struct SquelchLevelScale {
    double offsetDb = 0.0;
    double dbPerStep = 1.0;
    // Slice modes the mapping holds in; empty means every mode.
    QStringList modes;
    // True only where the pan's noise floor plus a margin lands on the gate the
    // radio applies, so Auto SQL may pick the level from pan bins.
    bool autoSquelch = false;

    bool operator==(const SquelchLevelScale&) const = default;

    [[nodiscard]] bool appliesTo(const QString& mode) const
    {
        return modes.isEmpty() || modes.contains(mode, Qt::CaseInsensitive);
    }

    [[nodiscard]] double thresholdDb(int level) const
    {
        return offsetDb + dbPerStep * static_cast<double>(level);
    }

    // The 1..100 level whose threshold is nearest `db`, rounding halves up.
    [[nodiscard]] int levelForThresholdDb(double db) const
    {
        if (!(dbPerStep > 0.0)) {
            return 1;
        }
        const double steps = std::clamp((db - offsetDb) / dbPerStep, -1.0, 101.0);
        return std::clamp(static_cast<int>(steps + 0.5), 1, 100);
    }
};

// The published record as it applies to a slice in `mode`: nullopt when the
// backend published none or the mode is not covered. The line and Auto SQL
// both read this, so neither can outlive the other's gate.
[[nodiscard]] inline std::optional<SquelchLevelScale> squelchScaleForMode(
    const std::optional<SquelchLevelScale>& published, const QString& mode)
{
    if (!published || !published->appliesTo(mode)) {
        return std::nullopt;
    }
    return published;
}

[[nodiscard]] inline bool autoSquelchAvailable(const std::optional<SquelchLevelScale>& scale)
{
    return scale && scale->autoSquelch;
}

}  // namespace AetherSDR
