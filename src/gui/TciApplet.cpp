#include "TciApplet.h"
#include "SliceLabel.h"

#ifdef HAVE_WEBSOCKETS
#include "MeterSlider.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <algorithm>
#endif

namespace MasterSDR {

#ifdef HAVE_WEBSOCKETS

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

constexpr const char* kInsetStyle =
    "QLineEdit { font-size: 10px; background: #0a0a18; border: 1px solid #1e2e3e;"
    " border-radius: 3px; padding: 0px 2px; color: #c8d8e8; }";

const QString kStatusLabel = "QLabel { color: #506070; font-size: 11px; }";

} // namespace

#endif // HAVE_WEBSOCKETS

TciApplet::TciApplet(QWidget* parent) : QWidget(parent)
{
#ifdef HAVE_WEBSOCKETS
    buildUI();
    hide();  // hidden by default
#else
    (void)parent;
#endif
}

#ifdef HAVE_WEBSOCKETS

void TciApplet::buildUI()
{
    setStyleSheet(kSectionStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto& settings = AppSettings::instance();

    // RX channel meter/sliders (TCI RX1-RX4)
    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(4, 1, 4, 1);
        row->setSpacing(4);
        auto* chLabel = new QLabel(QString("RX%1:").arg(i + 1));
        chLabel->setStyleSheet(kDimLabel);
        chLabel->setFixedWidth(40);
        row->addWidget(chLabel);

        m_rxStatus[i] = new QLabel(QStringLiteral("\u2014"));
        m_rxStatus[i]->setStyleSheet(kStatusLabel);
        m_rxStatus[i]->setFixedWidth(40);
        m_rxStatus[i]->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
        row->addWidget(m_rxStatus[i]);

        m_rxMeter[i] = new MeterSlider;
        {
            auto key = QStringLiteral("TciRxGain%1").arg(i + 1);
            float saved = settings.value(key, "0.5").toString().toFloat();
            m_rxMeter[i]->setGain(std::clamp(saved, 0.0f, 1.0f));
        }
        connect(m_rxMeter[i], &MeterSlider::gainChanged, this, [this, i](float g) {
            auto& ss = AppSettings::instance();
            ss.setValue(QStringLiteral("TciRxGain%1").arg(i + 1), QString::number(g, 'f', 2));
            ss.save();
            emit tciRxGainChanged(i + 1, g);
        });
        row->addWidget(m_rxMeter[i], 1);

        outer->addLayout(row);
    }

    // TX meter/slider
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(4, 1, 4, 1);
        row->setSpacing(4);
        auto* txLabel = new QLabel("TX:");
        txLabel->setStyleSheet(kDimLabel);
        txLabel->setFixedWidth(40);
        row->addWidget(txLabel);

        m_txStatus = new QLabel(QStringLiteral("\u2014"));
        m_txStatus->setStyleSheet(kStatusLabel);
        m_txStatus->setFixedWidth(40);
        m_txStatus->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
        row->addWidget(m_txStatus);

        m_txMeter = new MeterSlider;
        {
            // TciServer owns TciTxGain persistence (with DaxTxGain migration
            // on first read).  Mirror that stored value here for the slider
            // display so UI reflects server state without racing the migration.
            float saved = settings.value("TciTxGain", "0.5").toString().toFloat();
            m_txMeter->setGain(std::clamp(saved, 0.0f, 1.0f));
        }
        connect(m_txMeter, &MeterSlider::gainChanged, this, [this](float g) {
            // TciServer::setTxGain() persists TciTxGain internally.
            emit tciTxGainChanged(g);
        });
        row->addWidget(m_txMeter, 1);

        outer->addLayout(row);
    }

    // Port + Enable row (bottom)
    auto* enableRow = new QHBoxLayout;
    enableRow->setContentsMargins(4, 2, 4, 2);
    enableRow->setSpacing(4);

    auto* portLabel = new QLabel("Port:");
    portLabel->setStyleSheet(kDimLabel);
    enableRow->addWidget(portLabel);

    m_tciPort = new QLineEdit(settings.value("TciPort", "50001").toString());
    m_tciPort->setStyleSheet(kInsetStyle);
    m_tciPort->setFixedWidth(46);
    m_tciPort->setAlignment(Qt::AlignCenter);
    enableRow->addWidget(m_tciPort);

    m_tciStatus = new QLabel("(stopped)");
    m_tciStatus->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
    enableRow->addWidget(m_tciStatus, 1);

    m_tciEnable = new QPushButton("Enable");
    m_tciEnable->setCheckable(true);
    m_tciEnable->setStyleSheet(kGreenToggle);
    m_tciEnable->setFixedSize(60, 22);
    {
        QSignalBlocker b(m_tciEnable);
        m_tciEnable->setChecked(
            settings.value("AutoStartTCI", "False").toString() == "True");
    }
    enableRow->addWidget(m_tciEnable);

    outer->addLayout(enableRow);

    connect(m_tciPort, &QLineEdit::editingFinished, this, [this]() {
        int port = m_tciPort->text().toInt();
        if (port < 1024 || port > 65535) {
            port = 50001;
            m_tciPort->setText("50001");
        }
        auto& ss = AppSettings::instance();
        ss.setValue("TciPort", QString::number(port));
        ss.save();
        // If running, restart with new port
        if (m_tciEnable->isChecked() && m_tciServer) {
            m_tciServer->stop();
            m_tciServer->start(static_cast<quint16>(port));
            updateTciStatus();
        }
    });

    connect(m_tciEnable, &QPushButton::toggled, this, [this](bool on) {
        int port = m_tciPort->text().toInt();
        if (port < 1024 || port > 65535) {
            port = 50001;
        }
        auto& ss = AppSettings::instance();
        ss.setValue("TciPort", QString::number(port));
        ss.setValue("AutoStartTCI", on ? "True" : "False");
        ss.save();
        if (m_tciServer) {
            if (on) {
                m_tciServer->start(static_cast<quint16>(port));
                // If bind failed, snap the button back off so the UI doesn't
                // claim the server is enabled while isRunning() is false.
                if (!m_tciServer->isRunning() && m_tciEnable) {
                    QSignalBlocker b(m_tciEnable);
                    m_tciEnable->setChecked(false);
                    m_tciStatus->setText("(port in use)");
                    m_tciStatus->setStyleSheet(
                        "QLabel { color: #cc3333; font-size: 10px; }");
                    ss.setValue("AutoStartTCI", "False");
                    ss.save();
                    emit tciToggled(false);
                    return;
                }
            } else {
                m_tciServer->stop();
            }
            updateTciStatus();
        }
        emit tciToggled(on);
    });
}

