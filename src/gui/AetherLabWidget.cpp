#include "AetherLabWidget.h"
#include "CanonIndicators.h"

#include "core/ThemeManager.h"
#include "models/AntennaAnalyzerModel.h"

#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

class AetherLabPlot final : public QWidget {
    Q_OBJECT
public:
    explicit AetherLabPlot(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("aetherLabPlot"));
        setAccessibleName(tr("Antenna SWR plot"));
        setMinimumSize(360, 260);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
    }
    void setSamples(const QJsonArray& samples, qint64 start, qint64 stop, int marker)
    {
        m_samples = samples;
        m_start = start;
        m_stop = stop;
        m_marker = marker;
        const QString description = tr("%1 measured rows. SWR display spans 1 to 5; higher values are clipped. Use the marker slider to read each sample.").arg(samples.size());
        if (accessibleDescription() != description) {
            setAccessibleDescription(description);
            QAccessibleValueChangeEvent event(this, description);
            QAccessible::updateAccessibility(&event);
        }
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ThemeManager& theme = ThemeManager::instance();
        p.fillRect(rect(), theme.color(this, QStringLiteral("color.canon.nested")));
        const QRectF chart(46, 24, width() - 88, height() - 68);
        for (int swr = 1; swr <= 5; ++swr) {
            const qreal y = chart.bottom() - (swr - 1) * chart.height() / 4;
            p.setPen(theme.color(this, QStringLiteral("color.canon.lineHi")));
            p.drawLine(QPointF(chart.left(), y), QPointF(chart.right(), y));
            p.setPen(theme.color(this, QStringLiteral("color.canon.muted")));
            p.drawText(QRectF(0, y - 8, 38, 18), Qt::AlignRight, QString::number(swr));
        }
        p.drawText(QRectF(0, 2, 40, 18), Qt::AlignRight, tr("SWR"));
        for (int tick = 0; tick <= 4; ++tick) {
            const qreal x = chart.left() + tick * chart.width() / 4;
            p.setPen(theme.color(this, QStringLiteral("color.canon.lineHi")));
            p.drawLine(QPointF(x, chart.top()), QPointF(x, chart.bottom()));
            p.setPen(theme.color(this, QStringLiteral("color.canon.muted")));
            const double mhz = (m_start + (m_stop - m_start) * tick / 4.0) / 1e6;
            p.drawText(QRectF(x - 34, chart.bottom() + 8, 68, 20), Qt::AlignCenter, QString::number(mhz, 'f', 3));
        }
        p.drawText(QRectF(chart.left(), height() - 22, chart.width(), 18), Qt::AlignCenter, tr("Frequency (MHz)"));
        if (m_samples.isEmpty()) {
            p.drawText(chart, Qt::AlignCenter, tr("Connect a Stick Pro, then run an explicit sweep"));
            return;
        }
        QPainterPath path;
        bool continuation = false;
        QPointF marker;
        bool markerValid = false;
        for (qsizetype i = 0; i < m_samples.size(); ++i) {
            const QJsonObject sample = m_samples.at(i).toObject();
            const QJsonValue value = sample.value(QStringLiteral("swr"));
            const bool reflected = sample.value(QStringLiteral("quality")).toString() == QLatin1String("total-reflection");
            if ((!value.isDouble() && !reflected) || m_stop <= m_start) { continuation = false; continue; }
            const double swr = reflected ? 5 : std::clamp(value.toDouble(), 1.0, 5.0);
            const QPointF point(chart.left() + chart.width() *
                (sample.value(QStringLiteral("frequencyHz")).toDouble() - m_start) / (m_stop - m_start),
                chart.bottom() - chart.height() * (swr - 1) / 4);
            if (continuation) { path.lineTo(point); } else { path.moveTo(point); }
            continuation = true;
            if (i == m_marker) { marker = point; markerValid = true; }
        }
        p.setPen(QPen(theme.color(this, QStringLiteral("color.canon.cyan")), 2));
        p.drawPath(path);
        if (markerValid) {
            p.setPen(QPen(theme.color(this, QStringLiteral("color.canon.aqua")), 1, Qt::DashLine));
            p.drawLine(QPointF(marker.x(), chart.top()), QPointF(marker.x(), chart.bottom()));
            p.setBrush(theme.color(this, QStringLiteral("color.canon.aqua")));
            p.drawEllipse(marker, 4, 4);
        }
    }
