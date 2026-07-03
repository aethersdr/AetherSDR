#include "MeterApplet.h"
#include "HGauge.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/MeterModel.h"

#include <QAccessible>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

static const char* kSectionStyle =
    "QLabel { color: #8090a0; font-size: 10px; font-weight: bold; "
    "padding-top: 2px; }";

constexpr const char* kMtrAppletSettingsKey = "MtrApplet";
constexpr const char* kTempFahrenheitField  = "tempFahrenheit";

QJsonObject readMtrAppletSettings()
{
    const QString json = AppSettings::instance()
        .value(kMtrAppletSettingsKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writeMtrAppletSettings(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kMtrAppletSettingsKey,
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

bool readMtrTempFahrenheit()
{
    return readMtrAppletSettings()
        .value(kTempFahrenheitField)
        .toString(QStringLiteral("False")) == QStringLiteral("True");
}

void writeMtrTempFahrenheit(bool enabled)
{
    QJsonObject obj = readMtrAppletSettings();
    obj[kTempFahrenheitField] =
        enabled ? QStringLiteral("True") : QStringLiteral("False");
    writeMtrAppletSettings(obj);
}

float toFahrenheit(float degC)
{
    return degC * 9.0f / 5.0f + 32.0f;
}

QString formatTemp(float degC, bool fahrenheit)
{
    const float disp = fahrenheit ? toFahrenheit(degC) : degC;
    return QStringLiteral("%1°%2")
        .arg(disp, 0, 'f', 1)
        .arg(fahrenheit ? "F" : "C");
}

// °C scale ticks and the corresponding °F values (toFahrenheit of each)
static const QVector<HGauge::Tick> kCelsiusTicks = {
    {0.0f,   "0"},   {30.0f, "30"},   {55.0f,  "55"},
    {70.0f, "70"},   {90.0f, "90"},  {120.0f, "120"}
};

static const QVector<HGauge::Tick> kFahrenheitTicks = {
    {32.0f,  "32"},  {86.0f, "86"},  {131.0f, "131"},
    {158.0f, "158"}, {194.0f, "194"}, {248.0f, "248"}
};

} // namespace

MeterApplet::MeterApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/meter"));
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    // ── Header row: section label + °C/°F toggle ─────────────────────────────
    m_tempFahrenheit = readMtrTempFahrenheit();

    auto* header = new QLabel("Radio Hardware");
    header->setStyleSheet(kSectionStyle);

    m_tempUnitBtn = new QPushButton(m_tempFahrenheit ? "°F" : "°C", this);
    m_tempUnitBtn->setObjectName(QStringLiteral("mtrTempUnitButton"));
    m_tempUnitBtn->setFlat(true);
    m_tempUnitBtn->setFocusPolicy(Qt::TabFocus);
    m_tempUnitBtn->setCursor(Qt::PointingHandCursor);
    m_tempUnitBtn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_tempUnitBtn->setAccessibleDescription(
        "Toggles PA temperature display between Celsius and Fahrenheit");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_tempUnitBtn,
        "QPushButton { background: transparent; border: 1px solid transparent; "
        "color: #8090a0; font-size: 10px; text-align: right; padding: 0 2px; }"
        "QPushButton:hover { border-color: #203040; color: #ffffff; }"
        "QPushButton:focus { border-color: #00b4d8; }");
    connect(m_tempUnitBtn, &QPushButton::clicked, this, [this]() {
        m_tempFahrenheit = !m_tempFahrenheit;
        writeMtrTempFahrenheit(m_tempFahrenheit);
        updatePaTempDisplay();
    });

    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(4);
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(header);
    headerRow->addStretch();
    headerRow->addWidget(m_tempUnitBtn);
    vbox->addLayout(headerRow);

    // ── PA Temp gauge ─────────────────────────────────────────────────────────
    m_paTempGauge = new HGauge(0.0f, 120.0f, 70.0f, "PA Temp", "",
        kCelsiusTicks, this, 55.0f);
    m_paTempGauge->setAccessibleName(tr("PA temperature"));
    vbox->addWidget(m_paTempGauge);

    // ── Supply voltage gauge ───────────────────────────────────────────────────
    m_supplyGauge = new HGauge(10.0f, 16.0f, 15.0f, "+13.8V", "",
        {{10.5f, "10.5"}, {12, "12"}, {13.8f, "13.8"}, {15, "15"}},
        this, 14.1f);
    m_supplyGauge->setAccessibleName(tr("Supply voltage"));
    vbox->addWidget(m_supplyGauge);

    // ── Main Fan gauge ─────────────────────────────────────────────────────────
    m_fanGauge = new HGauge(0.0f, 3000.0f, 2500.0f, "Main Fan", "",
        {{0, "0"}, {500, "500"}, {1000, "1k"}, {1500, "1.5k"}, {2000, "2k"}, {3000, "3k"}},
        this, 2000.0f);
    m_fanGauge->setAccessibleName(tr("Main fan speed"));
    vbox->addWidget(m_fanGauge);

    vbox->addStretch();

    updatePaTempDisplay();
}

