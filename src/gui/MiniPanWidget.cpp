#include "gui/MiniPanWidget.h"

#include "gui/MiniPanScope.h"
#include "gui/FramelessWindowTitleBar.h"
#include "gui/FramelessResizer.h"
#include "gui/Theme.h"
#include "core/AppSettings.h"
#include "core/MiniPanSettings.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QCloseEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QActionGroup>
#include <QByteArray>
#include <QFont>

namespace AetherSDR {

MiniPanWidget::MiniPanWidget(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Mini-Pan"));
    setObjectName("miniPanWindow");   // addressable for automation/tests
    setAccessibleName(tr("Mini-pan"));
    setAccessibleDescription(
        tr("Floating narrow-span scope centred on the active VFO. "
           "Right-click for span and always-on-top."));

    const bool frameless =
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True";
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;
    setWindowFlags(flags);   // re-apply — some platform plugins ignore the ctor bitmask

    setAttribute(Qt::WA_DeleteOnClose, false);   // close == hide; single long-lived instance
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    // The app template already paints the window background and body text from
    // color.background.0 / color.text.primary and re-themes live; a second
    // setStyleSheet() here would REPLACE it (one stylesheet per widget) and
    // hardcode a colour the theme cannot reach. (#4562 review)
    applyAppTheme(this);
    setMinimumSize(240, 140);
    resize(340, 200);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar("Mini-Pan", this);
    m_titleBar->setVisible(frameless);   // OS chrome supplies the bar when not frameless
    m_layout->addWidget(m_titleBar);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(8, 4, 8, 8);
    bodyLayout->setSpacing(2);

    m_freqLabel = new QLabel(body);
    m_freqLabel->setObjectName("miniPanFreq");
    QFont f = m_freqLabel->font();
    f.setPointSize(20);
    f.setBold(true);
    m_freqLabel->setFont(f);
    m_freqLabel->setAlignment(Qt::AlignHCenter);
    m_freqLabel->setAccessibleName(tr("Mini-pan centre frequency"));
    bodyLayout->addWidget(m_freqLabel);

    m_scope = new MiniPanScope(body);
    m_scope->setObjectName("miniPanScope");
    m_scope->setDbmRange(-130.0f, -40.0f);
    bodyLayout->addWidget(m_scope, 1);

    // Restore the client-side display span (±5/±10 kHz) — no emit at construction;
    // MainWindow reads spanMhz() when it creates the pan. MiniPanSettings owns
    // the validation, so a hand-edited value can't reach the radio.
    m_spanKHz = MiniPanSettings::spanKHz();
    m_scope->setSpanKHz(m_spanKHz);

    m_layout->addWidget(body, 1);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this,
            [this]() { saveGeometryToSettings(); });

    // Debounce resize → one scopeResized() so MainWindow re-pushes xpixels once,
    // not once per intermediate resize step.
    m_xpixTimer.setSingleShot(true);
    m_xpixTimer.setInterval(150);
    connect(&m_xpixTimer, &QTimer::timeout, this, [this]() { emit scopeResized(); });

    FramelessResizer::install(this);

    // Restore always-on-top (useful floating over contest-logging software).
    if (MiniPanSettings::alwaysOnTop())
        setAlwaysOnTop(true);

    refreshHeader();
}

void MiniPanWidget::setCenterMhz(double mhz)
{
    m_centerMhz = mhz;
    refreshHeader();
}

void MiniPanWidget::setSpanKHz(double kHz)
{
    applySpanKHz(kHz, /*persistAndEmit=*/false);
}

void MiniPanWidget::applySpanKHz(double kHz, bool persistAndEmit)
{
    m_spanKHz = kHz;
    if (m_scope) m_scope->setSpanKHz(kHz);
    if (persistAndEmit) {
        MiniPanSettings::setSpanKHz(kHz);
        emit spanChanged(kHz);   // MainWindow re-pushes the radio pan bandwidth
    }
}

void MiniPanWidget::contextMenuEvent(QContextMenuEvent* e)
{
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

    menu.addSeparator();
    QAction* top = menu.addAction(tr("Always on Top"));
    top->setCheckable(true);
    top->setChecked(m_alwaysOnTop);
    connect(top, &QAction::toggled, this, [this](bool on) {
        setAlwaysOnTop(on);
        MiniPanSettings::setAlwaysOnTop(on);
    });

    menu.exec(e->globalPos());
}

void MiniPanWidget::setPassbandHz(int lowHz, int highHz)
{
    if (m_scope) m_scope->setPassbandHz(lowHz, highHz);
}

void MiniPanWidget::refreshHeader()
{
    m_freqLabel->setText(m_centerMhz > 0.0
                             ? QString::number(m_centerMhz, 'f', 6)
                             : QStringLiteral("—.———"));
}

// ── Window chrome / persistence ───────────────────────────────────────────────

void MiniPanWidget::setFramelessMode(bool on)
{
    const bool wasVisible = isVisible();
    const QRect geom = geometry();
    Qt::WindowFlags flags = windowFlags();
    if (on) flags |= Qt::FramelessWindowHint;
    else    flags &= ~Qt::FramelessWindowHint;
    setWindowFlags(flags);
    if (m_titleBar) m_titleBar->setVisible(on);
    if (wasVisible) { setGeometry(geom); show(); }
}

void MiniPanWidget::setAlwaysOnTop(bool on)
{
    if (m_alwaysOnTop == on) return;
    m_alwaysOnTop = on;
    const bool wasVisible = isVisible();
    const QRect geom = geometry();
    Qt::WindowFlags flags = windowFlags();
    if (on) flags |= Qt::WindowStaysOnTopHint;
    else    flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    if (wasVisible) { setGeometry(geom); show(); }
}

void MiniPanWidget::saveGeometryToSettings() const
{
    MiniPanSettings::setGeometryBase64(saveGeometry().toBase64());
}

void MiniPanWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!m_geometryRestored) {
        m_geometryRestored = true;
        const QByteArray geom =
            QByteArray::fromBase64(MiniPanSettings::geometryBase64());
        if (!geom.isEmpty()) {
            m_restoring = true;
            restoreGeometry(geom);
            m_restoring = false;
        }
    }
}

void MiniPanWidget::moveEvent(QMoveEvent* e)
{
    QWidget::moveEvent(e);
    if (!m_restoring) m_saveTimer.start();
}

void MiniPanWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (!m_restoring) {
        m_saveTimer.start();
        m_xpixTimer.start();   // → scopeResized() (debounced)
    }
}

void MiniPanWidget::closeEvent(QCloseEvent* e)
{
    // Close == hide: keep the instance; MainWindow frees the radio-side pan on
    // closedByUser. Geometry/open-state persist for the next session.
    m_saveTimer.stop();
    saveGeometryToSettings();
    MiniPanSettings::setOpen(false);
    e->ignore();
    hide();
    emit closedByUser();
}

} // namespace AetherSDR