void TciApplet::updateTciStatus()
{
    if (!m_tciStatus) {
        return;
    }
    if (!m_tciServer || !m_tciServer->isRunning()) {
        m_tciStatus->setText("(stopped)");
        m_tciStatus->setStyleSheet("QLabel { color: #506070; font-size: 10px; }");
    } else {
        int n = m_tciServer->clientCount();
        m_tciStatus->setText(
            QStringLiteral(":%1 (%2 client%3)")
                .arg(m_tciServer->port())
                .arg(n)
                .arg(n == 1 ? "" : "s"));
        m_tciStatus->setStyleSheet(
            n > 0 ? "QLabel { color: #00b4d8; font-size: 10px; }"
                  : "QLabel { color: #8090a0; font-size: 10px; }");
    }
}

#endif // HAVE_WEBSOCKETS

void TciApplet::setRadioModel(RadioModel* model)
{
#ifdef HAVE_WEBSOCKETS
    m_model = model;
    if (!model) {
        return;
    }

    // Slice → DAX channel mapping drives both DAX and TCI RX indicators.
    // TCI RX1-4 carry the same DAX channels (PanadapterStream::daxAudioReady
    // fans out to both DaxBridge and TciServer), so reuse the DAX channel
    // assignments for the slice letters here.
    auto updateRxLabels = [this]() {
        for (int i = 0; i < kChannels; ++i) {
            m_rxStatus[i]->setText(QStringLiteral("\u2014"));
        }
        if (!m_model) {
            return;
        }
        for (auto* sl : m_model->slices()) {
            int ch = sl->daxChannel();
            if (ch >= 1 && ch <= kChannels) {
                m_rxStatus[ch - 1]->setText(
                    QString("Slice %1").arg(
                        SliceLabel::richText(sl->sliceId(), sl->letter())));
            }
        }
    };
    connect(model, &RadioModel::sliceAdded, this, [this, updateRxLabels](SliceModel* s) {
        connect(s, &SliceModel::daxChannelChanged, this, updateRxLabels);
        updateRxLabels();
    });
    for (auto* s : model->slices()) {
        connect(s, &SliceModel::daxChannelChanged, this, updateRxLabels);
    }
    updateRxLabels();

    auto updateTxLabel = [this]() {
        if (!m_model) {
            m_txStatus->setText(QStringLiteral("\u2014"));
            return;
        }
        for (auto* s : m_model->slices()) {
            if (s->isTxSlice()) {
                m_txStatus->setText(
                    QString("Slice %1").arg(
                        SliceLabel::richText(s->sliceId(), s->letter())));
                return;
            }
        }
        m_txStatus->setText(QStringLiteral("\u2014"));
    };
    connect(model, &RadioModel::sliceAdded, this, [this, updateTxLabel](SliceModel* s) {
        connect(s, &SliceModel::txSliceChanged, this, updateTxLabel);
        updateTxLabel();
    });
    for (auto* s : model->slices()) {
        connect(s, &SliceModel::txSliceChanged, this, updateTxLabel);
    }
    updateTxLabel();
#else
    (void)model;
#endif
}

void TciApplet::setTciServer(TciServer* tci)
{
#ifdef HAVE_WEBSOCKETS
    m_tciServer = tci;
    if (tci) {
        connect(tci, &TciServer::clientCountChanged,
                this, [this]() { updateTciStatus(); });
    }
    updateTciStatus();
#else
    (void)tci;
#endif
}

void TciApplet::setTciEnabled(bool on)
{
#ifdef HAVE_WEBSOCKETS
    if (m_tciEnable) {
        QSignalBlocker b(m_tciEnable);
        m_tciEnable->setChecked(on);
    }
    updateTciStatus();
#else
    (void)on;
#endif
}

void TciApplet::setTciRxLevel(int channel, float rms)
{
#ifdef HAVE_WEBSOCKETS
    if (channel < 1 || channel > kChannels) {
        return;
    }
    // Exponential smoothing: fast attack (α=0.4), slow decay (α=0.08)
    static float smoothed[kChannels]{};
    float& s = smoothed[channel - 1];
    float alpha = (rms > s) ? 0.4f : 0.08f;
    s = alpha * rms + (1.0f - alpha) * s;
    if (m_rxMeter[channel - 1]) {
        m_rxMeter[channel - 1]->setLevel(std::clamp(s * 2.0f, 0.0f, 1.0f));
    }
#else
    (void)channel;
    (void)rms;
#endif
}

void TciApplet::setTciTxLevel(float rms)
{
#ifdef HAVE_WEBSOCKETS
    if (m_txMeter) {
        m_txMeter->setLevel(std::clamp(rms * 2.0f, 0.0f, 1.0f));
    }
#else
    (void)rms;
#endif
}

} // namespace MasterSDR
