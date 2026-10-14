#include "AetherLabWindow.h"

#include "core/ThemeManager.h"
#include "models/AntennaAnalyzerModel.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QPaintEvent>
#include <QVariant>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

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
        setMinimumSize(430, 300);
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
QString number(const QJsonObject& sample, const QString& key, int precision)
{
    const QJsonValue value = sample.value(key);
    if (value.isDouble()) { return QString::number(value.toDouble(), 'f', precision); }
    const QString quality = sample.value(QStringLiteral("quality")).toString();
    if ((key == QLatin1String("swr") && quality == QLatin1String("total-reflection"))
        || (key == QLatin1String("returnLossDb") && quality == QLatin1String("perfect-match"))) {
        return QStringLiteral("∞");
    }
    return QCoreApplication::translate("AetherLabWindow", "undefined");
}
} // namespace

AetherLabWindow::AetherLabWindow(AntennaAnalyzerModel* model, QWidget* parent)
    : CanonWindow(tr("AetherLab — Stick Pro USB proof of concept"), parent, Kind::Workspace), m_model(model)
{
    static const bool installed = [] { QAccessible::installFactory(labAccessibility); return true; }();
    Q_UNUSED(installed);
    setObjectName(QStringLiteral("aetherLabWindow"));
    setAccessibleName(tr("AetherLab antenna analyzer"));
    setLaunchSize(QSize(1050, 730));
    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(22, 18, 22, 20);
    layout->setSpacing(14);
    layout->addWidget(makeCanonHeader(tr("AetherLab · ANTENNA QUICK CHECK")));
    auto* warning = new QLabel(tr("USB proof of concept · Isolate the analyzer RF port from transmitters. Stop AntScope before connecting. AetherLab does not inhibit radio TX."), bodyWidget());
    warning->setWordWrap(true);
    warning->setAccessibleName(tr("Analyzer RF isolation warning"));
    ThemeManager::instance().applyStyleSheet(warning, QStringLiteral("color: {{color.accent.warning}};"));
    layout->addWidget(warning);
    auto* devicesRow = new QHBoxLayout;
    m_devices = new QComboBox(bodyWidget());
    m_devices->setObjectName(QStringLiteral("aetherLabDevice"));
    m_devices->setAccessibleName(tr("Stick Pro USB interface"));
    m_devices->setPlaceholderText(tr("Refresh USB to find a Stick Pro"));
    devicesRow->addWidget(m_devices, 1);
    auto* rescan = new QPushButton(tr("Refresh USB"), bodyWidget());
    m_rescan = rescan;
    rescan->setAccessibleName(tr("Refresh Stick Pro USB devices"));
    devicesRow->addWidget(rescan);
    m_connect = new QPushButton(tr("Connect"), bodyWidget());
    m_connect->setObjectName(QStringLiteral("aetherLabConnect"));
    m_connect->setAccessibleName(tr("Connect Stick Pro"));
    devicesRow->addWidget(m_connect);
    auto* disconnectButton = new QPushButton(tr("Disconnect"), bodyWidget());
    m_disconnect = disconnectButton;
    disconnectButton->setAccessibleName(tr("Disconnect Stick Pro"));
    devicesRow->addWidget(disconnectButton);
    layout->addLayout(devicesRow);
    m_status = new QLabel(bodyWidget());
    m_status->setObjectName(QStringLiteral("aetherLabStatus"));
    m_status->setAccessibleName(tr("Analyzer session status"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* analysis = new QHBoxLayout;
    m_plot = new AetherLabPlot(bodyWidget());
    analysis->addWidget(m_plot, 3);
    m_readout = new QLabel(tr("No measurement"), bodyWidget());
    m_readout->setObjectName(QStringLiteral("aetherLabReadout"));
    m_readout->setAccessibleName(tr("Marker measurement"));
    m_readout->setMinimumWidth(240);
    m_readout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont readoutFont = m_readout->font();
    readoutFont.setPointSize(18);
    m_readout->setFont(readoutFont);
    analysis->addWidget(m_readout, 1);
    layout->addLayout(analysis, 1);
    m_marker = new QSlider(Qt::Horizontal, bodyWidget());
    m_marker->setObjectName(QStringLiteral("aetherLabMarker"));
    m_marker->setAccessibleName(tr("Measurement marker sample"));
    layout->addWidget(m_marker);
    auto* controls = new QHBoxLayout;
    const auto frequency = [this](const QString& name, double value) {
        auto* spin = new QDoubleSpinBox(bodyWidget());
        spin->setDecimals(6);
        spin->setRange(0.1, 600.0);
        spin->setValue(value);
        spin->setSuffix(tr(" MHz"));
        spin->setAccessibleName(name);
        spin->setAccessibleDescription(tr("Frequency in MHz, 0.1 to 600, with 1 Hz entry resolution"));
        return spin;
    };
    m_start = frequency(tr("Sweep start frequency"), 14.0);
    m_start->setObjectName(QStringLiteral("aetherLabStartMHz"));
    m_stop = frequency(tr("Sweep stop frequency"), 14.35);
    m_stop->setObjectName(QStringLiteral("aetherLabStopMHz"));
    m_intervals = new QSpinBox(bodyWidget());
    m_intervals->setRange(1, 1000);
    m_intervals->setValue(200);
    m_intervals->setObjectName(QStringLiteral("aetherLabIntervals"));
    m_intervals->setAccessibleName(tr("FRX sweep intervals"));
    m_intervals->setAccessibleDescription(tr("Experimental FRX N count. N plus 1 rows expected; hardware qualification pending. Host cap is 1000."));
    controls->addWidget(new QLabel(tr("Start"), bodyWidget()));
    controls->addWidget(m_start);
    controls->addWidget(new QLabel(tr("Stop"), bodyWidget()));
    controls->addWidget(m_stop);
    controls->addWidget(new QLabel(tr("FRX intervals"), bodyWidget()));
    controls->addWidget(m_intervals);
    m_sweep = new QPushButton(tr("Sweep"), bodyWidget());
    m_sweep->setObjectName(QStringLiteral("aetherLabSweep"));
    m_sweep->setAccessibleName(tr("Sweep antenna with Stick Pro"));
    m_sweep->setProperty("aetherLabRf", true);
    controls->addWidget(m_sweep);
    m_cancel = new QPushButton(tr("Stop"), bodyWidget());
    m_cancel->setObjectName(QStringLiteral("aetherLabStop"));
    m_cancel->setAccessibleName(tr("Stop analyzer sweep"));
    controls->addWidget(m_cancel);
    layout->addLayout(controls);
    m_isolated = new QCheckBox(tr("Analyzer RF port is isolated from transmitters"), bodyWidget());
    m_isolated->setObjectName(QStringLiteral("aetherLabIsolated"));
    m_isolated->setAccessibleName(tr("Confirm analyzer RF isolation"));
    layout->addWidget(m_isolated);
    auto* footer = new QHBoxLayout;
    auto* minimum = new QPushButton(tr("Minimum SWR marker"), bodyWidget());
    m_minimum = minimum;
    minimum->setAccessibleName(tr("Select minimum measured SWR"));
    footer->addWidget(minimum);
    m_save = new QPushButton(tr("Save session…"), bodyWidget());
    m_save->setAccessibleName(tr("Save analyzer samples and protocol trace"));
    footer->addWidget(m_save);
    footer->addStretch();
    auto* returnButton = new QPushButton(tr("Return to radio"), bodyWidget());
    returnButton->setAccessibleName(tr("Close AetherLab and return to radio"));
    footer->addWidget(returnButton);
    layout->addLayout(footer);
    auto* qualification = new QLabel(tr("50 Ω reference · Device-corrected R/X · SWR display 1–5 · N+1 count is unqualified · Every sweep closes USB after attempting off; reconnect for another sweep."), bodyWidget());
    qualification->setWordWrap(true);
    layout->addWidget(qualification);
    ThemeManager::instance().applyStyleSheet(bodyWidget(), QStringLiteral(
        "QLabel { color: {{color.canon.inkSoft}}; background: transparent; } "
        "QPushButton, QComboBox, QDoubleSpinBox, QSpinBox { color: {{color.canon.ink}}; background: {{color.canon.control}}; border: 1px solid {{color.canon.lineHi}}; border-radius: 5px; padding: 5px; } "
        "QPushButton:hover { border-color: {{color.canon.cyan}}; } "
        "QPushButton:disabled { color: {{color.button.foreground.disabled}}; background: {{color.button.background.disabled}}; border-color: {{color.button.border.disabled}}; }"));
    ThemeManager::instance().applyStyleSheet(m_readout, QStringLiteral(
        "font-size: 22px; color: {{color.canon.ink}}; background: transparent;"));
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(100);
    connect(m_refreshTimer, &QTimer::timeout, this, &AetherLabWindow::refresh);
    connect(model, &AntennaAnalyzerModel::changed, this, [this] {
        if (!m_refreshTimer->isActive()) { m_refreshTimer->start(); }
    });
    connect(rescan, &QPushButton::clicked, model, &AntennaAnalyzerModel::refreshDevices);
    connect(m_connect, &QPushButton::clicked, this, [this] { m_model->connectDevice(m_devices->currentData().toInt()); });
    connect(disconnectButton, &QPushButton::clicked, model, &AntennaAnalyzerModel::disconnectDevice);
    connect(m_sweep, &QPushButton::clicked, this, [this] {
        m_model->sweep(std::llround(m_start->value() * 1e6), std::llround(m_stop->value() * 1e6),
                       m_intervals->value(), m_isolated->isChecked());
    });
    connect(m_cancel, &QPushButton::clicked, model, &AntennaAnalyzerModel::stop);
    connect(m_isolated, &QCheckBox::toggled, this, &AetherLabWindow::refresh);
    connect(m_marker, &QSlider::valueChanged, this, &AetherLabWindow::refresh);
    connect(minimum, &QPushButton::clicked, this, [this] {
        const QJsonArray samples = m_model->snapshot().value(QStringLiteral("samples")).toArray();
        double best = INFINITY;
        int index = 0;
        for (qsizetype i = 0; i < samples.size(); ++i) {
            const QJsonValue swr = samples.at(i).toObject().value(QStringLiteral("swr"));
            if (swr.isDouble() && swr.toDouble() < best) { best = swr.toDouble(); index = static_cast<int>(i); }
        }
        m_marker->setValue(index);
    });
    connect(m_save, &QPushButton::clicked, this, &AetherLabWindow::saveSession);
    connect(returnButton, &QPushButton::clicked, this, &QWidget::close);
    refresh();
}

void AetherLabWindow::refresh()
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
    const QString statusText = tr("%1 · %2 · %3 received rows%4").arg(m_model->stateText(),
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
    QString readout = tr("No measurement");
    if (!samples.isEmpty()) {
        const QJsonObject sample = samples.at(m_marker->value()).toObject();
        readout = tr("%1 MHz\n\nSWR  %2\nR  %3 Ω\nX  %4 Ω\nRL  %5 dB\n\n%6")
            .arg(QString::number(sample.value(QStringLiteral("frequencyHz")).toDouble() / 1e6, 'f', 6),
                number(sample, QStringLiteral("swr"), 3), number(sample, QStringLiteral("rOhm"), 2),
                number(sample, QStringLiteral("xOhm"), 2), number(sample, QStringLiteral("returnLossDb"), 2),
                sample.value(QStringLiteral("quality")).toString());
    }
    if (readout != m_readout->text()) {
        m_readout->setText(readout);
        QAccessibleValueChangeEvent event(m_readout, readout);
        QAccessible::updateAccessibility(&event);
    }
}

void AetherLabWindow::saveSession()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save AetherLab session"), QStringLiteral("aetherlab-session.json"), tr("JSON (*.json)"));
    if (path.isEmpty()) { return; }
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(m_model->snapshot()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_status->setText(tr("Could not save session: %1").arg(file.errorString()));
    }
}

void AetherLabWindow::hideEvent(QHideEvent* event)
{
    if (m_model->state() != AntennaAnalyzerModel::State::Complete
        && m_model->state() != AntennaAnalyzerModel::State::Cancelled
        && m_model->state() != AntennaAnalyzerModel::State::Error
        && m_model->state() != AntennaAnalyzerModel::State::Disconnected) { m_model->stop(); }
    CanonWindow::hideEvent(event);
}

} // namespace AetherSDR

#include "AetherLabWindow.moc"