void MeterApplet::setMeterModel(MeterModel* model)
{
    m_model = model;

    connect(model, &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyV) {
        m_paTemp    = paTemp;
        m_hasPaTemp = true;
        const float dispTemp = m_tempFahrenheit ? toFahrenheit(paTemp) : paTemp;
        m_paTempGauge->setValue(dispTemp);
        m_paTempGauge->setLabel(formatTemp(paTemp, m_tempFahrenheit));

        m_supplyGauge->setValue(supplyV);
        m_supplyGauge->setLabel(QString("+%1V").arg(supplyV, 0, 'f', 2));
    });

    connect(model, &MeterModel::meterUpdated,
            this, &MeterApplet::onMeterUpdated);

    resolveIndices();
}

void MeterApplet::resolveIndices()
{
    if (!m_model || m_resolved) return;

    m_fanIdx = m_model->findMeter("RAD", "MAINFAN");
    m_resolved = (m_fanIdx >= 0);
}

void MeterApplet::onMeterUpdated(int index, float value)
{
    if (!m_resolved)
        resolveIndices();

    if (index == m_fanIdx && m_fanIdx >= 0) {
        m_fanGauge->setValue(value);
        m_fanGauge->setLabel(QStringLiteral("%1 rpm").arg(static_cast<int>(value)));
    }
}

void MeterApplet::updatePaTempDisplay()
{
    // Apply the correct scale — snapping avoids animating the fill bar
    // across the °C→°F unit jump when the user toggles.
    if (m_tempFahrenheit) {
        m_paTempGauge->setRange(32.0f, 248.0f, 158.0f, kFahrenheitTicks, 131.0f);
        if (m_hasPaTemp)
            m_paTempGauge->setValueImmediate(toFahrenheit(m_paTemp));
    } else {
        m_paTempGauge->setRange(0.0f, 120.0f, 70.0f, kCelsiusTicks, 55.0f);
        if (m_hasPaTemp)
            m_paTempGauge->setValueImmediate(m_paTemp);
    }

    if (m_hasPaTemp)
        m_paTempGauge->setLabel(formatTemp(m_paTemp, m_tempFahrenheit));

    if (!m_tempUnitBtn)
        return;

    m_tempUnitBtn->setText(m_tempFahrenheit ? "°F" : "°C");
    const QString nextUnit = m_tempFahrenheit ? tr("Celsius") : tr("Fahrenheit");
    m_tempUnitBtn->setToolTip(
        tr("PA temperature unit\nClick to show in %1").arg(nextUnit));
    m_tempUnitBtn->setAccessibleName(
        tr("PA temperature: %1 — click to switch to %2")
            .arg(m_tempFahrenheit ? tr("Fahrenheit") : tr("Celsius"), nextUnit));

    if (QAccessible::isActive()) {
        QAccessibleEvent event(m_tempUnitBtn, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
}

} // namespace AetherSDR
