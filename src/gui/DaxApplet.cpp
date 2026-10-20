#include "DaxApplet.h"
#include "MeterSlider.h"
#include "ControlAvailabilityRegistry.h"
#include "SliceLabel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QAccessible>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>
#include <initializer_list>
#include <memory>

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

DaxApplet::~DaxApplet() = default;

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
    // one. The hosted-device Enable button and meters would be inert here;
    // stream ownership controls below remain available independently of a driver.
    // The setup pointer lives in Help → Configuring Data Modes.
    auto* winNote = new QLabel(
        tr("No built-in DAX driver on Windows.\n"
           "Use TCI, or SmartSDR DAX."));
    winNote->setObjectName(QStringLiteral("daxWindowsNote"));
    winNote->setAccessibleName(tr("DAX driver not shipped on Windows"));
    winNote->setWordWrap(true);
    winNote->setStyleSheet(
        "QLabel { color: #d0a040; font-size: 11px; padding: 4px 6px; }");
    outer->addWidget(winNote);
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

    // RX channel meter/sliders (DAX 1-8)
    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(4, 1, 4, 1);
        row->setSpacing(4);
        auto* chLabel = new QLabel(QString("DAX %1:").arg(i + 1));
        chLabel->setStyleSheet(kDimLabel);
        chLabel->setFixedWidth(40);
        row->addWidget(chLabel);

        m_daxRxStatus[i] = new QLabel(QStringLiteral("\u2014"));
        m_daxRxStatus[i]->setObjectName(QStringLiteral("daxRxStatus%1").arg(i+1));
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

        // Wrap each row in a container widget so setMaxDaxChannels() can hide
        // whole rows on radios with fewer than kChannels slices (#4854 review).
        m_daxRxRow[i] = new QWidget;
        m_daxRxRow[i]->setLayout(row);
        outer->addWidget(m_daxRxRow[i]);
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
    buildOwnershipUI(outer);
#ifdef Q_OS_WIN
    outer->addStretch();
#endif
}

// Hide RX rows above the radio's slice capacity so a 2-slice (6300/6400) or
// 4-slice (6600/8600) radio doesn't show dead DAX rows with live-but-routing-
// nowhere gain sliders. kChannels stays the allocation; we only toggle
// visibility. Windows hosted-driver builds leave m_daxRxRow[] null — guarded. (#4854)
void DaxApplet::setMaxDaxChannels(int n)
{
    n = qBound(1, n, kChannels);
    if (n == m_maxDaxChannels)
        return;
    m_maxDaxChannels = n;
    for (int i = 0; i < kChannels; ++i) {
        if (m_daxRxRow[i])
            m_daxRxRow[i]->setVisible(i < n);
    }
}

void DaxApplet::buildOwnershipUI(QVBoxLayout* outer)
{
    auto* ownership = new QWidget(this);
    ownership->setObjectName(QStringLiteral("daxTxOwnership"));
    auto* layout = new QVBoxLayout(ownership);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);
    ThemeManager::instance().applyStyleSheet(ownership, QStringLiteral(
        "QWidget#daxTxOwnership { background: {{color.canon.nested}};"
        " border: 1px solid {{color.canon.line}}; }"
        "QLabel { color: {{color.canon.inkSoft}}; font-size: 11px; }"
        "QPushButton { background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 3px;"
        " padding: 2px 4px; font-size: 11px; font-weight: bold;"
        " color: {{color.canon.cyan}}; }"
        "QPushButton:hover { background: {{color.canon.nested}};"
        " color: {{color.canon.aqua}}; }"
        "QPushButton:focus { border: 1px solid {{color.canon.aqua}}; }"
        "QPushButton:disabled { color: {{color.control.unavailable}};"
        " border-color: {{color.canon.line}}; }"));

    m_daxTxOwnershipStatus = new QLabel(ownership);
    m_daxTxOwnershipStatus->setObjectName(QStringLiteral("daxTxOwnershipStatus"));
    m_daxTxOwnershipStatus->setTextFormat(Qt::PlainText);
    m_daxTxOwnershipStatus->setWordWrap(true);
    layout->addWidget(m_daxTxOwnershipStatus);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    m_daxTxTake = new QPushButton(tr("Take DAX TX"), ownership);
    m_daxTxTake->setObjectName(QStringLiteral("daxTxTake"));
    m_daxTxTake->setAccessibleName(tr("Take DAX TX"));
    m_daxTxTake->setAccessibleDescription(tr(
        "Request selection of this client's DAX TX stream. This does not transmit."));
    m_daxTxTake->setToolTip(m_daxTxTake->accessibleDescription());
    m_daxTxTake->setFixedHeight(22);
    m_daxTxTake->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    actions->addWidget(m_daxTxTake, 1);
    m_daxTxRelease = new QPushButton(tr("Release DAX TX"), ownership);
    m_daxTxRelease->setObjectName(QStringLiteral("daxTxRelease"));
    m_daxTxRelease->setAccessibleName(tr("Release DAX TX"));
    m_daxTxRelease->setAccessibleDescription(tr(
        "Yield this client's DAX TX stream. This does not change the TX slice."));
    m_daxTxRelease->setToolTip(m_daxTxRelease->accessibleDescription());
    m_daxTxRelease->setFixedHeight(22);
    m_daxTxRelease->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    actions->addWidget(m_daxTxRelease, 1);
    layout->addLayout(actions);
    outer->addWidget(ownership);

    connect(m_daxTxTake, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->requestDaxTxOwnership(true);
        }
        updateDaxTxOwnership();
    });
    connect(m_daxTxRelease, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->requestDaxTxOwnership(false);
        }
        updateDaxTxOwnership();
    });
    updateDaxTxOwnership();
}

