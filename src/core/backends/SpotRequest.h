#pragma once

#include <QSet>
#include <QMetaType>
#include <QString>
#include <cmath>

namespace AetherSDR {

enum class SpotAction { Add, Remove, Clear, Trigger, Count };
struct SpotControl {
    QSet<SpotAction> actions;
    bool supports(SpotAction action) const { return actions.contains(action); }
};

// Text is unescaped data. Only the vendor encoder substitutes wire spaces.
struct SpotRequest {
    SpotAction action{SpotAction::Count};
    QString callsign, source, spotter, comment, color;
    double frequencyMhz{0.0};
    int lifetimeSeconds{0};
    int index{-1};

    bool valid() const
    {
        if (action == SpotAction::Clear) { return index == -1; }
        if (action == SpotAction::Remove || action == SpotAction::Trigger) { return index >= 0; }
        if (action != SpotAction::Add || index != -1
            || !std::isfinite(frequencyMhz) || frequencyMhz <= 0.0
            || frequencyMhz > 1.0e6 || lifetimeSeconds < 0
            || callsign.isEmpty() || source.isEmpty()) { return false; }
        for (const QString* text : {&callsign, &source, &spotter, &comment}) {
            if (text->size() > 1024) { return false; }
            for (const QChar ch : *text) {
                if (!ch.isPrint() || ch == QLatin1Char('|')) { return false; }
            }
        }
        if (!color.isEmpty()) {
            if (color.size() != 9 || !color.startsWith(QLatin1Char('#'))) { return false; }
            for (qsizetype i = 1; i < color.size(); ++i) {
                const QChar ch = color[i].toLower();
                if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('f'))) { return false; }
                if (ch.unicode() > 127) { return false; }
            }
        }
        return true;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SpotRequest)
