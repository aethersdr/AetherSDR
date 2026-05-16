#include "DaxApplet.h"
#include "MeterSlider.h"
#include "SliceLabel.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>

namespace AetherSDR {

namespace {

constexpr const char* kSectionStyle =
    "QWidget { background: transparent; }"
    "QLabel { color: #8090a0; font-size: 11px; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #205070;"
    "  border-radius: 3px; padding: 2px 8px; font-size: 11px; font-weight: bold; color: #c8d8e8; }"
    "QPushButton:hover { background: #204060; }";

const QString kGreenToggle =
    "QPushButton { background: #1a2a3a; border: 1px solid #205070; border-radius: 3px;"
    " color: #c8d8e8; font-size: 11px; font-weight: bold; padding: 2px 8px; }"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked { background: #006040; color: #00ff88; border: 1px solid #00a060; }";

constexpr const char* kDimLabel =
    "QLabel { color: #8090a0; font-size: 11px; }";

const QString kStatusLabel = "QLabel { color: #506070; font-size: 11px; }";

} // namespace

DaxApplet::DaxApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
    hide();  // hidden by default
}

void DaxApplet::buildUI()
{
    setStyleSheet(kSectionStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto& settings = AppSettings::instance();

    // DAX enable row
    auto* daxEnRow = new QHBoxLayout;
    daxEnRow->setContentsMargins(4, 2, 4, 2);
    auto* daxLabel = new QLabel("DAX:");
    daxLabel->setStyleSheet(kDimLabel);
    daxEnRow->addWidget(daxLabel);
    daxEnRow->addStretch();
    m_daxEnable = new QPushButton("Enable");
    m_daxEnable->setCheckable(true);
    m_daxEnable->setStyleSheet(kGreenToggle);
    m_daxEnable->setFixedSize(60, 22);
    daxEnRow->addWidget(m_daxEnable);

    // DAX enable button → save setting + notify MainWindow
    {
        const QSignalBlocker b(m_daxEnable);
        m_daxEnable->setChecked(
            settings.value("AutoStartDAX", "False").toString() == "True");
    }
    connect(m_daxEnable, &QPushButton::toggled, this, [this](bool on) {
        auto& ss = AppSettings::instance();
        ss.setValue("AutoStartDAX", on ? "True" : "False");
        ss.save();
        emit daxToggled(on);
    });

    // RX channel meter/sliders (DAX 1-4)
    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(4, 1, 4, 1);
        row->setSpacing(4);
        auto* chLabel = new QLabel(QString("DAX %1:").arg(i + 1));
        chLabel->setStyleSheet(kDimLabel);
        chLabel->setFixedWidth(40);
        row->addWidget(chLabel);

        m_daxRxStatus[i] = new QLabel(QStringLiteral("\u2014"));
        m_daxRxStatus[i]->setStyleSheet(kStatusLabel);
        m_daxRxStatus[i]->setFixedWidth(40);
        m_daxRxStatus[i]->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
        row->addWidget(m_daxRxStatus[i]);

        m_daxRxMeter[i] = new MeterSlider;
        {
            auto key = QStringLiteral("DaxRxGain%1").arg(i + 1);
            float saved = settings.value(key, "0.5").toString().toFloat();
            m_daxRxMeter[i]->setGain(std::clamp(saved, 0.0f, 1.0f));
        }
        connect(m_daxRxMeter[i], &MeterSlider::gainChanged, this, [this, i](float g) {
            auto& ss = AppSettings::instance();
            ss.setValue(QStringLiteral("DaxRxGain%1").arg(i + 1), QString::number(g, 'f', 2));
            ss.save();
            emit daxRxGainChanged(i + 1, g);
        });
        row->addWidget(m_daxRxMeter[i], 1);

        outer->addLayout(row);
    }

    // TX meter/slider
    auto* txRow = new QHBoxLayout;
    txRow->setContentsMargins(4, 1, 4, 1);
    txRow->setSpacing(4);
    auto* txLabel = new QLabel("TX:");
    txLabel->setStyleSheet(kDimLabel);
    txLabel->setFixedWidth(40);
    txRow->addWidget(txLabel);

    m_daxTxStatus = new QLabel(QStringLiteral("\u2014"));
    m_daxTxStatus->setStyleSheet(kStatusLabel);
    m_daxTxStatus->setFixedWidth(40);
    m_daxTxStatus->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
    txRow->addWidget(m_daxTxStatus);

    m_daxTxMeter = new MeterSlider;
    {
        float saved = settings.value("DaxTxGain", "0.5").toString().toFloat();
        m_daxTxMeter->setGain(std::clamp(saved, 0.0f, 1.0f));
    }
    connect(m_daxTxMeter, &MeterSlider::gainChanged, this, [this](float g) {
        auto& ss = AppSettings::instance();
        ss.setValue("DaxTxGain", QString::number(g, 'f', 2));
        ss.save();
        emit daxTxGainChanged(g);
    });
    txRow->addWidget(m_daxTxMeter, 1);

    outer->addLayout(txRow);
    outer->addLayout(daxEnRow);
}

void DaxApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    if (!model) {
        return;
    }

    // Wire slice add/remove for DAX channel tracking
    connect(model, &RadioModel::sliceAdded, this, [this](SliceModel* s) {
        connect(s, &SliceModel::daxChannelChanged, this, [this]() {
            // Update DAX status labels
            for (int i = 0; i < kChannels; ++i) {
                m_daxRxStatus[i]->setText(QStringLiteral("\u2014"));
            }
            if (!m_model) {
                return;
            }
            for (auto* sl : m_model->slices()) {
                int ch = sl->daxChannel();
                if (ch >= 1 && ch <= kChannels) {
                    m_daxRxStatus[ch - 1]->setText(
                        QString("Slice %1").arg(
                            SliceLabel::richText(sl->sliceId(), sl->letter())));
                }
            }
        });
    });

    // Wire TX slice label — always show which slice has TX privileges
    auto updateTxLabel = [this]() {
        if (!m_model) {
            m_daxTxStatus->setText(QStringLiteral("\u2014"));
            return;
        }
        for (auto* s : m_model->slices()) {
            if (s->isTxSlice()) {
                m_daxTxStatus->setText(
                    QString("Slice %1").arg(
                        SliceLabel::richText(s->sliceId(), s->letter())));
                return;
            }
        }
        m_daxTxStatus->setText(QStringLiteral("\u2014"));
    };
    connect(model, &RadioModel::sliceAdded, this, [this, updateTxLabel](SliceModel* s) {
        connect(s, &SliceModel::txSliceChanged, this, updateTxLabel);
        updateTxLabel();
    });
    updateTxLabel();
}

void DaxApplet::setDaxEnabled(bool on)
{
    QSignalBlocker b(m_daxEnable);
    m_daxEnable->setChecked(on);
}

void DaxApplet::setDaxRxLevel(int channel, float rms)
{
    if (channel < 1 || channel > kChannels) {
        return;
    }
    // Exponential smoothing: fast attack (α=0.4), slow decay (α=0.08)
    static float smoothed[kChannels]{};
    float& s = smoothed[channel - 1];
    float alpha = (rms > s) ? 0.4f : 0.08f;
    s = alpha * rms + (1.0f - alpha) * s;
    m_daxRxMeter[channel - 1]->setLevel(std::clamp(s * 2.0f, 0.0f, 1.0f));
}

void DaxApplet::setDaxTxLevel(float rms)
{
    m_daxTxMeter->setLevel(std::clamp(rms * 2.0f, 0.0f, 1.0f));
}

} // namespace AetherSDR
