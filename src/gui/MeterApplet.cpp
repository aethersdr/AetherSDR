#include "MeterApplet.h"
#include "HGauge.h"
#include "models/MeterModel.h"

#include <QVBoxLayout>
#include <QLabel>

namespace MasterSDR {

static const char* kSectionStyle =
    "QLabel { color: #8090a0; font-size: 10px; font-weight: bold; "
    "padding-top: 2px; }";

MeterApplet::MeterApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 2, 4, 2);
    vbox->setSpacing(2);

    auto* header = new QLabel("Radio Hardware");
    header->setStyleSheet(kSectionStyle);
    vbox->addWidget(header);

    m_paTempGauge = new HGauge(0.0f, 120.0f, 70.0f, "PA Temp", "",
        {{0, "0"}, {30, "30"}, {55, "55"}, {70, "70"}, {90, "90"}, {120, "120"}},
        this, 55.0f);
    vbox->addWidget(m_paTempGauge);

    m_supplyGauge = new HGauge(10.0f, 16.0f, 15.0f, "+13.8V", "",
        {{10.5f, "10.5"}, {12, "12"}, {13.8f, "13.8"}, {15, "15"}},
        this, 14.1f);
    vbox->addWidget(m_supplyGauge);

    m_fanGauge = new HGauge(0.0f, 3000.0f, 2500.0f, "Main Fan", "",
        {{0, "0"}, {500, "500"}, {1000, "1k"}, {1500, "1.5k"}, {2000, "2k"}, {3000, "3k"}},
        this, 2000.0f);
    vbox->addWidget(m_fanGauge);

    vbox->addStretch();
}

void MeterApplet::setMeterModel(MeterModel* model)
{
    m_model = model;

    connect(model, &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyV) {
        m_paTempGauge->setValue(paTemp);
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

    if (index == m_fanIdx && m_fanIdx >= 0)
        m_fanGauge->setValue(value);
}

} // namespace MasterSDR
