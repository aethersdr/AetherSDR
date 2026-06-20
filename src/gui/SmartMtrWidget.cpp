#include "SmartMtrWidget.h"
#include "core/ThemeManager.h"

#include <QHBoxLayout>
#include <QLabel>

namespace AetherSDR {

SmartMtrWidget::SmartMtrWidget(QWidget* parent)
    : QWidget(parent)
{
    // Transparent so the VFO flag's painted background shows through, matching
    // the spacer it replaces in the meter row.
    setAttribute(Qt::WA_TranslucentBackground);
    // The whole meter strip is one click target (toggles the selector); let
    // clicks fall through to VfoWidget::mousePressEvent instead of being eaten
    // by this placeholder.
    setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    setAccessibleName(tr("SmartMTR meter"));

    m_label = new QLabel(QStringLiteral("SmartMTR"));
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    // Muted, lightweight readout styling so the placeholder sits cleanly within
    // the 22 px meter strip and reads as a meter element (like the dBm label),
    // not bold text crowding the frequency above it.
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_label,
        "QLabel { background: transparent; border: none; "
        "color: #6888a0; font-size: 11px; font-weight: bold; "
        "letter-spacing: 1px; }");
    layout->addWidget(m_label);
}

QSize SmartMtrWidget::sizeHint() const
{
    // Placeholder height — matches the S-meter strip (22px) for now so toggling
    // doesn't jump.  When the real meter component lands it can return a
    // different height and the VfoWidget will resize to fit.
    return QSize(QWidget::sizeHint().width(), 22);
}

} // namespace AetherSDR
