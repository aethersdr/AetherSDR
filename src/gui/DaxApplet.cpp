#include "DaxApplet.h"
#include "MeterSlider.h"
#include "SliceLabel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
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
    theme::setContainer(this, QStringLiteral("applet/dax"));
    buildUI();
    hide();  // hidden by default
}

void DaxApplet::buildUI()
{
    setStyleSheet(kSectionStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

#ifdef Q_OS_WIN
    // On Windows AetherSDR has no built-in DAX bridge — it needs a kernel-mode
    // audio driver we ship only on macOS/Linux (the daxToggled → startDax()
    // wiring is compiled out here, see MainWindow_Session.cpp). DAX still works
    // on Windows via FlexRadio's own SmartSDR DAX drivers; we just don't provide
    // one. The Enable button, per-channel/TX meters and their labels would all
    // be inert, so on Windows the applet shows ONLY this note and nothing else
    // is built (#4112). The control member pointers stay null; the level/enable
    // setters and setRadioModel() are guarded for that. The full "where to set
    // it up" pointer lives in Help → Configuring Data Modes.
    auto* winNote = new QLabel(
        tr("No built-in DAX driver on Windows.\n"
           "Use TCI, or SmartSDR DAX."));
    winNote->setObjectName(QStringLiteral("daxWindowsNote"));
    winNote->setAccessibleName(tr("DAX driver not shipped on Windows"));
    winNote->setWordWrap(true);
    winNote->setStyleSheet(
        "QLabel { color: #d0a040; font-size: 11px; padding: 4px 6px; }");
    outer->addWidget(winNote);
    outer->addStretch();  // pin the note to the top; nothing below it on Windows
    return;
#else

    auto& settings = AppSettings::instance();

    // DAX enable row
    auto* daxEnRow = new QHBoxLayout;
    daxEnRow->setContentsMargins(4, 2, 4, 2);
    auto* daxLabel = new QLabel("DAX:");
    daxLabel->setStyleSheet(kDimLabel);
    daxEnRow->addWidget(daxLabel);
    daxEnRow->addStretch();
    const bool daxAutoStart = settings.value("AutoStartDAX", "False").toString() == "True";
    m_daxEnable = new QPushButton(daxAutoStart ? "Enabled" : "Disabled");
    m_daxEnable->setCheckable(true);
    m_daxEnable->setObjectName(QStringLiteral("daxEnable"));
    m_daxEnable->setAccessibleName(tr("DAX enable"));
    m_daxEnable->setAccessibleDescription(tr("Enable or disable DAX digital audio routing"));
    m_daxEnable->setStyleSheet(kGreenToggle);
    m_daxEnable->setFixedSize(76, 22);
    daxEnRow->addWidget(m_daxEnable);

    // DAX enable button → save setting + notify MainWindow
    {
        const QSignalBlocker b(m_daxEnable);
        m_daxEnable->setChecked(daxAutoStart);
    }
    connect(m_daxEnable, &QPushButton::toggled, this, [this](bool on) {
        m_daxEnable->setText(on ? "Enabled" : "Disabled");
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
        m_daxRxMeter[i]->setAccessibleName(tr("DAX RX %1 gain").arg(i + 1));
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
    m_daxTxMeter->setAccessibleName(tr("DAX TX gain"));
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
#endif  // !Q_OS_WIN
}

void DaxApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    // On Windows the applet is note-only — no status labels to update. (#4112)
    if (!model || !m_daxTxStatus) {
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
    if (!m_daxEnable) {  // note-only on Windows (#4112)
        return;
    }
    QSignalBlocker b(m_daxEnable);
    m_daxEnable->setChecked(on);
    m_daxEnable->setText(on ? "Enabled" : "Disabled");
}

void DaxApplet::setDaxRxLevel(int channel, float rms)
{
    if (channel < 1 || channel > kChannels || !m_daxRxMeter[channel - 1]) {
        return;  // note-only on Windows (#4112)
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
    if (!m_daxTxMeter) {  // note-only on Windows (#4112)
        return;
    }
    m_daxTxMeter->setLevel(std::clamp(rms * 2.0f, 0.0f, 1.0f));
}

} // namespace AetherSDR
