#pragma once

#include "BasemapStyle.h"
#include "core/ThemeManager.h"
#include <QGeoView/QGVLayerOSM.h>
#include <QGeoView/Raster/QGVImage.h>

namespace AetherSDR {
// Retain the shared original so toggling never compounds the transform or
// refetches a tile. Derived images live only as long as displayed QGV tiles.
class DarkBasemapTile final : public QGVImage {
public:
    explicit DarkBasemapTile(const QImage& original) : m_original(original) {}
    void setStyle(bool enabled, const QColor& background, const QColor& detail)
    {
        loadImage(enabled ? BasemapStyle::darkImage(m_original, background, detail)
                          : m_original);
    }
private:
    QImage m_original;
};

class DarkBasemapLayer final : public QGVLayerOSM {
public:
    DarkBasemapLayer()
    {
        updatePalette();
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, [this] {
                    updatePalette();
                    if (m_enabled) {
                        refreshTiles();
                    }
                });
    }
    void setDarkEnabled(bool enabled)
    {
        if (m_enabled == enabled) {
            return;
        }
        m_enabled = enabled;
        refreshTiles();
    }
protected:
    QGVImage* createTileImage(const QGV::GeoTilePos&, const QImage& image) override
    {
        auto* tile = new DarkBasemapTile(image);
        tile->setStyle(m_enabled, m_background, m_detail);
        return tile;
    }
private:
    void updatePalette()
    {
        ThemeManager& theme = ThemeManager::instance();
        m_background = theme.color(BasemapStyle::kBackgroundToken);
        m_detail = theme.color(BasemapStyle::kDetailToken);
    }
    void refreshTiles()
    {
        for (int i = 0; i < countItems(); ++i) {
            if (auto* tile = dynamic_cast<DarkBasemapTile*>(getItem(i))) {
                tile->setStyle(m_enabled, m_background, m_detail);
            }
        }
    }
    bool m_enabled{false};
    QColor m_background;
    QColor m_detail;
};
} // namespace AetherSDR
