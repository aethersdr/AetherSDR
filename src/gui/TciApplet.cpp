#include "TciApplet.h"
#include "ControlAvailabilityRegistry.h"
#include "SliceLabel.h"
#include "core/ThemeManager.h"

#ifdef HAVE_WEBSOCKETS
#include "MeterSlider.h"
#include "CanonIndicators.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QMenu>
#include <QActionGroup>
#include <QAccessible>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>
#endif

namespace AetherSDR {

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
    theme::setContainer(this, QStringLiteral("applet/tci"));
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
        m_rxMeter[i]->setAccessibleName(tr("TCI RX %1 gain").arg(i + 1));
        m_rxMeter[i]->setAccessibleDescription(tr("Receive attenuation from 0 to 100 percent; meter shows output from -60 to 0 dBFS RMS"));
        m_rxMeter[i]->setToolTip(tr("RX gain: 0-100% (100% is unity)\nMeter: -60 to 0 dBFS RMS"));
        m_rxMeter[i]->setLogarithmicLevelEnabled(true);
        {
            auto key = QStringLiteral("TciRxGain%1").arg(i + 1);
            float saved = settings.value(key, "0.5").toString().toFloat();
            m_rxMeter[i]->setGain(std::isfinite(saved) ? std::clamp(saved, 0.0f, 1.0f) : 0.5f);
        }
        connect(m_rxMeter[i], &MeterSlider::gainChanged, this, [this, i](float g) {
            auto& ss = AppSettings::instance();
            ss.setValue(QStringLiteral("TciRxGain%1").arg(i + 1), QString::number(g, 'f', 2));
            ss.save();
            emit tciRxGainChanged(i + 1, g);
        });
        row->addWidget(m_rxMeter[i], 1);

        // Wrap each row in a container widget so setMaxDaxChannels() can hide
        // whole rows on radios with fewer than kChannels slices (#4854 review).
        m_rxRow[i] = new QWidget;
        m_rxRow[i]->setLayout(row);
        outer->addWidget(m_rxRow[i]);
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
        m_txMeter->setAccessibleName(tr("TCI TX gain"));
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