private:
    QJsonArray m_samples;
    qint64 m_start{14000000};
    qint64 m_stop{14350000};
    int m_marker{0};
};

class AetherLabPlotAccessible final : public QAccessibleWidget {
public:
    explicit AetherLabPlotAccessible(AetherLabPlot* plot) : QAccessibleWidget(plot, QAccessible::Graphic) {}
    QString text(QAccessible::Text role) const override
    {
        if (role == QAccessible::Value) { return widget()->accessibleDescription(); }
        return QAccessibleWidget::text(role);
    }
};

namespace {
QAccessibleInterface* labAccessibility(const QString&, QObject* object)
{
    if (AetherLabPlot* plot = qobject_cast<AetherLabPlot*>(object)) { return new AetherLabPlotAccessible(plot); }
    return nullptr;
}
void availability(QWidget* widget, bool enabled, const QString& reason)
{
    widget->setEnabled(enabled);
    widget->setToolTip(enabled ? QString() : reason);
    widget->setAccessibleDescription(enabled ? QString() : reason);
}
QString analyzerStatusLabel(AntennaAnalyzerModel::State state)
{
    switch (state) {
    case AntennaAnalyzerModel::State::Disconnected: return QCoreApplication::translate("AetherLabWidget", "Disconnected");
    case AntennaAnalyzerModel::State::Identifying: return QCoreApplication::translate("AetherLabWidget", "Identifying");
    case AntennaAnalyzerModel::State::Ready: return QCoreApplication::translate("AetherLabWidget", "Ready");
    case AntennaAnalyzerModel::State::SettingFrequency: return QCoreApplication::translate("AetherLabWidget", "Setting frequency");
    case AntennaAnalyzerModel::State::SettingWidth: return QCoreApplication::translate("AetherLabWidget", "Setting width");
    case AntennaAnalyzerModel::State::Sweeping: return QCoreApplication::translate("AetherLabWidget", "Sweeping");
    case AntennaAnalyzerModel::State::Closing: return QCoreApplication::translate("AetherLabWidget", "Closing");
    case AntennaAnalyzerModel::State::Complete: return QCoreApplication::translate("AetherLabWidget", "Complete");
    case AntennaAnalyzerModel::State::Cancelled: return QCoreApplication::translate("AetherLabWidget", "Cancelled");
    case AntennaAnalyzerModel::State::Error: return QCoreApplication::translate("AetherLabWidget", "Error");
    }
    return {};
}

void padSweepEditor(QAbstractSpinBox* spin, const QString& widestText)
{
    spin->setMinimumHeight(40);
    spin->setMinimumWidth(spin->fontMetrics().horizontalAdvance(widestText) + 40);
    if (QLineEdit* editor = spin->findChild<QLineEdit*>()) {
        editor->setTextMargins(8, 2, 8, 2);
    }
}

QString number(const QJsonObject& sample, const QString& key, int precision)
{
    const QJsonValue value = sample.value(key);
    if (value.isDouble()) {
        return QString::number(value.toDouble(), std::abs(value.toDouble()) >= 1e6 ? 'g' : 'f',
                               std::abs(value.toDouble()) >= 1e6 ? 6 : precision);
    }
    const QString quality = sample.value(QStringLiteral("quality")).toString();
    if ((key == QLatin1String("swr") && quality == QLatin1String("total-reflection"))
        || (key == QLatin1String("returnLossDb") && quality == QLatin1String("perfect-match"))) {
        return QStringLiteral("∞");
    }
    return QCoreApplication::translate("AetherLabWidget", "undefined");
}
QString engineering(double value, const QString& unit)
{
    if (!std::isfinite(value)) { return QCoreApplication::translate("AetherLabWidget", "undefined"); }
    const double magnitude = std::abs(value);
    const QList<QPair<double, QString>> prefixes{{1e9, "G"}, {1e6, "M"}, {1e3, "k"},
        {1, ""}, {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}};
    if (value == 0) { return QStringLiteral("0 %1").arg(unit); }
    for (const auto& prefix : prefixes) {
        if (magnitude >= prefix.first || prefix.first == 1e-12) {
            return QStringLiteral("%1 %2%3").arg(QString::number(value / prefix.first, magnitude / prefix.first >= 10000 ? 'g' : 'f', magnitude / prefix.first >= 10000 ? 6 : 2), prefix.second, unit);
        }
    }
    return {};
}
} // namespace

