#include "CanonMessageBox.h"

#include "CanonIndicators.h"
#include "ModemChrome.h"

#include <QAccessible>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The texts wrap at the narrower width; one long unbreakable token (a path in
// a command to copy) widens the box up to the wider one rather than clipping.
constexpr int kTextWidth = 420;
constexpr int kMaxTextWidth = 720;

} // namespace

CanonMessageBox::CanonMessageBox(const QString& title, const QString& text, QWidget* parent)
    : CanonWindow(title, parent)
{
    setObjectName(QStringLiteral("canonMessageBox"));
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
    // Texts carry manifest and device strings (a product name, a version, a
    // path): never render them as rich text, the title included.
    QWidget* header = makeCanonHeader(title);
    if (auto* heading = header->findChild<QLabel*>(QStringLiteral("canonHeaderTitle")))
        heading->setTextFormat(Qt::PlainText);
    root->addWidget(header);

    // A screen reader announces a QMessageBox's text when it opens; this
    // dialog is told the same through its description.
    setAccessibleDescription(text);
    m_text = new QLabel(text);
    m_text->setObjectName(QStringLiteral("canonMessageText"));
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setMinimumWidth(kTextWidth);
    m_text->setMaximumWidth(kMaxTextWidth);
    root->addWidget(m_text);

    m_info = new QLabel;
    m_info->setObjectName(QStringLiteral("canonMessageInfo"));
    m_info->setTextFormat(Qt::PlainText);
    m_info->setWordWrap(true);
    m_info->setMinimumWidth(kTextWidth);
    m_info->setMaximumWidth(kMaxTextWidth);
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
    setAccessibleDescription(text.isEmpty() ? m_text->text()
                                            : m_text->text() + QLatin1Char('\n') + text);
}

void CanonMessageBox::setTextSelectable(bool on)
{
    const Qt::TextInteractionFlags flags = on ? Qt::TextSelectableByMouse : Qt::NoTextInteraction;
    m_text->setTextInteractionFlags(flags);
    m_info->setTextInteractionFlags(flags);
}

QPushButton* CanonMessageBox::addButton(const QString& text, Role role)
{
    // Auto-default, as in QMessageBox: a focused button takes Return itself,
    // and the default button takes it only while no other button has focus.
    auto* button = new QPushButton(text);
    button->setAutoDefault(true);
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
    m_clicked = nullptr;   // this showing's answer, not a previous one's
    if (m_default) {
        m_default->setFocus();
    }
    // As QMessageBox does, so a screen reader reads the texts as it opens.
    QAccessibleEvent alert(this, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

} // namespace AetherSDR
