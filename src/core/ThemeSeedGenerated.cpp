// GENERATED FILE — DO NOT EDIT.
//
// Regenerate with:  python tools/gen_theme_seed.py
// Source of truth:  resources/themes/default-dark.json
//
// ThemeManager::seedBuiltinDefaults() used to be a hand-maintained copy of
// that JSON. It drifted (9 tokens) and was incomplete (24 tokens never
// seeded at all, resolving TRANSPARENT on themes that predate them) — see
// #3184. Generating it makes the resource authoritative by construction.
//
// `{primitive}` aliases are resolved to concrete values here on purpose: the
// seed runs before any primitives palette exists, and older user themes have
// no primitives section at all.

#include "ThemeManager.h"

#include <QColor>
#include <QPointF>
#include <QVariant>

namespace AetherSDR {

void ThemeManager::seedGeneratedDefaults()
{
    m_tokens.insert("color.accent", QString("#00b4d8"));
    m_tokens.insert("color.accent.bright", QString("#00c8f0"));
    m_tokens.insert("color.accent.danger", QString("#ff4d4d"));
    m_tokens.insert("color.accent.dim", QString("#0070c0"));
    m_tokens.insert("color.accent.success", QString("#4dd87a"));
    m_tokens.insert("color.accent.warning", QString("#ffb84d"));
    m_tokens.insert("color.background.0", QString("#0f0f1a"));
    m_tokens.insert("color.background.1", QString("#1a2a3a"));
    m_tokens.insert("color.background.2", QString("#304050"));
    m_tokens.insert("color.background.3", QString("#506070"));
    m_tokens.insert("color.background.app", QString("#0f0f1a"));
    m_tokens.insert("color.background.spectrum", QString("#000000"));
    m_tokens.insert("color.background.success", QString("#006040"));
    m_tokens.insert("color.background.tx", QString("#3a2a0e"));
    m_tokens.insert("color.background.warning", QString("#5a3a0a"));
    m_tokens.insert("color.border.accent", QString("#00b4d8"));
    m_tokens.insert("color.border.strong", QString("#2a3a4d"));
    m_tokens.insert("color.border.subtle", QString("#1a2330"));
    m_tokens.insert("color.border.tx", QString("#5a4a28"));
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 100.0;
        g.stops.append({0.0, QColor("#3aa7ff")});
        g.stops.append({0.42, QColor("#5de3ff")});
        g.stops.append({1.0, QColor("#8ef7e6")});
        m_tokens.insert("color.brand.gradient", QVariant::fromValue(g));
    }
    m_tokens.insert("color.brand.wordmark", QString("#eaf2fb"));
    m_tokens.insert("color.button.background.disabled", QString("#203040"));
    m_tokens.insert("color.button.border.disabled", QString("#304050"));
    m_tokens.insert("color.button.danger.background.disabled", QString("#2a1818"));
    m_tokens.insert("color.button.danger.border.disabled", QString("#4a2828"));
    m_tokens.insert("color.button.danger.foreground.disabled", QString("#8a6055"));
    m_tokens.insert("color.button.foreground.disabled", QString("#506070"));
    m_tokens.insert("color.canvas.background", QString("#08080d"));
    m_tokens.insert("color.canvas.dots", QString("#50e6f0fa"));
    m_tokens.insert("color.highlight.fg", QString("#000000"));
    m_tokens.insert("color.highlight.message", QString("#e58be5"));
    m_tokens.insert("color.highlight.rx", QString("#379baf"));
    m_tokens.insert("color.highlight.tx", QString("#fc4500"));
    m_tokens.insert("color.knob.background", QString("#1a2a3a"));
    m_tokens.insert("color.knob.background.disabled", QString("#1a2330"));
    m_tokens.insert("color.knob.foreground", QString("#0070c0"));
    m_tokens.insert("color.knob.foreground.disabled", QString("#3a4a5a"));
    m_tokens.insert("color.knob.handle", QString("#c8d8e8"));
    m_tokens.insert("color.knob.handle.disabled", QString("#506070"));
    m_tokens.insert("color.meter.bar.fill", QString("#405060"));
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 0.0;
        g.stops.append({0.0, QColor("#2f9e6a")});
        g.stops.append({0.55, QColor("#6cc56a")});
        g.stops.append({0.8, QColor("#e8b94c")});
        g.stops.append({0.95, QColor("#e8553c")});
        g.stops.append({1.0, QColor("#f2362a")});
        m_tokens.insert("color.meter.bar.fillGradient", QVariant::fromValue(g));
    }
    m_tokens.insert("color.meter.crst", QString("#ff4d4d"));
    m_tokens.insert("color.meter.gainReduction", QString("#f2c14e"));
    m_tokens.insert("color.meter.peak", QString("#e6f0fa"));
    m_tokens.insert("color.meter.pivot.fill", QString("#050509"));
    m_tokens.insert("color.meter.pivot.glow", QString("#ffb060"));
    m_tokens.insert("color.meter.pivot.rim", QString("#3a3e48"));
    m_tokens.insert("color.meter.rms", QString("#00b4d8"));
    m_tokens.insert("color.meter.thresh", QString("#ffb84d"));
    m_tokens.insert("color.slice.a", QString("#00d4ff"));
    m_tokens.insert("color.slice.b", QString("#ff40ff"));
    m_tokens.insert("color.slice.c", QString("#40ff40"));
    m_tokens.insert("color.slice.d", QString("#ffff00"));
    m_tokens.insert("color.slice.dim.a", QString("#006080"));
    m_tokens.insert("color.slice.dim.b", QString("#802080"));
    m_tokens.insert("color.slice.dim.c", QString("#208020"));
    m_tokens.insert("color.slice.dim.d", QString("#808000"));
    m_tokens.insert("color.slice.dim.e", QString("#805000"));
    m_tokens.insert("color.slice.dim.f", QString("#007060"));
    m_tokens.insert("color.slice.dim.g", QString("#803040"));
    m_tokens.insert("color.slice.dim.h", QString("#584080"));
    m_tokens.insert("color.slice.e", QString("#ffa000"));
    m_tokens.insert("color.slice.f", QString("#00e0c0"));
    m_tokens.insert("color.slice.g", QString("#ff6080"));
    m_tokens.insert("color.slice.h", QString("#b080ff"));
    m_tokens.insert("color.slice.tx", QString("#ff4d4d"));
    m_tokens.insert("color.slider.background", QString("#1a2a3a"));
    m_tokens.insert("color.slider.background.disabled", QString("#1a2330"));
    m_tokens.insert("color.slider.foreground", QString("#00b4d8"));
    m_tokens.insert("color.slider.foreground.disabled", QString("#3a4a5a"));
    m_tokens.insert("color.slider.handle", QString("#c8d8e8"));
    m_tokens.insert("color.slider.handle.disabled", QString("#506070"));
    m_tokens.insert("color.spectrum.average", QString("#8ea8c0"));
    m_tokens.insert("color.spectrum.grid", QString("#1a2330"));
    m_tokens.insert("color.spectrum.peakHold", QString("#ffb84d"));
    m_tokens.insert("color.spectrum.trace", QString("#00b4d8"));
    m_tokens.insert("color.text.disabled", QString("#3a4a5a"));
    m_tokens.insert("color.text.label", QString("#506070"));
    m_tokens.insert("color.text.primary", QString("#c8d8e8"));
    m_tokens.insert("color.text.secondary", QString("#8ea8c0"));
    m_tokens.insert("color.titlebar.background", QString("#eb0d1624"));
    m_tokens.insert("color.titlebar.border", QString("#2978bee6"));
    m_tokens.insert("color.titlebar.caption.close.glyph", QString("#ffffff"));
    m_tokens.insert("color.titlebar.caption.close.hover", QString("#c42b1c"));
    m_tokens.insert("color.titlebar.caption.glyph", QString("#8ea8c0"));
    m_tokens.insert("color.titlebar.caption.glyph.hover", QString("#e6f0fa"));
    m_tokens.insert("color.titlebar.caption.hover", QString("#14ffffff"));
    m_tokens.insert("color.titlebar.status.available", QString("#506070"));
    m_tokens.insert("color.titlebar.status.connected", QString("#4dd87a"));
    m_tokens.insert("color.titlebar.status.inUse", QString("#ffb84d"));
    m_tokens.insert("color.titlebar.tab.active.background", QString("#1a00b4d8"));
    m_tokens.insert("color.titlebar.tab.active.border", QString("#7300b4d8"));
    m_tokens.insert("color.titlebar.tab.hover", QString("#12ffffff"));
    m_tokens.insert("color.toggle.accent.background.checked", QString("#0070c0"));
    m_tokens.insert("color.toggle.accent.border.checked", QString("#00b4d8"));
    m_tokens.insert("color.toggle.accent.foreground.checked", QString("#00b4d8"));
    m_tokens.insert("color.toggle.background", QString("#1a2a3a"));
    m_tokens.insert("color.toggle.background.disabled", QString("#0f0f1a"));
    m_tokens.insert("color.toggle.border", QString("#304050"));
    m_tokens.insert("color.toggle.border.disabled", QString("#0f0f1a"));
    m_tokens.insert("color.toggle.foreground", QString("#c8d8e8"));
    m_tokens.insert("color.toggle.foreground.disabled", QString("#3a4a5a"));
    m_tokens.insert("color.toggle.success.background.checked", QString("#006040"));
    m_tokens.insert("color.toggle.success.border.checked", QString("#4dd87a"));
    m_tokens.insert("color.toggle.success.foreground.checked", QString("#4dd87a"));
    m_tokens.insert("color.toggle.warning.background.checked", QString("#5a3a0a"));
    m_tokens.insert("color.toggle.warning.border.checked", QString("#ffb84d"));
    m_tokens.insert("color.toggle.warning.foreground.checked", QString("#ffb84d"));
    m_tokens.insert("color.tx.mox.border", QString("#d08020"));
    m_tokens.insert("color.tx.mox.border.hover", QString("#e09030"));
    m_tokens.insert("color.tx.mox.text", QString("#f0c890"));
    m_tokens.insert("color.tx.mox.text.hover", QString("#ffd8a0"));
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({0.25, QColor("#001e78")});
        g.stops.append({0.5, QColor("#0064b4")});
        g.stops.append({0.75, QColor("#00c882")});
        g.stops.append({1.0, QColor("#dcffdc")});
        m_tokens.insert("color.waterfall.colormap.blueGreen", QVariant::fromValue(g));
    }
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({0.15, QColor("#000080")});
        g.stops.append({0.3, QColor("#0040ff")});
        g.stops.append({0.45, QColor("#00c8ff")});
        g.stops.append({0.6, QColor("#00dc00")});
        g.stops.append({0.8, QColor("#ffff00")});
        g.stops.append({1.0, QColor("#ff0000")});
        m_tokens.insert("color.waterfall.colormap.default", QVariant::fromValue(g));
    }
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({0.25, QColor("#800000")});
        g.stops.append({0.5, QColor("#dc5000")});
        g.stops.append({0.75, QColor("#ffc800")});
        g.stops.append({1.0, QColor("#ffffdc")});
        m_tokens.insert("color.waterfall.colormap.fire", QVariant::fromValue(g));
    }
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({1.0, QColor("#ffffff")});
        m_tokens.insert("color.waterfall.colormap.grayscale", QVariant::fromValue(g));
    }
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({0.25, QColor("#50008c")});
        g.stops.append({0.5, QColor("#c80078")});
        g.stops.append({0.75, QColor("#f07800")});
        g.stops.append({1.0, QColor("#ffff50")});
        m_tokens.insert("color.waterfall.colormap.plasma", QVariant::fromValue(g));
    }
    {
        ThemeGradient g;
        g.type = ThemeGradient::Linear;
        g.angle = 180.0;
        g.stops.append({0.0, QColor("#000000")});
        g.stops.append({0.15, QColor("#0000ff")});
        g.stops.append({0.3, QColor("#00ff00")});
        g.stops.append({0.45, QColor("#ffff00")});
        g.stops.append({0.6, QColor("#ff0000")});
        g.stops.append({0.75, QColor("#800080")});
        g.stops.append({1.0, QColor("#ffffff")});
        m_tokens.insert("color.waterfall.colormap.purple", QVariant::fromValue(g));
    }
    m_tokens.insert("color.waterfall.history", QString("#506070"));
    m_tokens.insert("color.waterfall.live", QString("#ff4d4d"));
    {
        ThemeFont f;
        f.family = QStringLiteral("DSEG7 Modern");
        f.size = 20;
        f.color = QColor("#00b4d8");
        m_tokens.insert("font.family.freq", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("monospace");
        f.size = 12;
        f.color = QColor("#c8d8e8");
        m_tokens.insert("font.family.mono", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("DSEG14 Modern");
        f.size = 14;
        f.color = QColor("#00b4d8");
        m_tokens.insert("font.family.segment14", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("DSEG7 Modern");
        f.size = 14;
        f.color = QColor("#00b4d8");
        m_tokens.insert("font.family.segment7", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("DSEG7 Modern");
        f.size = 12;
        f.color = QColor("#8ea8c0");
        m_tokens.insert("font.family.temp", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("Inter");
        f.size = 12;
        f.color = QColor("#c8d8e8");
        m_tokens.insert("font.family.ui", QVariant::fromValue(f));
    }
    {
        ThemeFont f;
        f.family = QStringLiteral("DSEGWeather");
        f.size = 14;
        f.color = QColor("#c8d8e8");
        m_tokens.insert("font.family.weather", QVariant::fromValue(f));
    }
    m_tokens.insert("font.size.large", 14);
    m_tokens.insert("font.size.normal", 12);
    m_tokens.insert("font.size.small", 10);
    m_tokens.insert("font.size.tiny", 9);
    m_tokens.insert("sizing.border.strong", 2);
    m_tokens.insert("sizing.border.subtle", 1);
    m_tokens.insert("sizing.panel.cornerRadius", 4);
    m_tokens.insert("sizing.panel.padding", 4);
    m_tokens.insert("sizing.panel.spacing", 4);

    // scope: applet/comp
    seedScopedToken(QStringLiteral("applet/comp"), "color.knob.foreground", QString("#ffb84d"));
    seedScopedToken(QStringLiteral("applet/comp"), "color.slider.foreground", QString("#ffb84d"));
    seedScopedToken(QStringLiteral("applet/comp"), "color.toggle.accent.background.checked", QString("#ffb84d"));

    // scope: applet/rx
    seedScopedToken(QStringLiteral("applet/rx"), "color.knob.foreground", QString("#4dd87a"));
    seedScopedToken(QStringLiteral("applet/rx"), "color.slider.foreground", QString("#4dd87a"));
    seedScopedToken(QStringLiteral("applet/rx"), "color.toggle.accent.background.checked", QString("#4dd87a"));

    // scope: applet/tx
    seedScopedToken(QStringLiteral("applet/tx"), "color.knob.foreground", QString("#ff4d4d"));
    seedScopedToken(QStringLiteral("applet/tx"), "color.slider.foreground", QString("#ff4d4d"));
    seedScopedToken(QStringLiteral("applet/tx"), "color.toggle.accent.background.checked", QString("#ff4d4d"));
}

} // namespace AetherSDR
