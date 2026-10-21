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
#endif  // !Q_OS_WIN
    buildBridgeUI(outer);
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

void DaxApplet::buildBridgeUI(QVBoxLayout* outer)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(4, 2, 4, 2);
    auto* label = new QLabel(tr("Audio bridge:"), this);
    label->setObjectName(QStringLiteral("daxBridgeLabel"));
    ThemeManager::instance().applyStyleSheet(label,
        QStringLiteral("QLabel { color: {{color.canon.muted}}; font-size: 11px; }"));
    row->addWidget(label);
    row->addStretch();
    m_daxEnable = new QPushButton(this);
    m_daxEnable->setObjectName(QStringLiteral("daxEnable"));
    m_daxEnable->setAccessibleName(tr("Audio bridge"));
    m_daxEnable->setCheckable(true);
    m_daxEnable->setFixedSize(96, 22);
    ThemeManager::instance().applyStyleSheet(m_daxEnable, QStringLiteral(
        "QPushButton { background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 3px;"
        " color: {{color.canon.inkSoft}}; font-size: 11px; font-weight: bold; padding: 2px 4px; }"
        "QPushButton:hover { background: {{color.canon.nested}}; }"
        "QPushButton:checked { background: {{color.toggle.success.background.checked}};"
        " color: {{color.toggle.success.foreground.checked}};"
        " border-color: {{color.toggle.success.border.checked}}; }"
        "QPushButton:disabled { color: {{color.control.unavailable}}; }"));
#if defined(Q_OS_MACOS) || defined(HAVE_PIPEWIRE)
    const bool autoStart = AppSettings::instance().value("AutoStartDAX", "False").toString() == "True";
    setDaxEnabled(autoStart);
    const QString description = tr("Enable local virtual audio routing and autostart DAX on connection. "
                                   "TX stream selection is separate.");
    m_daxEnable->setAccessibleDescription(description);
    m_daxEnable->setToolTip(description);
    connect(m_daxEnable, &QPushButton::toggled, this, [this](bool on) {
        m_daxEnable->setText(on ? tr("Enabled") : tr("Disabled"));
        auto& settings = AppSettings::instance();
        settings.setValue("AutoStartDAX", on ? "True" : "False");
        settings.save();
        emit daxToggled(on);
    });
#else
    m_daxEnable->setText(tr("Unavailable"));
    m_daxEnable->setEnabled(false);
#ifdef Q_OS_WIN
    const QString reason = tr("No built-in audio bridge on Windows. Use TCI or SmartSDR DAX. "
                              "SmartSDR DAX manages its own TX stream.");
#else
    const QString reason = tr("This build has no local audio bridge support. Use TCI for audio.");
#endif
    m_daxEnable->setAccessibleDescription(reason);
    m_daxEnable->setToolTip(reason);
#endif
    row->addWidget(m_daxEnable);
    outer->addLayout(row);
}

void DaxApplet::buildOwnershipUI(QVBoxLayout* outer)
{
    auto* section = new QWidget(this);
    section->setObjectName(QStringLiteral("daxTxOwnership"));
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(2);
    ThemeManager::instance().applyStyleSheet(section, QStringLiteral(
        "QWidget#daxTxOwnership { background: {{color.canon.nested}};"
        " border: 1px solid {{color.canon.line}}; }"
        "QLabel { color: {{color.canon.inkSoft}}; font-size: 11px; }"
        "QPushButton { background: {{color.canon.control}};"
        " border: 1px solid {{color.canon.lineHi}}; border-radius: 3px;"
        " padding: 2px 4px; font-size: 11px; font-weight: bold;"
        " color: {{color.canon.inkSoft}}; }"
        "QPushButton:hover { background: {{color.canon.nested}}; }"
        "QPushButton:checked { background: {{color.toggle.accent.background.checked}};"
        " color: {{color.canon.cyan}}; border-color: {{color.canon.cyan}}; }"
        "QPushButton:focus { border: 1px solid {{color.canon.aqua}}; }"
        "QPushButton:disabled { color: {{color.control.unavailable}};"
        " border-color: {{color.canon.line}}; }"));
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("TX stream:"), section));
    row->addStretch();
    m_daxTxToggle = new QPushButton(section);
    m_daxTxToggle->setObjectName(QStringLiteral("daxTxToggle"));
    m_daxTxToggle->setAccessibleName(tr("AetherSDR TX stream"));
    m_daxTxToggle->setCheckable(true);
    m_daxTxToggle->setFixedSize(96, 22);
    row->addWidget(m_daxTxToggle);
    layout->addLayout(row);
    m_daxTxOwnershipStatus = new QLabel(section);
    m_daxTxOwnershipStatus->setObjectName(QStringLiteral("daxTxOwnershipStatus"));
    m_daxTxOwnershipStatus->setTextFormat(Qt::PlainText);
    m_daxTxOwnershipStatus->setWordWrap(true);
    layout->addWidget(m_daxTxOwnershipStatus);
    outer->addWidget(section);

    connect(m_daxTxToggle, &QPushButton::toggled, this, [this](bool take) {
        const QPointer<DaxApplet> self(this);
        const QPointer<RadioModel> origin(m_model);
        // Restore the observation before dispatching the requested selection.
        updateDaxTxOwnership();
        if (self && origin && m_model == origin) {
            origin->requestDaxTxOwnership(take);
        }
        if (self) { self->updateDaxTxOwnership(); }
    });
    updateDaxTxOwnership();
}