void DaxApplet::updateDaxTxOwnership()
{
    m_ownershipAvailability.reset();
    const QJsonObject snapshot = m_model ? m_model->daxTxOwnershipSnapshot() : QJsonObject();
    const QString state = snapshot.value(QStringLiteral("state")).toString();
    QString stateText;
    if (state == QLatin1String("creating")) {
        stateText = tr("Creating");
    } else if (state == QLatin1String("taking")) {
        stateText = tr("Taking");
    } else if (state == QLatin1String("releasing")) {
        stateText = tr("Releasing");
    } else if (state == QLatin1String("refused")) {
        stateText = tr("Refused");
    } else if (state == QLatin1String("timed-out")) {
        stateText = tr("Timed out");
    }

    QString observation = tr("Unknown");
    if (!m_model || !snapshot.value(QStringLiteral("available")).toBool()) {
        observation = tr("Unavailable");
    } else if (snapshot.value(QStringLiteral("txKnown")).toBool()) {
        observation = snapshot.value(QStringLiteral("owned")).toBool()
            ? tr("Owned") : tr("Released");
    } else if (state == QLatin1String("no-stream")) {
        observation = tr("No stream");
    }
    QString status = tr("DAX TX: %1").arg(observation);
    if (!stateText.isEmpty()) {
        status += tr(" · %1").arg(stateText);
    }
    if (snapshot.value(QStringLiteral("txKnown")).toBool()
        && snapshot.value(QStringLiteral("owned")).toBool()
        && !snapshot.value(QStringLiteral("ready")).toBool()) {
        status += tr(" · Audio blocked");
    }
    const QString error = snapshot.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        status += QLatin1Char('\n') + error;
    }
    const bool changed = m_daxTxOwnershipStatus->text() != status;
    m_daxTxOwnershipStatus->setText(status);
    m_daxTxOwnershipStatus->setAccessibleName(status);
    m_daxTxOwnershipStatus->setAccessibleDescription(
        snapshot.value(QStringLiteral("ready")).toBool()
            ? tr("Radio-confirmed stream ownership permits DAX transmit audio. Transmit still requires a separate operator action.")
            : tr("DAX transmit audio is blocked. Ownership is confirmed only by radio status."));
    if (changed) {
        QAccessibleEvent event(m_daxTxOwnershipStatus, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }

    m_daxTxTake->setAccessibleDescription(tr(
        "Request selection of this client's DAX TX stream. This does not transmit."));
    m_daxTxTake->setToolTip(m_daxTxTake->accessibleDescription());
    m_daxTxRelease->setAccessibleDescription(tr(
        "Yield this client's DAX TX stream. This does not change the TX slice."));
    m_daxTxRelease->setToolTip(m_daxTxRelease->accessibleDescription());
    const QString noModelReason = tr("Connect a supported radio to manage DAX TX stream ownership.");
    if (!m_model) {
        for (QPushButton* button : {m_daxTxTake, m_daxTxRelease}) {
            button->setEnabled(false);
            button->setToolTip(noModelReason);
            button->setAccessibleDescription(noModelReason);
            ThemeManager::instance().setWidgetForegroundToken(
                button, QStringLiteral("color.control.unavailable"));
        }
        return;
    }

    // Snapshot guard reasons change with pending work, TX activity and stream
    // identity. Re-register as one unit so both accessible channels stay current.
    m_ownershipAvailability = std::make_unique<ControlAvailabilityRegistry>(*m_model, this);
    const auto registerAction = [this, &snapshot](QPushButton* button, bool take) {
        const bool canRequest = snapshot.value(take ? QStringLiteral("canTake")
                                                   : QStringLiteral("canRelease")).toBool();
        const QString reason = snapshot.value(take ? QStringLiteral("takeReason")
                                                   : QStringLiteral("releaseReason")).toString();
        const bool engaged = snapshot.value(QStringLiteral("txKnown")).toBool()
            && snapshot.value(QStringLiteral("owned")).toBool() == take;
        m_ownershipAvailability->registerWidget(button, reason.isEmpty()
            ? tr("DAX TX ownership is not available in the current radio state.") : reason,
            [canRequest](bool, const RadioCapabilities&) { return canRequest; },
            [engaged] { return engaged; }, false);
    };
    registerAction(m_daxTxTake, true);
    registerAction(m_daxTxRelease, false);
}

