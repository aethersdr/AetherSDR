#include "gui/MiniPanApplet.h"

#include "gui/MiniPanScope.h"
#include "core/MiniPanSettings.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QShowEvent>
#include <QHideEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QActionGroup>
#include <QFont>

#include <algorithm>

namespace AetherSDR {

MiniPanApplet::MiniPanApplet(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("miniPanApplet");   // addressable for automation/tests
    setAccessibleName(tr("Mini-pan"));
    setAccessibleDescription(
        tr("Narrow-span scope centred on the active VFO. "
           "Right-click to choose ±5 or ±10 kHz."));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(2);

    m_freqLabel = new QLabel(this);
    m_freqLabel->setObjectName("miniPanFreq");
    QFont f = m_freqLabel->font();
    f.setPointSize(18);
    f.setBold(true);
    m_freqLabel->setFont(f);
    m_freqLabel->setAlignment(Qt::AlignHCenter);
    m_freqLabel->setAccessibleName(tr("Mini-pan centre frequency"));
    layout->addWidget(m_freqLabel);

    m_scope = new MiniPanScope(this);
    m_scope->setObjectName("miniPanScope");
    m_scope->setDbmRange(-130.0f, -40.0f);
    layout->addWidget(m_scope, 1);

    // MiniPanSettings owns the validation, so a hand-edited value can only ever
    // be one of the two spans the menu offers.
    m_spanKHz = MiniPanSettings::spanKHz();
    m_scope->setSpanKHz(m_spanKHz);

    refreshHeader();
}

QSize MiniPanApplet::sizeHint() const
{
    // 260 is the applet panel's fixed width — also the width it has in Minimal
    // Mode. The trace is resampled to the scope's width, so the useful detail
    // is set by the MAIN pan's bin width, not by this number.
    return {260, 150};
}

void MiniPanApplet::setCenterMhz(double mhz)
{
    m_centerMhz = mhz;
    refreshHeader();
}

void MiniPanApplet::setSpanKHz(double kHz)
{
    applySpanKHz(kHz, /*persistAndEmit=*/false);
}

void MiniPanApplet::setPassbandHz(int lowHz, int highHz)
{
    if (m_scope) m_scope->setPassbandHz(lowHz, highHz);
}

void MiniPanApplet::applySpanKHz(double kHz, bool persistAndEmit)
{
    m_spanKHz = kHz;
    if (m_scope) m_scope->setSpanKHz(kHz);
    if (persistAndEmit) {
        MiniPanSettings::setSpanKHz(kHz);
        emit spanChanged(kHz);   // the next frame re-slices to the new window
    }
}

void MiniPanApplet::refreshHeader()
{
    m_freqLabel->setText(m_centerMhz > 0.0
                             ? QString::number(m_centerMhz, 'f', 6)
                             : QStringLiteral("—.———"));
}

void MiniPanApplet::contextMenuEvent(QContextMenuEvent* e)
{
    // Span only. Float / dock / always-on-top / close all live on the standard
    // ContainerTitleBar this applet is wrapped in — duplicating them here would
    // be a second, drifting copy of framework behaviour.
    QMenu menu(this);
    auto* spanGroup = new QActionGroup(&menu);
    spanGroup->setExclusive(true);
    const auto addSpan = [&](const QString& text, double kHz) {
        QAction* a = menu.addAction(text);
        a->setCheckable(true);
        a->setChecked(qFuzzyCompare(m_spanKHz, kHz));
        a->setActionGroup(spanGroup);
        connect(a, &QAction::triggered, this, [this, kHz]() {
            applySpanKHz(kHz, /*persistAndEmit=*/true);
        });
    };
    addSpan(tr("±5 kHz"),  MiniPanSettings::kSpanNarrowKHz);   // 10 kHz span
    addSpan(tr("±10 kHz"), MiniPanSettings::kSpanWideKHz);      // 20 kHz span
    menu.exec(e->globalPos());
}

// ── Feed lifecycle ────────────────────────────────────────────────────────────
// show/hide is the one hook that catches every way this applet can come and go:
// the tray toggle, the container's close button, a float, a dock, and the
// panel-wide layout apply. MainWindow starts consuming pan frames on true and
// stops on false — nothing radio-side is created or destroyed either way.

void MiniPanApplet::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    emit feedWanted(true);
}

void MiniPanApplet::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    emit feedWanted(false);
}

} // namespace AetherSDR