AetherLabWidget::AetherLabWidget(AntennaAnalyzerModel* model, QWidget* parent)
    : CanonCanvas(parent), m_model(model)
{
    static const bool installed = [] { QAccessible::installFactory(labAccessibility); return true; }();
    Q_UNUSED(installed);
    setObjectName(QStringLiteral("aetherLabWorkspace"));
    setAccessibleName(tr("AetherLab antenna analyzer workspace"));
    setFocusPolicy(Qt::StrongFocus);
    setCloseButtonVisible(false);
    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(bodyWidget());
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("aetherLabContent"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    m_heading = new QBoxLayout(QBoxLayout::LeftToRight);
    m_heading->setSpacing(12);
    auto* identity = new QWidget(content);
    auto* identityRow = new QHBoxLayout(identity);
    identityRow->setContentsMargins(0, 0, 0, 0);
    identityRow->setSpacing(12);
    auto* title = new QLabel(tr("AetherLab"), identity);
    title->setProperty("labHeading", true);
    identityRow->addWidget(title);
    auto* subtitle = new QLabel(tr("Antenna Quick Check"), identity);
    subtitle->setObjectName(QStringLiteral("aetherLabSubtitle"));
    subtitle->setProperty("labMuted", true);
    identityRow->addWidget(subtitle);
    identityRow->addStretch();
    m_heading->addWidget(identity, 1);
    auto* actions = new QWidget(content);
    auto* actionsRow = new QHBoxLayout(actions);
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setSpacing(8);
    auto* warning = new QLabel(tr("Radio TX is not inhibited"), actions);
    warning->setObjectName(QStringLiteral("aetherLabIsolationWarning"));
    warning->setAccessibleName(tr("Radio TX is not inhibited"));
    warning->setAccessibleDescription(tr("Isolate the analyzer RF port from transmitters. USB close does not confirm RF silence."));
    warning->setToolTip(warning->accessibleDescription());
    actionsRow->addWidget(warning);
    auto* returnButton = new QPushButton(tr("Return to radio"), actions);
    returnButton->setObjectName(QStringLiteral("aetherLabReturn"));
    returnButton->setAccessibleName(tr("Return to radio and stop analyzer session"));
    actionsRow->addWidget(returnButton);
    m_heading->addWidget(actions, 0, Qt::AlignRight);
    layout->addLayout(m_heading);
    const auto card = [content](const QString& title) {
        auto* frame = new QFrame(content);
        frame->setProperty("labCard", true);
        auto* box = new QVBoxLayout(frame);
        box->setContentsMargins(16, 12, 16, 12);
        box->setSpacing(12);
        auto* label = new QLabel(title, frame);
        label->setProperty("labSection", true);
        box->addWidget(label);
        return frame;
    };
    m_controls = new QBoxLayout(QBoxLayout::TopToBottom);
    m_controls->setSpacing(12);
    auto* connection = card(tr("STICK PRO · USB"));
    auto* devicesRow = new QHBoxLayout;
    m_devices = new QComboBox(connection);
    m_devices->setMinimumContentsLength(14);
    m_devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_devices->setObjectName(QStringLiteral("aetherLabDevice"));
    m_devices->setAccessibleName(tr("Stick Pro USB interface"));
    m_devices->setPlaceholderText(tr("Refresh USB to find a Stick Pro"));
    devicesRow->addWidget(m_devices, 1);
    m_rescan = new QPushButton(tr("Refresh USB"), connection);
    m_rescan->setAccessibleName(tr("Refresh Stick Pro USB devices"));
    devicesRow->addWidget(m_rescan);
    m_connect = new QPushButton(tr("Connect"), connection);
    m_connect->setObjectName(QStringLiteral("aetherLabConnect"));
    m_connect->setAccessibleName(tr("Connect Stick Pro"));
    devicesRow->addWidget(m_connect);
    m_disconnect = new QPushButton(tr("Disconnect"), connection);
    m_disconnect->setAccessibleName(tr("Disconnect Stick Pro"));
    devicesRow->addWidget(m_disconnect);
    static_cast<QVBoxLayout*>(connection->layout())->addLayout(devicesRow);
    m_status = new QLabel(connection);
    m_status->setObjectName(QStringLiteral("aetherLabStatus"));
    m_status->setAccessibleName(tr("Analyzer session status"));
    m_status->setWordWrap(true);
    connection->layout()->addWidget(m_status);
    auto* guidance = new QLabel(tr("Isolate the analyzer RF port from transmitters. Stop AntScope before connecting."), connection);
    guidance->setObjectName(QStringLiteral("aetherLabIsolationGuidance"));
    guidance->setWordWrap(true);
    guidance->setAccessibleName(tr("Analyzer RF isolation guidance"));
    guidance->setProperty("labMuted", true);
    connection->layout()->addWidget(guidance);
    m_controls->addWidget(connection, 1);

    auto* sweepCard = card(tr("SWEEP RANGE"));
    auto* controls = new QGridLayout;
    controls->setHorizontalSpacing(12);
    controls->setVerticalSpacing(4);
    const auto frequency = [sweepCard](const QString& name, double value) {
        auto* spin = new QDoubleSpinBox(sweepCard);
        spin->setDecimals(6);
        spin->setRange(0.1, 600.0);
        spin->setValue(value);
        spin->setSuffix(tr(" MHz"));
        padSweepEditor(spin, QStringLiteral("600.000000 MHz"));
        spin->setAccessibleName(name);
        spin->setAccessibleDescription(tr("Frequency in MHz, 0.1 to 600, with 1 Hz entry resolution"));
        return spin;
    };
    m_start = frequency(tr("Sweep start frequency"), 14.0);
    m_start->setObjectName(QStringLiteral("aetherLabStartMHz"));
    m_stop = frequency(tr("Sweep stop frequency"), 14.35);
    m_stop->setObjectName(QStringLiteral("aetherLabStopMHz"));
    m_intervals = new QSpinBox(sweepCard);
    m_intervals->setRange(1, 1000);
    m_intervals->setValue(200);
    padSweepEditor(m_intervals, QStringLiteral("1000"));
    m_intervals->setObjectName(QStringLiteral("aetherLabIntervals"));
    m_intervals->setAccessibleName(tr("FRX sweep intervals"));
    m_intervals->setAccessibleDescription(tr("FRX N count. N plus 1 rows expected. Host cap is 1000."));
    const QList<QPair<QString, QWidget*>> fields{{tr("Start"), m_start}, {tr("Stop"), m_stop}, {tr("Intervals"), m_intervals}};
    for (int i = 0; i < fields.size(); ++i) {
        auto* label = new QLabel(fields.at(i).first, sweepCard);
        label->setBuddy(fields.at(i).second);
        controls->addWidget(label, 0, i);
        controls->addWidget(fields.at(i).second, 1, i);
        controls->setColumnStretch(i, i < 2 ? 2 : 1);
    }
    m_sweep = new QPushButton(tr("Run sweep"), sweepCard);
    m_sweep->setObjectName(QStringLiteral("aetherLabSweep"));
    m_sweep->setAccessibleName(tr("Sweep antenna with Stick Pro"));
    m_sweep->setProperty("aetherLabRf", true);
    m_cancel = new QPushButton(tr("Stop"), sweepCard);
    m_cancel->setObjectName(QStringLiteral("aetherLabStop"));
    m_cancel->setAccessibleName(tr("Stop analyzer sweep"));
    controls->addWidget(m_sweep, 1, 3);
    controls->addWidget(m_cancel, 1, 4);
    static_cast<QVBoxLayout*>(sweepCard->layout())->addLayout(controls);
    m_isolated = new QCheckBox(tr("Analyzer RF port is isolated from transmitters"), sweepCard);
    m_isolated->setObjectName(QStringLiteral("aetherLabIsolated"));
    m_isolated->setAccessibleName(tr("Confirm analyzer RF isolation"));
    sweepCard->layout()->addWidget(m_isolated);
    m_controls->addWidget(sweepCard, 1);
    layout->addLayout(m_controls);

    m_analysis = new QBoxLayout(QBoxLayout::LeftToRight);
    m_analysis->setSpacing(12);
    auto* plotCard = card(tr("SWR · 50 Ω REFERENCE"));
    m_plot = new AetherLabPlot(plotCard);
    plotCard->layout()->addWidget(m_plot);
    static_cast<QVBoxLayout*>(plotCard->layout())->setStretch(1, 1);
    m_marker = new QSlider(Qt::Horizontal, plotCard);
    m_marker->setObjectName(QStringLiteral("aetherLabMarker"));
    m_marker->setAccessibleName(tr("Measurement marker sample"));
    plotCard->layout()->addWidget(m_marker);
    m_analysis->addWidget(plotCard, 3);
    auto* markerCard = card(tr("MARKER 1"));
    markerCard->setMinimumWidth(376);
    markerCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_frequency = new QLabel(tr("No measurement"), markerCard);
    m_frequency->setObjectName(QStringLiteral("aetherLabMarkerFrequency"));
    m_frequency->setProperty("labFrequency", true);
    m_frequency->setAccessibleName(tr("Marker frequency"));
    markerCard->layout()->addWidget(m_frequency);
    const auto metric = [this](QGridLayout* grid, int row, int column, const QString& label,
                               const QString& key, const QString& description = QString()) {
        auto* cell = new QWidget;
        cell->setProperty("labMetric", true);
        auto* box = new QVBoxLayout(cell);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(6);
        box->setAlignment(Qt::AlignTop);
        auto* caption = new QLabel(label, cell);
        caption->setProperty("labMuted", true);
        box->addWidget(caption);
        auto* value = new QLabel(QStringLiteral("—"), cell);
        value->setObjectName(QStringLiteral("aetherLabValue_") + key);
        value->setAccessibleName(label);
        value->setAccessibleDescription(description);
        value->setToolTip(description);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setProperty("labValue", true);
        box->addWidget(value);
        m_values.insert(key, value);
        grid->addWidget(cell, row, column);
        grid->setColumnStretch(column, 1);
    };
    auto* markerMetrics = new QGridLayout;
    markerMetrics->setHorizontalSpacing(24);
    markerMetrics->setVerticalSpacing(28);
    for (int row = 0; row < 3; ++row) { markerMetrics->setRowStretch(row, 1); }
    metric(markerMetrics, 0, 0, tr("SWR"), QStringLiteral("swr"));
    m_values.value(QStringLiteral("swr"))->setProperty("labPrimary", true);
    metric(markerMetrics, 0, 1, tr("Return loss"), QStringLiteral("returnLossDb"));
    metric(markerMetrics, 1, 0, tr("|Z|"), QStringLiteral("impedanceMagnitudeOhm"));
    metric(markerMetrics, 1, 1, tr("Phase ∠S11"), QStringLiteral("reflectionPhaseDeg"), tr("Reflection coefficient phase at 50 Ω; undefined for a perfect match."));
    metric(markerMetrics, 2, 0, tr("Magnitude |S11|"), QStringLiteral("reflectionMagnitude"));
    metric(markerMetrics, 2, 1, tr("Phase ∠Z"), QStringLiteral("impedancePhaseDeg"), tr("Impedance phase, distinct from reflection phase."));
    static_cast<QVBoxLayout*>(markerCard->layout())->addLayout(markerMetrics, 1);
    m_quality = new QLabel(tr("Run a sweep to place a marker"), markerCard);
    m_quality->setWordWrap(true);
    m_quality->setObjectName(QStringLiteral("aetherLabMarkerQuality"));
    m_quality->setProperty("labMuted", true);
    markerCard->layout()->addWidget(m_quality);
    m_analysis->addWidget(markerCard, 1);
    layout->addLayout(m_analysis, 1);

    m_models = new QBoxLayout(QBoxLayout::LeftToRight);
    m_models->setSpacing(12);
    auto* series = card(tr("SERIES MODEL"));
    auto* seriesMetrics = new QGridLayout;
    seriesMetrics->setHorizontalSpacing(20);
    metric(seriesMetrics, 0, 0, tr("R"), QStringLiteral("rOhm"));
    metric(seriesMetrics, 0, 1, tr("X"), QStringLiteral("xOhm"));
    metric(seriesMetrics, 1, 0, tr("L"), QStringLiteral("seriesInductanceH"));
    metric(seriesMetrics, 1, 1, tr("C"), QStringLiteral("seriesCapacitanceF"));
    static_cast<QVBoxLayout*>(series->layout())->addLayout(seriesMetrics);
    auto* parallel = card(tr("PARALLEL MODEL"));
    auto* parallelMetrics = new QGridLayout;
    parallelMetrics->setHorizontalSpacing(20);
    metric(parallelMetrics, 0, 0, tr("R"), QStringLiteral("parallelResistanceOhm"));
    metric(parallelMetrics, 0, 1, tr("X"), QStringLiteral("parallelReactanceOhm"));
    metric(parallelMetrics, 1, 0, tr("L"), QStringLiteral("parallelInductanceH"));
    metric(parallelMetrics, 1, 1, tr("C"), QStringLiteral("parallelCapacitanceF"));
    static_cast<QVBoxLayout*>(parallel->layout())->addLayout(parallelMetrics);
    m_models->addWidget(series, 1);
    m_models->addWidget(parallel, 1);
    layout->addLayout(m_models);
    auto* equivalents = new QLabel(tr("Signed L/C equivalents at the marker frequency. Negative L indicates capacitive reactance; negative C indicates inductive reactance. Undefined values are not finite at this operating point."), content);
    equivalents->setWordWrap(true);
    equivalents->setProperty("labMuted", true);
    layout->addWidget(equivalents);
    auto* footer = new QHBoxLayout;
    m_minimum = new QPushButton(tr("Minimum SWR marker"), content);
    m_minimum->setAccessibleName(tr("Select minimum measured SWR"));
    footer->addWidget(m_minimum);
    m_save = new QPushButton(tr("Save session…"), content);
    m_save->setAccessibleName(tr("Save analyzer samples and protocol trace"));
    footer->addWidget(m_save);
    footer->addStretch();
    auto* qualification = new QLabel(tr("Device-corrected R/X · USB closes after each sweep"), content);
    qualification->setProperty("labMuted", true);
    qualification->setWordWrap(true);
    footer->addWidget(qualification);
    layout->addLayout(footer);
    applyCanonSheet(content, [] { return QStringLiteral(
        "QWidget#aetherLabContent, QWidget[labMetric=true] { background: transparent; }"
        "QLabel, QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; }"
        "QFrame[labCard=true] { background: {{color.canon.nested}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 8px; }"
        "QLabel[labHeading=true] { font-size: 26px; font-weight: 600; color: {{color.canon.ink}}; }"
        "QLabel[labSection=true] { color: {{color.canon.muted}}; font-size: 11px; font-weight: 600; }"
        "QLabel[labMuted=true] { color: {{color.canon.muted}}; }"
        "QLabel[labValue=true] { font-family: 'JetBrains Mono', monospace; font-size: 24px; color: {{color.canon.ink}}; }"
        "QLabel[labFrequency=true] { font-family: 'JetBrains Mono', monospace; font-size: 24px; color: {{color.canon.ink}}; }"
        "QLabel[labPrimary=true] { color: {{color.canon.cyan}}; }"
        "QLabel#aetherLabIsolationWarning { color: {{color.toggle.warning.foreground.checked}}; background: {{color.toggle.warning.background.checked}}; border: 1px solid {{color.toggle.warning.border.checked}}; border-radius: 5px; padding: 6px 10px; }"
        "QPushButton, QComboBox, QDoubleSpinBox, QSpinBox { color: {{color.canon.ink}}; background: {{color.canon.control}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 5px; padding: 6px 8px; }"
        "QPushButton:hover, QPushButton:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border-color: {{color.canon.cyan}}; }"
        "QPushButton:disabled { color: {{color.button.foreground.disabled}}; background: {{color.button.background.disabled}}; border-color: {{color.button.border.disabled}}; }"
        "QPushButton#aetherLabSweep:enabled { color: {{color.canon.cyan}}; border-color: {{color.canon.cyan}}; }") + canonIndicatorRules(); });
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &AetherLabWidget::refresh);
    connect(model, &AntennaAnalyzerModel::changed, this, [this] {
        if (!m_refreshTimer->isActive()) { m_refreshTimer->start(); }
    });
    connect(m_rescan, &QPushButton::clicked, model, &AntennaAnalyzerModel::refreshDevices);
    connect(m_connect, &QPushButton::clicked, this, [this] { m_model->connectDevice(m_devices->currentData().toInt()); });
    connect(m_disconnect, &QPushButton::clicked, model, &AntennaAnalyzerModel::disconnectDevice);
    connect(m_sweep, &QPushButton::clicked, this, [this] {
        m_model->sweep(std::llround(m_start->value() * 1e6), std::llround(m_stop->value() * 1e6),
                       m_intervals->value(), m_isolated->isChecked());
    });
    connect(m_cancel, &QPushButton::clicked, model, &AntennaAnalyzerModel::stop);
    connect(m_isolated, &QCheckBox::toggled, this, &AetherLabWidget::refresh);
    connect(m_marker, &QSlider::valueChanged, this, &AetherLabWidget::refresh);
    connect(m_minimum, &QPushButton::clicked, this, [this] {
        const QJsonArray samples = m_model->snapshot().value(QStringLiteral("samples")).toArray();
        double best = INFINITY;
        int index = 0;
        for (qsizetype i = 0; i < samples.size(); ++i) {
            const QJsonValue swr = samples.at(i).toObject().value(QStringLiteral("swr"));
            if (swr.isDouble() && swr.toDouble() < best) { best = swr.toDouble(); index = static_cast<int>(i); }
        }
        m_marker->setValue(index);
    });
    connect(m_save, &QPushButton::clicked, this, &AetherLabWidget::saveSession);
    connect(returnButton, &QPushButton::clicked, this, &AetherLabWidget::returnRequested);
    refresh();
}

