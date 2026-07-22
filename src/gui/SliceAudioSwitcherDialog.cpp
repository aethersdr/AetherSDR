#include "SliceAudioSwitcherDialog.h"
#include "SliceColorManager.h"
#include "SliceLabel.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace AetherSDR {

namespace {

QString muteGlyph(bool muted)
{
    return muted ? QString::fromUtf8("\xF0\x9F\x94\x87")   // U+1F507
                 : QString::fromUtf8("\xF0\x9F\x94\x8A");  // U+1F50A
}

} // namespace

SliceAudioSwitcherDialog::SliceAudioSwitcherDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog("Slice Audio", "SliceAudioSwitcherDialogGeometry", parent)
    , m_model(model)
{
    auto* layout = new QHBoxLayout(bodyWidget());
    layout->setSpacing(6);

    for (int id = 0; id < static_cast<int>(m_slots.size()); ++id) {
        auto* btn = new QPushButton(bodyWidget());
        btn->setFixedSize(56, 40);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setAccessibleName(QString("Slice %1 audio mute toggle").arg(QChar('A' + id)));
        layout->addWidget(btn);
        m_slots[id].button = btn;

        connect(btn, &QPushButton::clicked, this, [this, id] {
            for (auto* slice : m_model->slices()) {
                if (slice && slice->sliceId() == id) {
                    slice->setAudioMute(!slice->audioMute());
                    break;
                }
            }
        });

        rebuildSlot(id);
    }

    setMinimumSize(4 * 62, 56);

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
}

void SliceAudioSwitcherDialog::rebuildSlot(int sliceId)
{
    Slot& slot = m_slots[sliceId];
    QObject::disconnect(slot.muteConn);

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
        AetherSDR::ThemeManager::instance().applyStyleSheet(btn,
            "QPushButton { background: {{color.background.1}}; color: {{color.text.secondary}}; "
            "border: none; border-radius: 4px; font-weight: bold; }");
        return;
    }

    updateButton(btn, slice);
    slot.muteConn = connect(slice, &SliceModel::audioMuteChanged, this,
                             [this, btn, slice](bool) { updateButton(btn, slice); });
}

void SliceAudioSwitcherDialog::updateButton(QPushButton* button, SliceModel* slice) const
{
    if (!slice)
        return;

    const int colourIdx = SliceLabel::displayColorIndex(slice->sliceId(), slice->letter());
    const bool muted = slice->audioMute();
    const QColor bg = muted ? SliceColorManager::instance().dimColor(colourIdx)
                             : SliceColorManager::instance().activeColor(colourIdx);

    button->setText(QString("%1\n%2")
                         .arg(SliceLabel::unicodeForm(slice->sliceId(), slice->letter()))
                         .arg(muteGlyph(muted)));
    button->setToolTip(muted ? QString("Slice %1 muted — click to unmute").arg(slice->letter())
                              : QString("Slice %1 audio on — click to mute").arg(slice->letter()));
    button->setAccessibleDescription(muted ? QStringLiteral("Currently muted")
                                            : QStringLiteral("Currently on"));
    button->setStyleSheet(
        QString("QPushButton { background: %1; color: #000000; border: none; "
                "border-radius: 4px; font-weight: bold; }")
            .arg(bg.name()));
}

} // namespace AetherSDR
