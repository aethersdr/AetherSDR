#include "gui/MiniPanWidget.h"

#include "gui/MiniPanScope.h"
#include "gui/FramelessWindowTitleBar.h"
#include "gui/FramelessResizer.h"
#include "gui/Theme.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QCloseEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QByteArray>
#include <QFont>

namespace AetherSDR {

MiniPanWidget::MiniPanWidget(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle("Mini-Pan");
    setObjectName("miniPanWindow");   // addressable for automation/tests

    const bool frameless =
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True";
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;
    setWindowFlags(flags);   // re-apply — some platform plugins ignore the ctor bitmask

    setAttribute(Qt::WA_DeleteOnClose, false);   // close == hide; single long-lived instance
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    applyAppTheme(this);
    setStyleSheet("QWidget#miniPanWindow { background: #0a162c; }");
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
    m_freqLabel->setStyleSheet("color: #e0e0e0;");
    m_freqLabel->setAlignment(Qt::AlignHCenter);
    bodyLayout->addWidget(m_freqLabel);

    m_scope = new MiniPanScope(body);
    m_scope->setObjectName("miniPanScope");
    m_scope->setDbmRange(-130.0f, -40.0f);
    m_scope->setSpanKHz(10.0);
    bodyLayout->addWidget(m_scope, 1);

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
    refreshHeader();
}

void MiniPanWidget::setCenterMhz(double mhz)
{
    m_centerMhz = mhz;
    refreshHeader();
}

void MiniPanWidget::setSpanKHz(double kHz)
{
    if (m_scope) m_scope->setSpanKHz(kHz);
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
    AppSettings::instance().setValue(kGeometryKey, saveGeometry().toBase64());
}

void MiniPanWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!m_geometryRestored) {
        m_geometryRestored = true;
        const QByteArray geom = QByteArray::fromBase64(
            AppSettings::instance().value(kGeometryKey, "").toByteArray());
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
    AppSettings::instance().setValue(kOpenKey, "False");
    AppSettings::instance().save();
    e->ignore();
    hide();
    emit closedByUser();
}

} // namespace AetherSDR