void AetherLabWidget::refresh()
{
    const QJsonObject state = m_model->snapshot();
    const quint64 generation = static_cast<quint64>(state.value(QStringLiteral("generation")).toDouble());
    if (m_generation != generation) { m_generation = generation; m_isolated->setChecked(false); }
    const int selected = m_devices->currentData().toInt();
    const QJsonArray devices = state.value(QStringLiteral("devices")).toArray();
    const QString signature = QString::fromUtf8(QJsonDocument(devices).toJson(QJsonDocument::Compact));
    if (m_deviceSignature != signature) {
        m_deviceSignature = signature;
        m_devices->clear();
        for (const QJsonValue& value : devices) {
            const QJsonObject device = value.toObject();
            m_devices->addItem(tr("%1 · %2").arg(device.value(QStringLiteral("product")).toString(),
                device.value(QStringLiteral("serial")).toString()), device.value(QStringLiteral("index")).toInt());
        }
        if (selected < m_devices->count()) { m_devices->setCurrentIndex(selected); }
    }
    const AntennaAnalyzerModel::State status = m_model->state();
    const bool active = status == AntennaAnalyzerModel::State::Identifying || status == AntennaAnalyzerModel::State::Ready
        || status == AntennaAnalyzerModel::State::SettingFrequency || status == AntennaAnalyzerModel::State::SettingWidth
        || status == AntennaAnalyzerModel::State::Sweeping || status == AntennaAnalyzerModel::State::Closing;
    availability(m_rescan, !active, tr("Disconnect before rescanning USB"));
    availability(m_disconnect, active, tr("No open instrument session"));
    availability(m_connect, !active && !devices.isEmpty(), tr("Refresh USB and select a Stick Pro; disconnect any open session first"));
    availability(m_devices, !active, tr("Disconnect before selecting another interface"));
    availability(m_sweep, status == AntennaAnalyzerModel::State::Ready && m_isolated->isChecked(), tr("Connect and identify the Stick Pro, then confirm RF isolation"));
    availability(m_cancel, active, tr("No open instrument session"));
    const QString statusText = tr("%1 · %2 · %3 received rows%4").arg(analyzerStatusLabel(status),
        state.value(QStringLiteral("identity")).toString().isEmpty() ? tr("No verified identity") : state.value(QStringLiteral("identity")).toString())
        .arg(state.value(QStringLiteral("receivedRows")).toInt())
        .arg(m_model->lastError().isEmpty() ? QString() : QStringLiteral(" · ") + m_model->lastError());
    if (m_status->text() != statusText) {
        m_status->setText(statusText);
        QAccessibleEvent event(m_status, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
    const QJsonArray samples = state.value(QStringLiteral("samples")).toArray();
    availability(m_save, !samples.isEmpty() || !state.value(QStringLiteral("trace")).toArray().isEmpty(), tr("No samples or protocol trace to save"));
    m_marker->blockSignals(true);
    m_marker->setRange(0, std::max(0, static_cast<int>(samples.size()) - 1));
    m_marker->blockSignals(false);
    availability(m_minimum, !samples.isEmpty(), tr("Run a sweep to find the minimum measured SWR"));
    availability(m_marker, !samples.isEmpty(), tr("Run a sweep to populate marker samples"));
    const qint64 start = state.value(QStringLiteral("startHz")).toDouble() > 0
        ? static_cast<qint64>(state.value(QStringLiteral("startHz")).toDouble()) : 14000000;
    const qint64 stop = state.value(QStringLiteral("stopHz")).toDouble() > 0
        ? static_cast<qint64>(state.value(QStringLiteral("stopHz")).toDouble()) : 14350000;
    m_plot->setSamples(samples, start, stop, m_marker->value());
    const QJsonObject sample = samples.isEmpty() ? QJsonObject() : samples.at(m_marker->value()).toObject();
    const QString frequency = samples.isEmpty() ? tr("No measurement")
        : tr("%1 MHz").arg(QString::number(sample.value(QStringLiteral("frequencyHz")).toDouble() / 1e6, 'f', 6));
    m_frequency->setText(frequency);
    m_quality->setText(samples.isEmpty() ? tr("Run a sweep to place a marker")
        : tr("Sample %1 / %2 · %3").arg(m_marker->value() + 1).arg(samples.size()).arg(sample.value(QStringLiteral("quality")).toString()));
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        const QString& key = it.key();
        QString text = QStringLiteral("—");
        if (!samples.isEmpty()) {
            const QJsonValue value = sample.value(key);
            if (key.endsWith(QLatin1Char('H')) || key.endsWith(QLatin1Char('F'))) {
                text = value.isDouble() ? engineering(value.toDouble(), key.right(1)) : tr("undefined");
            } else {
                text = number(sample, key, key == QLatin1String("swr") || key == QLatin1String("reflectionMagnitude") ? 3 : 2);
                if (value.isDouble() && key.endsWith(QLatin1String("Ohm"))) { text += QStringLiteral(" Ω"); }
                if (value.isDouble() && key.endsWith(QLatin1String("Deg"))) { text += QStringLiteral("°"); }
                if (key == QLatin1String("returnLossDb")) { text += QStringLiteral(" dB"); }
            }
        }
        if (it.value()->text() != text) {
            it.value()->setText(text);
            QAccessibleValueChangeEvent event(it.value(), text);
            QAccessible::updateAccessibility(&event);
        }
    }
}

void AetherLabWidget::saveSession()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save AetherLab session"), QStringLiteral("aetherlab-session.json"), tr("JSON (*.json)"));
    if (path.isEmpty()) { return; }
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(m_model->snapshot()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_status->setText(tr("Could not save session: %1").arg(file.errorString()));
    }
}

void AetherLabWidget::resizeEvent(QResizeEvent* event)
{
    CanonCanvas::resizeEvent(event);
    m_heading->setDirection(event->size().width() < 1000 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_controls->setDirection(event->size().width() < 1280 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    const bool narrow = event->size().width() < 1000;
    m_analysis->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_models->setDirection(event->size().width() < 720 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

} // namespace AetherSDR

#include "AetherLabWidget.moc"