void DaxApplet::updateDaxTxOwnership()
{
    m_ownershipAvailability.reset();
    const QJsonObject snapshot = m_model ? m_model->daxTxOwnershipSnapshot() : QJsonObject();
    const bool available = snapshot.value(QStringLiteral("available")).toBool();
    const bool pending = snapshot.value(QStringLiteral("pending")).toBool();
    const bool uncertain = snapshot.value(QStringLiteral("uncertain")).toBool();
    const bool known = snapshot.value(QStringLiteral("txKnown")).toBool();
    const bool owned = snapshot.value(QStringLiteral("owned")).toBool();
    const bool ready = snapshot.value(QStringLiteral("ready")).toBool();
    const QString state = snapshot.value(QStringLiteral("state")).toString();
    const bool noStream = snapshot.value(QStringLiteral("streamId")).toString() == QLatin1String("0x00000000");
    QString text;
    if (!available) {
        text = tr("Unavailable");
    } else if (pending) {
        text = state == QLatin1String("releasing") ? tr("Deselecting…") : tr("Selecting…");
    } else if (uncertain || (!known && !noStream)) {
        text = tr("Unknown");
    } else {
        text = owned ? tr("Selected") : tr("Not selected");
    }
    const bool take = !owned;
    const bool canRequest = available && !pending && !uncertain && (known || noStream)
        && snapshot.value(take ? QStringLiteral("canTake") : QStringLiteral("canRelease")).toBool();
    {
        const QSignalBlocker blocker(m_daxTxToggle);
        m_daxTxToggle->setChecked(available && !uncertain && known && owned);
        m_daxTxToggle->setText(text);
    }
    const QString description = tr("Select or deselect AetherSDR's DAX transmit-audio stream. "
                                   "This does not start transmission or control SmartSDR DAX's stream.");
    m_daxTxToggle->setAccessibleDescription(description);
    m_daxTxToggle->setToolTip(description);
    QString reason = snapshot.value(take ? QStringLiteral("takeReason") : QStringLiteral("releaseReason")).toString();
    if (reason.isEmpty()) {
        reason = tr("Waiting for confirmed TX stream status from a supported radio.");
    }
    if (m_model) {
        m_ownershipAvailability = std::make_unique<ControlAvailabilityRegistry>(*m_model, this);
        m_ownershipAvailability->registerWidget(m_daxTxToggle, reason,
            [canRequest](bool, const RadioCapabilities&) { return canRequest; },
            [owned] { return owned; }, false);
    } else {
        m_daxTxToggle->setEnabled(false);
        m_daxTxToggle->setToolTip(reason);
        m_daxTxToggle->setAccessibleDescription(reason);
    }

    QString status;
    if (available && known && owned && !ready) {
        status = tr("Audio blocked");
    }
    if (state == QLatin1String("refused")) {
        if (!status.isEmpty()) { status += QStringLiteral(" · "); }
        status += tr("Request refused");
    } else if (state == QLatin1String("timed-out")) {
        if (!status.isEmpty()) { status += QStringLiteral(" · "); }
        status += tr("Request timed out");
    }
    const QString error = snapshot.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        if (!status.isEmpty()) { status += QLatin1Char('\n'); }
        status += error;
    }
    // Reasons remain visible even when a disabled button cannot receive focus.
    if (!canRequest && status.isEmpty()) { status = reason; }
    const bool changed = m_daxTxOwnershipStatus->text() != status;
    m_daxTxOwnershipStatus->setText(status);
    m_daxTxOwnershipStatus->setVisible(!status.isEmpty());
    m_daxTxOwnershipStatus->setAccessibleName(tr("TX stream status"));
    m_daxTxOwnershipStatus->setAccessibleDescription(status);
    if (changed) {
        QAccessibleEvent event(m_daxTxOwnershipStatus, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
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
#if !defined(Q_OS_MACOS) && !defined(HAVE_PIPEWIRE)
    Q_UNUSED(on);
    return;
#endif
    if (!m_daxEnable) { return; }
    QSignalBlocker b(m_daxEnable);
    m_daxEnable->setChecked(on);
    m_daxEnable->setText(on ? tr("Enabled") : tr("Disabled"));
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
