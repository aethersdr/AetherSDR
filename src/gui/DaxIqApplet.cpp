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
#include "core/ThemeManager.h"

namespace AetherSDR {

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
    theme::setContainer(this, QStringLiteral("applet/daxiq"));
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
        AetherSDR::ThemeManager::instance().applyStyleSheet(m_iqMeter[i], "QProgressBar { background: {{color.background.0}}; border: 1px solid {{color.background.1}}; border-radius: 2px; }"
            "QProgressBar::chunk { background: {{color.accent}}; }");
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
                // Sync the model's desired rate to the combo before enabling, so a
                // rate chosen while off (or restored from settings) is applied to the
                // freshly-created stream instead of lost to the radio's 48k default.
                emit iqRateChanged(i + 1, m_iqRateCombo[i]->currentData().toInt());
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
        restoreEnabledChannels();
    });

    // The radio can already be connected by the time this applet wires up the
    // handler above: the connection completes asynchronously during the long
    // startup constructor, often before setRadioModel() runs, so the initial
    // connectionStateChanged(true) is emitted and missed -> persisted channels
    // never restore. Cover that ordering explicitly.
    if (model->isConnected()) {
        restoreEnabledChannels();
    }

    // Wire DAX IQ stream state changes → sync On/Off buttons
    connect(&model->daxIqModel(), &DaxIqModel::streamChanged, this, [this](int ch) {
        if (ch < 1 || ch > kChannels) {
            return;
        }
        int idx = ch - 1;
        bool exists = m_model->daxIqModel().stream(ch).exists;
        m_iqEnable[idx]->setText(exists ? "On" : "Off");
        m_iqEnable[idx]->setStyleSheet(exists ? kIqBtnOn : kIqBtnOff);
        // Zero the meter whenever the channel is not actively bound to a pan: it
        // may be Off (exists==false) OR enabled-but-unbound (pan==0x0, e.g. the
        // pan's daxiq_channel was moved to another IQ channel). No IQ data flows
        // in either case, so the bar must drop to 0 instead of freezing.
        const QString pan = exists ? m_model->daxIqModel().stream(ch).panId : QString();
        const bool bound = exists && !pan.isEmpty()
                           && pan != QStringLiteral("0x0") && pan != QStringLiteral("0");
        if (!bound) {
            m_iqMeterDb[idx] = -70.0f;
            m_iqMeter[idx]->setValue(0);
        }

        // Sync rate combo from radio state ONLY while the stream exists and is
        // not mid-rate-settling. On disable the model resets the stream's
        // sampleRate to its 48k default; copying that into the combo would
        // clobber the rate the user selected. During a non-default-rate enable
        // the stream reports 48k transiently (rateSettling) before the real rate
        // arrives; syncing then would flip the combo to 48k and back. In both
        // cases the combo holds user intent and is re-applied on the next enable.
        // (The On/Off button above still syncs every emit, settling or not.)
        if (exists && !m_model->daxIqModel().stream(ch).rateSettling) {
            int rate = m_model->daxIqModel().stream(ch).sampleRate;
            QSignalBlocker sb(m_iqRateCombo[idx]);
            for (int i = 0; i < m_iqRateCombo[idx]->count(); ++i) {
                if (m_iqRateCombo[idx]->itemData(i).toInt() == rate) {
                    m_iqRateCombo[idx]->setCurrentIndex(i);
                    break;
                }
            }
        }
    });
}

