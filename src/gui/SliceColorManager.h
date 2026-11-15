#pragma once

#include "SliceColors.h"
#include <QColor>
#include <QObject>
#include <QString>
#include <array>

namespace AetherSDR {

// Manages per-slice color overrides.  Consumers call activeColor()/dimColor()
// instead of reading kSliceColors[] directly.  Connect to colorsChanged() to
// repaint widgets when the user updates colors in Settings.
class SliceColorManager : public QObject {
    Q_OBJECT
public:
    static SliceColorManager& instance();

    // Returns the effective active/dim color for sliceId (% 8 applied).
    QColor activeColor(int sliceId) const;
    QColor dimColor(int sliceId) const;
    // Returns the hex string for CSS stylesheets (e.g. "#00d4ff").
    QString hexActive(int sliceId) const;

    // Whether custom colors are enabled (false = Aether defaults).
    bool useCustomColors() const { return m_useCustom; }
    void setUseCustomColors(bool enabled);

    // Get / set a custom base color for the given slice (0-7).
    QColor customColor(int sliceId) const;
    void setCustomColor(int sliceId, QColor color);

    // Persist current state to AppSettings.
    void save() const;
    // Load from AppSettings (called at startup).
    void load();

    // Reset a single slot to its default color.
    void resetToDefault(int sliceId);

signals:
    void colorsChanged();

private:
    SliceColorManager();

    bool m_useCustom{false};
    std::array<QColor, kSliceColorCount> m_customColors;
    std::array<QString, kSliceColorCount> m_hexCache;

    static int safeIdx(int sliceId);
    static QColor defaultActive(int idx);
    static QColor defaultDim(int idx);
    void rebuildHexCache();
};

} // namespace AetherSDR
