#include "WeatherRadarLegend.h"
#include "core/ThemeManager.h"
#include "core/weather/OperaRadarImage.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
class PaletteBar final : public QWidget {
public:
    PaletteBar(const QJsonArray& entries, QWidget* parent)
        : QWidget(parent), m_entries(entries)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return {240, fontMetrics().height() + 8}; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_entries.isEmpty()) { return; }
        QPainter painter(this);
        const int margin = fontMetrics().horizontalAdvance(QStringLiteral("200")) / 2;
        const double span = std::max(1, width() - 2 * margin);
        const double step = span / m_entries.size();
        for (int i = 0; i < m_entries.size(); ++i) {
            const QJsonObject entry = m_entries[i].toObject();
            const double x = margin + i * step;
            QLinearGradient gradient(x, 0, x + step, 0);
            // These are measurement palette data, never theme UI colors.
            gradient.setColorAt(0, QColor(entry.value(QStringLiteral("start")).toString()));
            gradient.setColorAt(1, QColor(entry.value(QStringLiteral("end")).toString()));
            painter.fillRect(QRectF(x, 0, step + 0.5, 4), gradient);
            const QString label = entry.value(QStringLiteral("label")).toString();
            if (!label.isEmpty()) {
                painter.setPen(ThemeManager::instance().color(this, "color.text.secondary"));
                painter.drawText(QPointF(x - fontMetrics().horizontalAdvance(label) / 2.0,
                    7 + fontMetrics().ascent()), label);
            }
        }
    }
private:
    QJsonArray m_entries;
};
}

WeatherRadarLegend::WeatherRadarLegend(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("pskReporterRadarLegend"));
    setAccessibleName(tr("Displayed weather sources and separate intensity scales"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    ThemeManager::instance().applyStyleSheet(this,
        "QFrame#pskReporterRadarLegend { background: {{color.background.1}}; border: none; }"
        "QWidget { font-size: 11px; background: transparent; border: none; }"
        "QLabel { color: {{color.text.secondary}}; background: transparent; border: none; }");
    QFile file(QStringLiteral(":/radar/legends.json"));
    QJsonObject palettes;
    if (file.open(QIODevice::ReadOnly)) {
        palettes = QJsonDocument::fromJson(file.readAll()).object();
    }
    QJsonArray opera;
    for (int dbz = -30; dbz < 80; ++dbz) {
        opera.append(QJsonObject{
            {QStringLiteral("label"), dbz % 20 == 0 ? QString::number(dbz) : QString()},
            {QStringLiteral("start"), OperaRadarImage::colorForDbz(dbz).name()},
            {QStringLiteral("end"), OperaRadarImage::colorForDbz(dbz + 1).name()}});
    }
    const std::array<QString, 4> titles{
        tr("NOAA · United States · dBZ"), tr("ECCC · Canada · mm/h"),
        tr("OPERA · Europe · dBZ"), tr("LibreWXR · Global · dBZ equivalent")};
    const std::array<QJsonArray, 4> scales{
        palettes.value(QStringLiteral("noaa")).toArray(),
        palettes.value(QStringLiteral("eccc")).toArray(), opera,
        palettes.value(QStringLiteral("libre")).toArray()};
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(7);
    for (int i = 0; i < 4; ++i) {
        m_rows[i] = new QWidget(this);
        auto* row = new QVBoxLayout(m_rows[i]);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        auto* title = new QLabel(titles[i], m_rows[i]);
        title->setObjectName(QStringLiteral("pskReporterRadarSourceLabel%1").arg(i));
        title->setAccessibleName(titles[i]);
        row->addWidget(title);
        row->addWidget(new PaletteBar(scales[i], m_rows[i]));
        if (i == 3) {
            auto* note = new QLabel(tr("Radar, satellite estimates and model data"), m_rows[i]);
            row->addWidget(note);
        }
        layout->addWidget(m_rows[i]);
    }
    setProviders(0);
}

void WeatherRadarLegend::setProviders(int providers)
{
    m_providers = providers;
    for (int i = 0; i < 4; ++i) { m_rows[i]->setVisible(providers & (1 << i)); }
    setVisible(m_legendVisible && providers != 0);
    adjustSize();
    raise();
}

void WeatherRadarLegend::setLegendVisible(bool visible)
{
    m_legendVisible = visible;
    setVisible(visible && m_providers != 0);
    adjustSize();
    raise();
}
}
