#include "DaxIqApplet.h"
#include "ComboStyle.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/DaxIqModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr const char* kSectionStyle =
    "QWidget { background: transparent; }"
    "QLabel { color: #8090a0; font-size: 11px; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #205070;"
    "  border-radius: 3px; padding: 2px 8px; font-size: 11px; font-weight: bold; color: #c8d8e8; }"
    "QPushButton:hover { background: #204060; }";

constexpr const char* kDimLabel =
    "QLabel { color: #8090a0; font-size: 11px; }";

const QString kIqBtnOn =
    "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
    "border: 1px solid #008ba8; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";

const QString kIqBtnOff =
    "QPushButton { background: #1a2a3a; color: #8090a0; "
    "border: 1px solid #205070; padding: 2px 8px; border-radius: 3px; font-size: 10px; }";

} // namespace

DaxIqApplet::DaxIqApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
    hide();  // hidden by default
}

void DaxIqApplet::buildUI()
{
    setStyleSheet(kSectionStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addSpacing(2);

    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        row->setContentsMargins(4, 1, 4, 1);

        auto* label = new QLabel(QString("IQ %1:").arg(i + 1));
        label->setStyleSheet(kDimLabel);
        label->setFixedWidth(28);
        row->addWidget(label);

        m_iqRateCombo[i] = new GuardedComboBox;
        applyComboStyle(m_iqRateCombo[i]);
        m_iqRateCombo[i]->addItem("24k",  24000);
        m_iqRateCombo[i]->addItem("48k",  48000);
        m_iqRateCombo[i]->addItem("96k",  96000);
        m_iqRateCombo[i]->addItem("192k", 192000);
        {
            int savedRate = AppSettings::instance()
                .value(QStringLiteral("DaxIqRate%1").arg(i + 1), "48000").toInt();
            QSignalBlocker sb(m_iqRateCombo[i]);
            for (int j = 0; j < m_iqRateCombo[i]->count(); ++j) {
                if (m_iqRateCombo[i]->itemData(j).toInt() == savedRate) {
                    m_iqRateCombo[i]->setCurrentIndex(j);
                    break;
                }
            }
        }
        m_iqRateCombo[i]->setFixedWidth(60);
        connect(m_iqRateCombo[i], &QComboBox::currentIndexChanged, this, [this, i]() {
            int rate = m_iqRateCombo[i]->currentData().toInt();
            auto& ss = AppSettings::instance();
            ss.setValue(QStringLiteral("DaxIqRate%1").arg(i + 1), QString::number(rate));
            ss.save();
            emit iqRateChanged(i + 1, rate);
        });
        row->addWidget(m_iqRateCombo[i]);

        m_iqMeter[i] = new QProgressBar;
        m_iqMeter[i]->setRange(0, 100);
        m_iqMeter[i]->setValue(0);
        m_iqMeter[i]->setTextVisible(false);
        m_iqMeter[i]->setFixedHeight(14);
        m_iqMeter[i]->setStyleSheet(
            "QProgressBar { background: #0a0a14; border: 1px solid #203040; border-radius: 2px; }"
            "QProgressBar::chunk { background: #00b4d8; }");
        row->addWidget(m_iqMeter[i], 1);

        m_iqEnable[i] = new QPushButton("Off");
        m_iqEnable[i]->setFixedWidth(36);
        m_iqEnable[i]->setStyleSheet(kIqBtnOff);
        connect(m_iqEnable[i], &QPushButton::clicked, this, [this, i]() {
            bool wasOn = m_iqEnable[i]->text() == "On";
            auto& ss = AppSettings::instance();
            if (wasOn) {
                emit iqDisableRequested(i + 1);
                m_iqEnable[i]->setText("Off");
                m_iqEnable[i]->setStyleSheet(kIqBtnOff);
                m_iqMeter[i]->setValue(0);
                ss.setValue(QStringLiteral("DaxIqEnabled%1").arg(i + 1), "False");
            } else {
                emit iqEnableRequested(i + 1);
                m_iqEnable[i]->setText("On");
                m_iqEnable[i]->setStyleSheet(kIqBtnOn);
                ss.setValue(QStringLiteral("DaxIqEnabled%1").arg(i + 1), "True");
            }
            ss.save();
        });
        row->addWidget(m_iqEnable[i]);

        outer->addLayout(row);
    }
}

void DaxIqApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    if (!model) {
        return;
    }

    // DAX IQ streams are per-session. On reconnect: reset the UI immediately,
    // then re-enable persisted channels after a short delay. connectionStateChanged(true)
    // fires at TCP connect, before sub/stream setup completes; emitting
    // iqEnableRequested immediately risks a rejected stream create if the radio
    // isn't ready yet.
    connect(model, &RadioModel::connectionStateChanged,
            this, [this](bool connected) {
        for (int i = 0; i < kChannels; ++i) {
            if (!m_iqEnable[i]) {
                continue;
            }
            if (!connected) {
                m_iqEnable[i]->setText("Off");
                m_iqEnable[i]->setStyleSheet(kIqBtnOff);
                if (m_iqMeter[i]) {
                    m_iqMeter[i]->setValue(0);
                }
                continue;
            }
            // Reset to Off; stream requests follow after session setup settles.
            m_iqEnable[i]->setText("Off");
            m_iqEnable[i]->setStyleSheet(kIqBtnOff);
            if (m_iqMeter[i]) {
                m_iqMeter[i]->setValue(0);
            }
        }
        if (!connected) {
            return;
        }
        QTimer::singleShot(1500, this, [this]() {
            if (!m_model || !m_model->isConnected()) {
                return;
            }
            auto& ss = AppSettings::instance();
            for (int i = 0; i < kChannels; ++i) {
                if (!m_iqEnable[i]) {
                    continue;
                }
                if (ss.value(QStringLiteral("DaxIqEnabled%1").arg(i + 1), "False").toString() == "True") {
                    emit iqEnableRequested(i + 1);
                    m_iqEnable[i]->setText("On");
                    m_iqEnable[i]->setStyleSheet(kIqBtnOn);
                }
            }
        });
    });

    // Wire DAX IQ stream state changes → sync On/Off buttons
    connect(&model->daxIqModel(), &DaxIqModel::streamChanged, this, [this](int ch) {
        if (ch < 1 || ch > kChannels) {
            return;
        }
        int idx = ch - 1;
        bool exists = m_model->daxIqModel().stream(ch).exists;
        m_iqEnable[idx]->setText(exists ? "On" : "Off");
        m_iqEnable[idx]->setStyleSheet(exists ? kIqBtnOn : kIqBtnOff);
        if (!exists) {
            m_iqMeter[idx]->setValue(0);
        }

        // Sync rate combo from radio state
        int rate = m_model->daxIqModel().stream(ch).sampleRate;
        QSignalBlocker sb(m_iqRateCombo[idx]);
        for (int i = 0; i < m_iqRateCombo[idx]->count(); ++i) {
            if (m_iqRateCombo[idx]->itemData(i).toInt() == rate) {
                m_iqRateCombo[idx]->setCurrentIndex(i);
                break;
            }
        }
    });
}

void DaxIqApplet::setDaxIqLevel(int channel, float rms)
{
    if (channel < 1 || channel > kChannels) {
        return;
    }
    // Scale RMS to 0-100 for QProgressBar (IQ values are typically 0.0-0.5 range)
    int level = static_cast<int>(std::clamp(rms * 200.0f, 0.0f, 100.0f));
    m_iqMeter[channel - 1]->setValue(level);
}

} // namespace MasterSDR
