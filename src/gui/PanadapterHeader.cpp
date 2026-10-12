#include "PanadapterHeader.h"

#include "core/ThemeManager.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <algorithm>

namespace AetherSDR {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("AetherSDR::PanadapterApplet", text);
}

QPushButton* makeHeaderButton(const QString& glyph, const QString& name)
{
    auto* button = new QPushButton(glyph);
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFixedSize(18, 18);
    button->setCursor(Qt::PointingHandCursor);
    button->hide();   // the owner shows it (multi-pan, floating, canvas)
    return button;
}

} // namespace

QString PanadapterHeader::styleSheet()
{
    return QStringLiteral(
        "QWidget#panHeader, QWidget#panHeaderButtons { background: transparent; }"
        "QLabel { background: transparent; }"
        "QLabel#panHeaderGrip { color: {{color.canon.muted}}; font-size: 11px; }"
        "QLabel#panHeaderTitle { color: {{color.canon.ink}}; font-size: 12px; font-weight: 700; }"
        "QPushButton, QToolButton { background: transparent; color: {{color.canon.muted}}; "
        "border: 1px solid transparent; border-radius: 9px; font-size: 11px; padding: 0; }"
        "QPushButton:hover, QToolButton:hover { background: {{color.canon.nested}}; "
        "color: {{color.canon.ink}}; border-color: {{color.canon.lineHi}}; }"
        "QPushButton:focus, QToolButton:focus { border: 1px solid {{color.canon.cyan}}; }");
}

PanadapterHeader PanadapterHeader::build(QWidget* host)
{
    PanadapterHeader h;
    auto& tm = ThemeManager::instance();

    h.titleBar = new QWidget(host);
    h.titleBar->setObjectName(QStringLiteral("panHeader"));
    h.titleBar->setCursor(Qt::ArrowCursor);
    // Addressable for the automation bridge (#3646/#4864 pattern): the
    // canvas live-move stream starts here, and a drag the bridge cannot
    // aim at is a drag no smoke can regress-test.
    h.titleBar->setAccessibleName(QStringLiteral("panTitleBar"));
    // A press or double-click the title does not use stays in the header
    // instead of falling through to the spectrum's tuning.
    h.titleBar->setAttribute(Qt::WA_NoMousePropagation);
    tm.applyStyleSheet(h.titleBar, styleSheet());

    auto* titleLayout = new QHBoxLayout(h.titleBar);
    titleLayout->setContentsMargins(8, 0, 8, 0);
    titleLayout->setSpacing(6);

    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"));
    grip->setObjectName(QStringLiteral("panHeaderGrip"));
    titleLayout->addWidget(grip);

    h.title = new QLabel;
    h.title->setObjectName(QStringLiteral("panHeaderTitle"));
    h.title->setTextFormat(Qt::RichText);  // slice letter may be HTML (#2606)
    titleLayout->addWidget(h.title);

    h.buttons = new QWidget(host);
    h.buttons->setObjectName(QStringLiteral("panHeaderButtons"));
    h.buttons->setCursor(Qt::ArrowCursor);
    h.buttons->setAttribute(Qt::WA_NoMousePropagation);
    tm.applyStyleSheet(h.buttons, styleSheet());

    h.buttonRow = new QHBoxLayout(h.buttons);
    h.buttonRow->setContentsMargins(0, 0, 4, 0);
    h.buttonRow->setSpacing(2);

    // Pop-out / Dock (⬈ when docked, ↩ when floating), then maximize (□).
    h.popOut = makeHeaderButton(QStringLiteral("⬈"), tr("Pop out panadapter"));
    h.popOut->setObjectName(QStringLiteral("panFloatToggle"));
    h.buttonRow->addWidget(h.popOut);
    h.maximize = makeHeaderButton(QStringLiteral("□"), tr("Maximize panadapter"));
    h.buttonRow->addWidget(h.maximize);
    return h;
}

int PanadapterHeader::place(int hostWidth) const
{
    const int titleW = std::min(titleBar->sizeHint().width(), hostWidth);
    titleBar->setGeometry(0, 0, titleW, kHeight);
    const int buttonsW = buttons->sizeHint().width();
    buttons->setGeometry(hostWidth - buttonsW, 0, buttonsW, kHeight);

    for (int i = 0; i < buttonRow->count(); ++i) {
        const QWidget* w = buttonRow->itemAt(i)->widget();
        if (w && !w->isHidden()) {
            return buttonsW;
        }
    }
    return 0;
}

} // namespace AetherSDR
