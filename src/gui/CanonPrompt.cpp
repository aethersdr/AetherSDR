#include "gui/CanonPrompt.h"

#include "gui/CanonIndicators.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

QString sheet()
{
    return QStringLiteral(
        "QLabel { background: transparent; }"
        "QLabel#canonPromptEyebrow { color: {{color.accent.warning}}; font-size: 11px; font-weight: 700;"
        " letter-spacing: 1.5px; }"
        "QLabel#canonPromptText { color: {{color.canon.ink}}; font-size: 14px; }"
        "QPushButton { border-radius: 10px; padding: 9px 16px; font-weight: 600; }"
        "QPushButton#canonPromptCancel { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
        " border: 1px solid {{color.canon.lineHi}}; }"
        "QPushButton#canonPromptCancel:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
        "QPushButton#canonPromptCancel:focus { border-color: {{color.canon.aqua}}; }"
        "QPushButton#canonPromptConfirm { background: transparent; color: {{color.accent.warning}};"
        " border: 1px solid {{color.accent.warning}}; }"
        "QPushButton#canonPromptConfirm:hover { background: {{color.canon.nested}}; }"
        "QPushButton#canonPromptConfirm:focus { border-width: 2px; padding: 8px 15px; }");
}

} // namespace

CanonPrompt::CanonPrompt(const QString& title, const QString& text, const QString& confirmLabel,
                         QWidget* parent)
    : CanonWindow(title, parent)
{
    setModal(true);
    applyCanonSheet(bodyWidget(), sheet);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(28, 24, 28, 22);
    root->setSpacing(12);

    auto* eyebrow = new QLabel(QStringLiteral("⚠ ") + title.toUpper());
    eyebrow->setObjectName(QStringLiteral("canonPromptEyebrow"));
    eyebrow->setTextFormat(Qt::PlainText);
    root->addWidget(eyebrow);

    m_text = new QLabel;
    m_text->setObjectName(QStringLiteral("canonPromptText"));
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_text->setFixedWidth(420);
    root->addWidget(m_text);

    root->addSpacing(6);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("canonPromptCancel"));
    m_confirm = new QPushButton(confirmLabel);
    m_confirm->setObjectName(QStringLiteral("canonPromptConfirm"));
    m_confirm->setAutoDefault(false);
    m_cancel->setDefault(true);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_confirm);
    root->addLayout(buttons);

    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_confirm, &QPushButton::clicked, this, &QDialog::accept);
    setText(text);
    m_cancel->setFocus();
}

void CanonPrompt::setText(const QString& text)
{
    m_text->setText(text);
    setAccessibleDescription(text);
    adjustSize();
}

QString CanonPrompt::text() const
{
    return m_text->text();
}

} // namespace AetherSDR