        // Right-click on the TX slider → overflow-mode picker.  Lets users
        // choose how out-of-range (>1.0) samples from TCI clients are
        // handled before the radio sees them.  Default Clip preserves the
        // legacy defensive limiter; NaNGuard / Measure are progressively
        // less destructive for digital-mode tone fidelity.
        m_txMeter->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_txMeter, &QWidget::customContextMenuRequested,
                this, [this](const QPoint& pos) {
            int saved = AppSettings::instance()
                          .value("TciTxOverflowMode", "0").toString().toInt();
            saved = std::clamp(saved, 0, 2);

            QMenu menu(this);
            menu.setTitle("TX overflow handling");
            // Every entry below carries the explanation of what its overflow
            // mode does to the samples; without the opt-in Qt renders none of
            // them (#5546).
            menu.setToolTipsVisible(true);
            auto* group = new QActionGroup(&menu);
            group->setExclusive(true);

            struct Item { int mode; const char* label; const char* tip; };
            const Item items[] = {
                { 0, "Clip (saturating ±1.0)",
                  "Hard-clamp overshoots to ±1.0.  Defensive default; "
                  "introduces harmonics on overshoot but protects downstream "
                  "int16 conversion." },
                { 1, "NaN guard (zero NaN/Inf only)",
                  "Pass samples through bit-exact; only zero pathological "
                  "NaN/Inf values.  Preserves digital-mode tone fidelity; "
                  "out-of-range floats reach the radio." },
                { 2, "Measure only (true bypass)",
                  "Never mutate samples.  Count overshoots for telemetry; "
                  "the downstream int16 conversion still clamps in the "
                  "radio-native DAX route." },
            };

            for (const auto& it : items) {
                auto* act = menu.addAction(QString::fromUtf8(it.label));
                act->setToolTip(QString::fromUtf8(it.tip));
                act->setCheckable(true);
                act->setChecked(it.mode == saved);
                act->setData(it.mode);
                group->addAction(act);
            }

            connect(&menu, &QMenu::triggered, this, [this](QAction* act) {
                if (!act) return;
                emit tciTxOverflowModeChanged(act->data().toInt());
            });

            menu.exec(m_txMeter->mapToGlobal(pos));
        });

        row->addWidget(m_txMeter, 1);

        outer->addLayout(row);
    }

    auto* recordingRow = new QHBoxLayout;
    auto* lineOutRow = new QHBoxLayout;
    lineOutRow->setContentsMargins(4, 2, 4, 2);
    m_lineOutIncludeTx = new QCheckBox(tr("Line Out TX (Experimental)"), this);
    m_lineOutIncludeTx->setObjectName(QStringLiteral("tciLineOutIncludeTx"));
    m_lineOutIncludeTx->setAccessibleName(tr("TCI Line Out TX recording"));
    m_lineOutIncludeTx->setAccessibleDescription(tr(
        "Include TX monitor audio present in the radio speaker mix in Line Out. "
        "Does not enable MON or add microphone audio. When off, Line Out is silent "
        "during radio transmit, including simultaneous receive audio."));
    m_lineOutIncludeTx->setToolTip(m_lineOutIncludeTx->accessibleDescription());
    m_lineOutIncludeTx->setEnabled(false);
    applyCanonSheet(m_lineOutIncludeTx, [] {
        return QStringLiteral("QCheckBox { color: {{color.canon.inkSoft}}; font-size: 11px; }")
            + canonIndicatorRules();
    });
    lineOutRow->addWidget(m_lineOutIncludeTx);
    lineOutRow->addStretch();
    outer->addLayout(lineOutRow);
    connect(m_lineOutIncludeTx, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_tciServer) { m_tciServer->setLineOutIncludeTx(enabled); }
    });
    recordingRow->setContentsMargins(4, 2, 4, 2);
    m_microphoneRecording = new QCheckBox(tr("Mic During TX (Experimental)"), this);
    m_microphoneRecording->setObjectName(QStringLiteral("tciMicrophoneRecording"));
    m_microphoneRecording->setAccessibleName(tr("TCI microphone recording"));
    m_microphoneRecording->setAccessibleDescription(tr(
        "Replace the TX receiver's TCI audio with the radio microphone during transmit. "
        "Affects all subscribers on that receiver; leave off for RX-only digital clients."));
    m_microphoneRecording->setToolTip(m_microphoneRecording->accessibleDescription());
    m_microphoneRecording->setEnabled(false);
    applyCanonSheet(m_microphoneRecording, [] {
        return QStringLiteral("QCheckBox { color: {{color.canon.inkSoft}}; font-size: 11px; }")
            + canonIndicatorRules();
    });
    recordingRow->addWidget(m_microphoneRecording);
    recordingRow->addStretch();
    outer->addLayout(recordingRow);
    connect(m_microphoneRecording, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_tciServer) { m_tciServer->setMicrophoneRecordingEnabled(enabled); }
    });

    auto* microphoneRow = new QHBoxLayout;
    microphoneRow->setContentsMargins(4, 1, 4, 1);
    microphoneRow->setSpacing(4);
    auto* microphoneLabel = new QLabel(tr("MIC:"), this);
    microphoneLabel->setFixedWidth(40);
    applyCanonSheet(microphoneLabel, [] {
        return QStringLiteral("QLabel { color: {{color.canon.muted}}; font-size: 11px; }");
    });
    microphoneRow->addWidget(microphoneLabel);
    m_microphoneLevelLabel = new QLabel(QStringLiteral("\u2014"), this);
    m_microphoneLevelLabel->setObjectName(QStringLiteral("tciMicrophoneLevel"));
    m_microphoneLevelLabel->setAccessibleName(tr("TCI mic recording RMS level"));
    m_microphoneLevelLabel->setAccessibleDescription(tr(
        "Post-fader microphone RMS in dBFS; CLIP indicates a sample peak at or above full scale"));
    m_microphoneLevelLabel->setToolTip(m_microphoneLevelLabel->accessibleDescription());
    m_microphoneLevelLabel->setFixedWidth(40);
    applyCanonSheet(m_microphoneLevelLabel, [] {
        return QStringLiteral("QLabel { color: {{color.canon.inkSoft}}; font-size: 10px; }");
    });
    microphoneRow->addWidget(m_microphoneLevelLabel);
    m_microphoneMeter = new MeterSlider(this);
    m_microphoneMeter->setObjectName(QStringLiteral("tciMicrophoneGain"));
    m_microphoneMeter->setAccessibleName(tr("TCI mic recording gain"));
    m_microphoneMeter->setAccessibleDescription(tr(
        "Enable Mic During TX (Experimental) to adjust the recording feed. "
        "Zero to 100 percent, attenuation only; does not change radio MIC gain or RF output."));
    m_microphoneMeter->setToolTip(m_microphoneMeter->accessibleDescription());
    m_microphoneMeter->setLogarithmicLevelEnabled(true);
    m_microphoneMeter->setGain(1.0f);
    m_microphoneMeter->setDragValueFormatter([](float gain) {
        return gain > 0.0f ? QStringLiteral("%1 dB").arg(20.0f * std::log10(gain), 0, 'f', 1)
                           : tr("Mute");
    });
    connect(m_microphoneMeter, &MeterSlider::gainChanged, this, [this](float gain) {
        if (m_tciServer) { m_tciServer->setMicrophoneRecordingGain(gain); }
    });
    microphoneRow->addWidget(m_microphoneMeter, 1);
    m_microphoneRow = new QWidget(this);
    m_microphoneRow->setObjectName(QStringLiteral("tciMicrophoneRow"));
    m_microphoneRow->setLayout(microphoneRow);
    outer->addWidget(m_microphoneRow);
    m_microphoneMeterTimer = new QTimer(this);
    m_microphoneMeterTimer->setInterval(100);
    connect(m_microphoneMeterTimer, &QTimer::timeout, this, [this] {
        const float rms = m_microphoneSamples > 0
            ? static_cast<float>(std::sqrt(m_microphoneSquareSum / m_microphoneSamples)) : 0.0f;
        m_microphoneSquareSum = 0.0;
        m_microphoneSamples = 0;
        m_microphoneMeter->setLevel(rms);
        const float output = rms * m_microphoneMeter->gain();
        const QString text = m_microphoneClipped ? tr("CLIP") : (output > 0.0f
            ? QString::number(20.0f * std::log10(output), 'f', 1) : QStringLiteral("\u2014"));
        m_microphoneClipped = false;
        if (text != m_microphoneLevelLabel->text()) {
            const bool clipTransition = (text == tr("CLIP"))
                != (m_microphoneLevelLabel->text() == tr("CLIP"));
            m_microphoneLevelLabel->setText(text);
            if (clipTransition) {
                QAccessibleEvent event(m_microphoneLevelLabel, QAccessible::NameChanged);
                QAccessible::updateAccessibility(&event);
            }
        }
    });
    updateMicrophoneRecordingControls();

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
    m_tciPort->setObjectName(QStringLiteral("tciPort"));
    m_tciPort->setAccessibleName(tr("TCI port"));
    m_tciPort->setAccessibleDescription(tr("TCP port the TCI server listens on"));
    enableRow->addWidget(m_tciPort);

    m_tciStatus = new QLabel("(stopped)");
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_tciStatus, "QLabel { color: {{color.background.3}}; font-size: 10px; }");
    enableRow->addWidget(m_tciStatus, 1);

    const bool tciAutoStart = settings.value("AutoStartTCI", "False").toString() == "True";
    m_tciEnable = new QPushButton(tciAutoStart ? "Enabled" : "Disabled");
    m_tciEnable->setCheckable(true);
    m_tciEnable->setObjectName(QStringLiteral("tciEnable"));
    m_tciEnable->setAccessibleName(tr("TCI server enable"));
    m_tciEnable->setAccessibleDescription(tr("Start or stop the TCI server"));
    m_tciEnable->setStyleSheet(kGreenToggle);
    m_tciEnable->setFixedSize(76, 22);
    {
        QSignalBlocker b(m_tciEnable);
        m_tciEnable->setChecked(tciAutoStart);
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
        m_tciEnable->setText(on ? "Enabled" : "Disabled");
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
                    m_tciEnable->setText("Disabled");
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

void TciApplet::updateMicrophoneRecordingControls()
{
    const bool enabled = m_tciServer && m_tciServer->microphoneRecordingEnabled();
    m_microphoneRow->setVisible(enabled);
    const bool supported = m_model && m_model->backendCapabilities().microphoneCapture.has_value();
    m_microphoneMeter->setEnabled(enabled && supported);
    m_microphoneLevelLabel->setEnabled(enabled && supported);
    m_microphoneSquareSum = 0.0;
    m_microphoneSamples = 0;
    m_microphoneClipped = false;
    m_microphoneMeter->setLevel(0.0f);
    m_microphoneLevelLabel->setText(QStringLiteral("\u2014"));
    if (enabled) { m_microphoneMeterTimer->start(); }
    else { m_microphoneMeterTimer->stop(); }
}

void TciApplet::updateTciStatus()
{
    if (!m_tciStatus) {
        return;
    }
    if (!m_tciServer || !m_tciServer->isRunning()) {
        m_tciStatus->setText("(stopped)");
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_tciStatus, "QLabel { color: {{color.background.3}}; font-size: 10px; }");
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
    delete m_microphoneAvailability;
    m_microphoneAvailability = nullptr;
    if (!model) {
        m_lineOutIncludeTx->setEnabled(false);
        m_lineOutIncludeTx->setAccessibleDescription(tr("Line Out requires a connected radio and a TCI server, not PC Audio."));
        m_microphoneRecording->setEnabled(false);
        m_microphoneRecording->setAccessibleDescription(tr("Microphone capture requires a supported connected radio."));
        updateMicrophoneRecordingControls();
        return;
    }
    m_microphoneAvailability = new ControlAvailabilityRegistry(*model, this);
    m_microphoneAvailability->registerWidget(m_lineOutIncludeTx,
        tr("Line Out requires a connected radio and a TCI server, not PC Audio."),
        [this](bool connected, const RadioCapabilities&) { return connected && m_tciServer; },
        [this] { return m_tciServer && m_tciServer->lineOutIncludeTx(); }, false);
    m_microphoneAvailability->registerWidget(m_microphoneRecording,
        tr("Microphone capture requires a supported connected radio and a TCI server."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && m_tciServer && caps.microphoneCapture.has_value();
        }, [this] { return m_tciServer && m_tciServer->microphoneRecordingEnabled(); }, false);
    connect(model, &RadioModel::capabilitiesChanged, this,
        [this](bool, const RadioCapabilities&) { updateMicrophoneRecordingControls(); });

    // Slice → DAX channel mapping drives both DAX and TCI RX indicators.
    // TCI RX1-8 carry the same DAX channels (PanadapterStream::daxPcmReady
    // fans out to both DaxBridge and TciServer), so reuse the DAX channel
    // assignments for the slice letters here. The TCI receiver (trx) index is
    // NOT capped at 4: trx is a positional slice index bounded by
    // slices.size() (up to 8 on a 6700), and TciServer::onDaxPcmReady /
    // setRxChannelGain already span channels 1-8 — so DAX 5-8 do have TCI
    // representation. (The old "RX1-4" was stale from the 4-DAX era.)
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
    if (m_tciServer) {
        disconnect(m_tciServer, &TciServer::lineOutIncludeTxChanged, this, nullptr);
        disconnect(m_tciServer, &TciServer::microphoneRecordingEnabledChanged, this, nullptr);
        disconnect(m_tciServer, &TciServer::microphoneRecordingGainChanged, this, nullptr);
        disconnect(m_tciServer, &TciServer::microphoneRecordingLevel, this, nullptr);
        disconnect(m_tciServer, &TciServer::clientCountChanged, this, nullptr);
    }
    m_tciServer = tci;
    {
        const QSignalBlocker blocker(m_lineOutIncludeTx);
        m_lineOutIncludeTx->setChecked(tci && tci->lineOutIncludeTx());
    }
    m_microphoneMeter->setGain(tci ? tci->microphoneRecordingGain() : 1.0f);
    m_microphoneRecording->setEnabled(false);
    if (m_microphoneAvailability) { m_microphoneAvailability->refreshEngaged(); }
    {
        const QSignalBlocker blocker(m_microphoneRecording);
        m_microphoneRecording->setChecked(tci && tci->microphoneRecordingEnabled());
    }
    if (tci) {
        connect(tci, &TciServer::lineOutIncludeTxChanged, this, [this](bool enabled) {
            const QSignalBlocker blocker(m_lineOutIncludeTx);
            m_lineOutIncludeTx->setChecked(enabled);
            if (m_microphoneAvailability) { m_microphoneAvailability->refreshEngaged(); }
        });
        connect(tci, &TciServer::microphoneRecordingEnabledChanged, this, [this](bool enabled) {
            const QSignalBlocker blocker(m_microphoneRecording);
            m_microphoneRecording->setChecked(enabled);
            updateMicrophoneRecordingControls();
        });
        connect(tci, &TciServer::microphoneRecordingGainChanged,
                m_microphoneMeter, &MeterSlider::setGain);
        connect(tci, &TciServer::microphoneRecordingLevel, this, [this](float rms, quint64 samples, bool clipped) {
            if (m_microphoneMeter->isEnabled()) {
                m_microphoneSquareSum += static_cast<double>(rms) * rms * samples;
                m_microphoneSamples += samples;
                m_microphoneClipped |= clipped;
            }
        });
        connect(tci, &TciServer::clientCountChanged,
                this, [this]() { updateTciStatus(); });
    }
    updateMicrophoneRecordingControls();
    updateTciStatus();
#else
    (void)tci;
#endif
}

// Hide RX rows above the radio's slice capacity (mirrors DaxApplet). kChannels
// stays the allocation; we only toggle visibility. Compiles to a no-op when
// WebSocket support is absent (the applet is an empty widget then). (#4854)
void TciApplet::setMaxDaxChannels(int n)
{
#ifdef HAVE_WEBSOCKETS
    n = qBound(1, n, kChannels);
    if (n == m_maxDaxChannels)
        return;
    m_maxDaxChannels = n;
    for (int i = 0; i < kChannels; ++i) {
        if (m_rxRow[i])
            m_rxRow[i]->setVisible(i < n);
    }
#else
    (void)n;
#endif
}

void TciApplet::setTciEnabled(bool on)
{
#ifdef HAVE_WEBSOCKETS
    if (m_tciEnable) {
        QSignalBlocker b(m_tciEnable);
        m_tciEnable->setChecked(on);
        m_tciEnable->setText(on ? "Enabled" : "Disabled");
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
    if (m_rxMeter[channel - 1]) {
        m_rxMeter[channel - 1]->setLevel(rms);
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

} // namespace AetherSDR
