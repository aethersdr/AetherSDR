#include "SliceColorManager.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {
// 'a' + idx → letter token suffix.  kSliceColorCount = 8 keeps idx in
// the 0..7 range so this never overflows past 'h'.
QString sliceLetterToken(int idx, bool dim)
{
    const QChar letter = QChar(QLatin1Char(char('a' + (idx % kSliceColorCount))));
    return dim
        ? QStringLiteral("color.slice.dim.%1").arg(letter)
        : QStringLiteral("color.slice.%1").arg(letter);
}
} // namespace

static SliceColorManager* s_instance = nullptr;

SliceColorManager& SliceColorManager::instance()
{
    if (!s_instance)
        s_instance = new SliceColorManager;
    return *s_instance;
}

SliceColorManager::SliceColorManager()
{
    for (int i = 0; i < kSliceColorCount; ++i)
        m_customColors[i] = defaultActive(i);
    rebuildHexCache();

    // When the user switches themes, the eight slice-default tokens
    // resolve to fresh hex values; rebuild the cache and notify every
    // listener (spectrum overlay paint, VFO badge, RxApplet header) so
    // they repaint with the new active/dim colours.  Custom-colour users
    // also get the notification — their stored hex stays untouched, but
    // anything that derives styling from the *defaults* still updates.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() {
        rebuildHexCache();
        emit colorsChanged();
    });
}

int SliceColorManager::safeIdx(int sliceId)
{
    // Defensive: callers normally pass 0..kSliceColorCount-1, but transient
    // sentinels (e.g. -1 during slice teardown) must not index out of range
    // or invoke implementation-defined behavior on negative-modulo.
    return std::abs(sliceId) % kSliceColorCount;
}

QColor SliceColorManager::defaultActive(int idx)
{
    return ThemeManager::instance().color(sliceLetterToken(idx, /*dim=*/false));
}

QColor SliceColorManager::defaultDim(int idx)
{
    return ThemeManager::instance().color(sliceLetterToken(idx, /*dim=*/true));
}

QColor SliceColorManager::activeColor(int sliceId) const
{
    int idx = safeIdx(sliceId);
    if (m_useCustom)
        return m_customColors[idx];
    return defaultActive(idx);
}

QColor SliceColorManager::dimColor(int sliceId) const
{
    int idx = safeIdx(sliceId);
    if (m_useCustom) {
        // Derive dim color at ~40% of the custom active color's value.
        QColor c = m_customColors[idx];
        return QColor(c.red() * 2 / 5, c.green() * 2 / 5, c.blue() * 2 / 5);
    }
    return defaultDim(idx);
}

QString SliceColorManager::hexActive(int sliceId) const
{
    return m_hexCache[safeIdx(sliceId)];
}

QColor SliceColorManager::customColor(int sliceId) const
{
    return m_customColors[safeIdx(sliceId)];
}

void SliceColorManager::setUseCustomColors(bool enabled)
{
    if (m_useCustom == enabled)
        return;
    m_useCustom = enabled;
    rebuildHexCache();
    save();
    emit colorsChanged();
}

void SliceColorManager::setCustomColor(int sliceId, QColor color)
{
    int idx = safeIdx(sliceId);
    if (m_customColors[idx] == color)
        return;
    m_customColors[idx] = color;
    rebuildHexCache();
    if (m_useCustom) {
        save();
        emit colorsChanged();
    }
}

void SliceColorManager::resetToDefault(int sliceId)
{
    int idx = safeIdx(sliceId);
    m_customColors[idx] = defaultActive(idx);
    rebuildHexCache();
    if (m_useCustom) {
        save();
        emit colorsChanged();
    }
}

void SliceColorManager::rebuildHexCache()
{
    for (int i = 0; i < kSliceColorCount; ++i)
        m_hexCache[i] = activeColor(i).name();
}

void SliceColorManager::save() const
{
    auto& s = AppSettings::instance();
    s.setValue("SliceColorsUseCustom", m_useCustom ? "True" : "False");
    for (int i = 0; i < kSliceColorCount; ++i) {
        s.setValue(QStringLiteral("SliceColor%1").arg(i),
                   m_customColors[i].name());
    }
    s.save();
}

void SliceColorManager::load()
{
    auto& s = AppSettings::instance();
    m_useCustom = s.value("SliceColorsUseCustom", "False").toString() == "True";
    for (int i = 0; i < kSliceColorCount; ++i) {
        QColor def = defaultActive(i);
        QString saved = s.value(QStringLiteral("SliceColor%1").arg(i),
                                def.name()).toString();
        QColor c(saved);
        m_customColors[i] = c.isValid() ? c : def;
    }
    rebuildHexCache();
}

} // namespace AetherSDR
