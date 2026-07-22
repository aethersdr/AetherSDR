#include "SliceAudioSwitcherDialog.h"
#include "SliceColorManager.h"
#include "SliceLabel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

const QString kSoloModeSettingsKey = QStringLiteral("SliceAudioSwitcherSoloMode");

QString muteGlyph(bool muted)
{
    return muted ? QString::fromUtf8("\xF0\x9F\x94\x87")   // U+1F507
                 : QString::fromUtf8("\xF0\x9F\x94\x8A");  // U+1F50A
}

// Simple luminance-based pick so the mute glyph/label stays readable against
// both light and dark per-slice colors (SliceColorManager custom colors can
// land anywhere in the wheel).
QColor contrastingTextColor(const QColor& background)
{
    const double luminance = (0.299 * background.red() + 0.587 * background.green()
                               + 0.114 * background.blue()) / 255.0;
    return luminance > 0.55 ? QColor(0, 0, 0) : QColor(255, 255, 255);
}

} // namespace

SliceAudioSwitcherDialog::SliceAudioSwitcherDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog("Slice Audio Switcher", "SliceAudioSwitcherDialogGeometry", parent)
    , m_model(model)
{
    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setSpacing(4);

    m_soloCheck = new QCheckBox(QStringLiteral("Solo (only one slice audible)"), bodyWidget());
    m_soloCheck->setChecked(AppSettings::instance().value(kSoloModeSettingsKey, false).toBool());
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_soloCheck,
        "QCheckBox { color: {{color.text.secondary}}; font-size: 11px; spacing: 6px; }"
        + ThemeManager::checkBoxIndicatorStyle());
    connect(m_soloCheck, &QCheckBox::toggled, this, [](bool on) {
        AppSettings::instance().setValue(kSoloModeSettingsKey, on);
    });
    outer->addWidget(m_soloCheck);

    m_buttonRow = new QHBoxLayout();
    m_buttonRow->setSpacing(6);
    outer->addLayout(m_buttonRow);

    setSlotCount(m_model->maxSlices() > 0 ? m_model->maxSlices() : 1);

    connect(m_model, &RadioModel::sliceAdded, this, [this](SliceModel* slice) {
        if (slice && slice->sliceId() >= 0
            && slice->sliceId() < static_cast<int>(m_slots.size())) {
            rebuildSlot(slice->sliceId());
        }
    });
    connect(m_model, &RadioModel::sliceRemoved, this, [this](int sliceId) {
        if (sliceId >= 0 && sliceId < static_cast<int>(m_slots.size()))
            rebuildSlot(sliceId);
    });
    // Radio capacity (4 vs 8 slices on dual-SCU models) is only known once
    // info arrives, and can change across a reconnect to a different model.
    connect(m_model, &RadioModel::infoChanged, this, [this]() {
        setSlotCount(m_model->maxSlices() > 0 ? m_model->maxSlices() : 1);
    });
    // Custom slice colors (Settings) apply globally, not per-slice, so
    // refresh every populated slot rather than waiting on a per-slice signal.
    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged, this, [this]() {
        for (auto& slot : m_slots) {
            if (slot.slice)
                updateButton(slot.button, slot.slice);
        }
    });
}

void SliceAudioSwitcherDialog::setSlotCount(int count)
{
    if (count < 1)
        count = 1;
    if (count == static_cast<int>(m_slots.size()))
        return;

    if (count < static_cast<int>(m_slots.size())) {
        for (int id = count; id < static_cast<int>(m_slots.size()); ++id) {
            QObject::disconnect(m_slots[id].muteConn);
            QObject::disconnect(m_slots[id].letterConn);
            delete m_slots[id].button;
        }
        m_slots.resize(count);
    } else {
        const int oldSize = static_cast<int>(m_slots.size());
        m_slots.resize(count);
        for (int id = oldSize; id < count; ++id) {
            auto* btn = new QPushButton(bodyWidget());
            btn->setFixedSize(56, 40);
            btn->setCursor(Qt::PointingHandCursor);
            m_buttonRow->addWidget(btn);
            m_slots[id].button = btn;

            connect(btn, &QPushButton::clicked, this, [this, id] {
                SliceModel* clicked = nullptr;
                for (auto* slice : m_model->slices()) {
                    if (slice && slice->sliceId() == id) {
                        clicked = slice;
                        break;
                    }
                }
                if (!clicked)
                    return;

                clicked->setAudioMute(!clicked->audioMute());

                // Solo mode: turning a slice's audio ON mutes every other
                // slice so only one is ever audible at a time. Muting the
                // soloed slice itself doesn't touch the others.
                if (m_soloCheck->isChecked() && !clicked->audioMute()) {
                    for (auto* slice : m_model->slices()) {
                        if (slice && slice->sliceId() != id && !slice->audioMute())
                            slice->setAudioMute(true);
                    }
                }
            });

            rebuildSlot(id);
        }
    }

    setMinimumSize(count * 62, 86);
}

void SliceAudioSwitcherDialog::rebuildSlot(int sliceId)
{
    Slot& slot = m_slots[sliceId];
    QObject::disconnect(slot.muteConn);
    QObject::disconnect(slot.letterConn);
    slot.slice = nullptr;

    SliceModel* slice = nullptr;
    for (auto* s : m_model->slices()) {
        if (s && s->sliceId() == sliceId) {
            slice = s;
            break;
        }
    }

    QPushButton* btn = slot.button;
    btn->setEnabled(slice != nullptr);

    if (!slice) {
        btn->setText(QString("%1\n\xE2\x80\x94").arg(QChar('A' + sliceId)));
        btn->setAccessibleName(QString("Slice %1 audio mute toggle").arg(QChar('A' + sliceId)));
        AetherSDR::ThemeManager::instance().applyStyleSheet(btn,
            "QPushButton { background: {{color.background.1}}; color: {{color.text.secondary}}; "
            "border: none; border-radius: 4px; font-weight: bold; }");
        return;
    }

    slot.slice = slice;
    updateButton(btn, slice);
    slot.muteConn = connect(slice, &SliceModel::audioMuteChanged, this,
                             [this, btn, slice](bool) { updateButton(btn, slice); });
    slot.letterConn = connect(slice, &SliceModel::letterChanged, this,
                               [this, btn, slice](const QString&) { updateButton(btn, slice); });
}

void SliceAudioSwitcherDialog::updateButton(QPushButton* button, SliceModel* slice) const
{
    if (!slice)
        return;

    const int colourIdx = SliceLabel::displayColorIndex(slice->sliceId(), slice->letter());
    const bool muted = slice->audioMute();
    const QColor bg = muted ? SliceColorManager::instance().dimColor(colourIdx)
                             : SliceColorManager::instance().activeColor(colourIdx);
    const QColor fg = contrastingTextColor(bg);

    button->setText(QString("%1\n%2")
                         .arg(SliceLabel::unicodeForm(slice->sliceId(), slice->letter()))
                         .arg(muteGlyph(muted)));
    button->setAccessibleName(QString("Slice %1 audio mute toggle").arg(slice->letter()));
    button->setToolTip(muted ? QString("Slice %1 muted — click to unmute").arg(slice->letter())
                              : QString("Slice %1 audio on — click to mute").arg(slice->letter()));
    button->setAccessibleDescription(muted ? QStringLiteral("Currently muted")
                                            : QStringLiteral("Currently on"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(button,
        QString("QPushButton { background: %1; color: %2; border: none; "
                "border-radius: 4px; font-weight: bold; }")
            .arg(bg.name(), fg.name()));
}

} // namespace AetherSDR