void DaxIqApplet::restoreEnabledChannels()
{
    // Re-create the persisted-enabled IQ streams a short moment after connect,
    // once session/stream setup has settled. Idempotent (skips channels whose
    // stream already exists) so calling it from both the connect handler and the
    // already-connected path in setRadioModel cannot double-create. The `exists`
    // guard lags the radio round-trip, though, so two timers scheduled within the
    // same settle window would both pass it and double-create; collapse overlapping
    // calls with a pending flag, cleared when the timer fires.
    if (m_restorePending) {
        return;
    }
    m_restorePending = true;
    QTimer::singleShot(1500, this, [this]() {
        m_restorePending = false;
        if (!m_model || !m_model->isConnected()) {
            return;
        }
        auto& ss = AppSettings::instance();
        for (int i = 0; i < kChannels; ++i) {
            if (!m_iqEnable[i]) {
                continue;
            }
            if (ss.value(QStringLiteral("DaxIqEnabled%1").arg(i + 1), "False").toString() != "True") {
                continue;
            }
            if (m_model->daxIqModel().stream(i + 1).exists) {
                continue;  // already restored
            }
            // Restore the saved rate too: sync the model's desired rate to the
            // combo (restored from DaxIqRate%1 in buildUI) so the stream comes up
            // at the persisted rate instead of the radio's 48k default.
            emit iqRateChanged(i + 1, m_iqRateCombo[i]->currentData().toInt());
            emit iqEnableRequested(i + 1);
            // The On button is driven by the streamChanged sync once the stream
            // actually exists; don't pre-set it here so a restore that runs before
            // the createStream wiring is in place can't show a dead "On".
        }
    });
}

void DaxIqApplet::setDaxIqLevel(int channel, float rms)
{
    if (channel < 1 || channel > kChannels) {
        return;
    }
    const int i = channel - 1;

    // Ignore stale level updates when the channel is off OR unbound from a pan
    // (pan==0x0): no IQ data flows, and a levelReady queued just before the
    // disable/unbind would otherwise freeze the bar at its last value (random,
    // timing-dependent). Force the bar to 0 and reset ballistics in that case.
    if (!m_model) {
        m_iqMeterDb[i] = -70.0f; m_iqMeter[i]->setValue(0); return;
    }
    const auto& st = m_model->daxIqModel().stream(channel);
    const bool live = m_iqEnable[i]->text() == QStringLiteral("On")
                      && st.exists && !st.panId.isEmpty()
                      && st.panId != QStringLiteral("0x0") && st.panId != QStringLiteral("0");
    if (!live) {
        m_iqMeterDb[i] = -70.0f;
        m_iqMeter[i]->setValue(0);
        return;
    }

    // DAX-IQ samples are the radio's raw baseband amplitude (int16 full-scale,
    // ~32768), NOT normalized [-1,1] like DAX audio. The old `rms * 200` assumed
    // normalized IQ and pegged the bar on real data (verified live on a FLEX-6700:
    // 48 kHz IQ noise-floor RMS ~16 -> -66 dBFS, a strong AM carrier ~43 -> -58
    // dBFS; rms * 200 saturates above 0.5). A wideband IQ RMS is noise-dominated,
    // so this is really a level/overload meter: map RMS to dBFS vs int16 full-
    // scale over a [-70, -10] dBFS window, so normal signals sit low and the bar
    // only fills as the stream approaches digital overload.
    constexpr float kFullScale = 32768.0f;  // int16 IQ full-scale
    constexpr float kFloorDb   = -70.0f;    // 0% of bar
    constexpr float kCeilDb    = -10.0f;    // 100% of bar (overload headroom)
    const float dbfs = (rms > 1e-4f) ? 20.0f * std::log10(rms / kFullScale) : kFloorDb;

    // Attack-fast / decay-slow ballistics (mirrors DaxApplet RX-meter feel).
    const float a = (dbfs > m_iqMeterDb[i]) ? 0.5f : 0.15f;
    m_iqMeterDb[i] = a * dbfs + (1.0f - a) * m_iqMeterDb[i];

    const float frac = (m_iqMeterDb[i] - kFloorDb) / (kCeilDb - kFloorDb);
    m_iqMeter[i]->setValue(static_cast<int>(std::clamp(frac * 100.0f, 0.0f, 100.0f)));
}

} // namespace AetherSDR