void DaxApplet::setRadioModel(RadioModel* model)
{
    m_ownershipAvailability.reset();
    m_modelBinding.reset();
    m_model = model;
    if (!model) {
        updateReceiveLabels();
        updateTxSliceLabel();
        updateDaxTxOwnership();
        return;
    }

    m_modelBinding = std::make_unique<QObject>(this);
    QObject* binding = m_modelBinding.get();
    connect(model, &RadioModel::daxTxOwnershipChanged, binding,
            [this] { updateDaxTxOwnership(); });
    connect(model, &RadioModel::capabilitiesChanged, binding,
            [this] { updateDaxTxOwnership(); });
    connect(model, &RadioModel::connectionStateChanged, binding,
            [this] { updateDaxTxOwnership(); });
    connect(model, &QObject::destroyed, binding, [this] {
        m_model = nullptr;
        updateReceiveLabels();
        updateTxSliceLabel();
        updateDaxTxOwnership();
    });

    if (m_daxTxMeter) {
        auto* availability = new ControlAvailabilityRegistry(*model, binding);
        availability->registerWidget(m_daxTxMeter,
            tr("DAX transmit audio is unavailable on this receive-only radio."),
            [](bool connected, const RadioCapabilities& caps) {
                return !connected || (caps.canTransmit && caps.hasDaxStreams);
            });
    }

    const auto bindSlice = [this, binding](SliceModel* slice) {
        connect(slice, &SliceModel::daxChannelChanged, binding,
                [this] { updateReceiveLabels(); });
        connect(slice, &SliceModel::txSliceChanged, binding, [this] {
            updateTxSliceLabel();
            updateDaxTxOwnership();
        });
        updateReceiveLabels();
        updateTxSliceLabel();
        updateDaxTxOwnership();
    };
    connect(model, &RadioModel::sliceAdded, binding, bindSlice);
    connect(model, &RadioModel::sliceRemoved, binding, [this] {
        updateReceiveLabels();
        updateTxSliceLabel();
        updateDaxTxOwnership();
    });
    for (SliceModel* slice : model->slices()) {
        bindSlice(slice);
    }
    updateReceiveLabels();
    updateTxSliceLabel();
    updateDaxTxOwnership();
}

void DaxApplet::updateReceiveLabels()
{
    if (m_model && m_nativeReceiveRouting) {
        return;
    }
    for (QLabel* label : m_daxRxStatus) {
        if (label) {
            label->setText(QStringLiteral("—"));
        }
    }
    if (!m_model) {
        return;
    }
    for (SliceModel* slice : m_model->slices()) {
        const int channel = slice->daxChannel();
        if (channel >= 1 && channel <= kChannels && m_daxRxStatus[channel - 1]) {
            m_daxRxStatus[channel - 1]->setText(tr("Slice %1").arg(
                SliceLabel::richText(slice->sliceId(), slice->letter())));
        }
    }
}

void DaxApplet::updateTxSliceLabel()
{
    if (!m_daxTxStatus) {
        return;
    }
    if (m_model) {
        for (SliceModel* slice : m_model->slices()) {
            if (slice->isTxSlice()) {
                m_daxTxStatus->setText(tr("Slice %1").arg(
                    SliceLabel::richText(slice->sliceId(), slice->letter())));
                return;
            }
        }
    }
    m_daxTxStatus->setText(QStringLiteral("—"));
}

void DaxApplet::setNativeReceiveRouting(bool native)
{
    if (m_nativeReceiveRouting == native) { return; }
    m_nativeReceiveRouting = native;
    for (int channel = 1; channel <= kChannels; ++channel) {
        setReceiveChannelSlice(channel,nullptr);
    }
}

void DaxApplet::setReceiveChannelSlice(int channel, SliceModel* slice)
{
    if (channel < 1 || channel > kChannels || !m_daxRxStatus[channel-1]) { return; }
    m_daxRxStatus[channel-1]->setText(slice
        ? tr("Slice %1").arg(SliceLabel::richText(slice->sliceId(),slice->letter()))
        : QStringLiteral("—"));
}

void DaxApplet::setDaxEnabled(bool on)
{
    if (!m_daxEnable) {  // no hosted-device controls on Windows
        return;
    }
    QSignalBlocker b(m_daxEnable);
    m_daxEnable->setChecked(on);
    m_daxEnable->setText(on ? "Enabled" : "Disabled");
}

void DaxApplet::setDaxRxLevel(int channel, float rms)
{
    if (channel < 1 || channel > kChannels || !m_daxRxMeter[channel - 1]) {
        return;  // no hosted-device controls on Windows
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
    if (!m_daxTxMeter) {  // no hosted-device controls on Windows
        return;
    }
    m_daxTxMeter->setLevel(std::clamp(rms * 2.0f, 0.0f, 1.0f));
}

} // namespace AetherSDR
