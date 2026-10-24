#include "CanonMessageBox.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kTextWidth = 420;

} // namespace

CanonMessageBox::CanonMessageBox(const QString& title, const QString& text, QWidget* parent)
    : CanonWindow(title, parent)
{
    setObjectName(QStringLiteral("canonMessageBox"));
    setModal(true);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
               + QStringLiteral(
                   "QLabel#canonMessageText { color: {{color.canon.ink}}; }"
                   "QLabel#canonMessageInfo { color: {{color.canon.inkSoft}}; }"
                   "QPushButton { padding: 6px 18px; }"
                   "QPushButton:default { color: {{color.canon.cyan}}; border-color: {{color.canon.cyan}}; }"
                   "QPushButton[role=\"destructive\"] { color: {{color.canon.sparkGold}}; }");
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(12);
    root->addWidget(makeCanonHeader(title));

    m_text = new QLabel(text);
    m_text->setObjectName(QStringLiteral("canonMessageText"));
    m_text->setWordWrap(true);
    m_text->setFixedWidth(kTextWidth);
    root->addWidget(m_text);

    m_info = new QLabel;
    m_info->setObjectName(QStringLiteral("canonMessageInfo"));
    m_info->setWordWrap(true);
    m_info->setFixedWidth(kTextWidth);
    m_info->hide();
    root->addWidget(m_info);

    root->addSpacing(4);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(8);
    m_buttons->addStretch(1);
    root->addLayout(m_buttons);
}

void CanonMessageBox::setInformativeText(const QString& text)
{
    m_info->setText(text);
    m_info->setVisible(!text.isEmpty());
}

void CanonMessageBox::setTextSelectable(bool on)
{
    const Qt::TextInteractionFlags flags = on ? Qt::TextSelectableByMouse : Qt::NoTextInteraction;
    m_text->setTextInteractionFlags(flags);
    m_info->setTextInteractionFlags(flags);
}

QPushButton* CanonMessageBox::addButton(const QString& text, Role role)
{
    auto* button = new QPushButton(text);
    button->setAutoDefault(false);
    if (role == Role::Destructive) {
        button->setProperty("role", QStringLiteral("destructive"));
    }
    connect(button, &QPushButton::clicked, this, [this, button, role] {
        m_clicked = button;
        if (role == Role::Reject) {
            reject();
        } else {
            accept();
        }
    });
    m_buttons->addWidget(button);
    m_added.append(button);
    return button;
}

void CanonMessageBox::setDefaultButton(QPushButton* button)
{
    for (QPushButton* b : std::as_const(m_added)) {
        b->setDefault(b == button);
    }
    m_default = button;
}

void CanonMessageBox::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    if (m_default) {
        m_default->setFocus();
    }
}

} // namespace AetherSDR
