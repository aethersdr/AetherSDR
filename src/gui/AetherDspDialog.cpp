#include "AetherDspDialog.h"
#include "AetherDspWidget.h"
#include "FramelessMoveHelper.h"
#include "core/AppSettings.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

namespace {
constexpr int kResizeMargin = 12;
}

namespace AetherSDR {

AetherDspDialog::AetherDspDialog(AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("AetherDSP Settings");
    setMouseTracking(true);
    setStyleSheet("QDialog { background: #0f0f1a; color: #c8d8e8; }");

    // Outer layout: zero-margin so the title bar runs edge-to-edge.  The
    // 6 px resize hit zone lives on the bare gap around the inner content
    // widget (which carries its own padding).
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Custom title bar ─────────────────────────────────────────────
    // Same chrome family as NetworkDiagnosticsDialog / AetherialAudioStrip:
    // 18 px tall, blue-gradient background, 10 px bold title, trio of
    // window-control buttons at the right.
    {
        m_titleBar = new QWidget(this);
        m_titleBar->setFixedHeight(18);
        m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
        m_titleBar->setStyleSheet(
            "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
            "border-bottom: 1px solid #0a1a28; }");
        m_titleBar->installEventFilter(this);

        auto* tbRow = new QHBoxLayout(m_titleBar);
        tbRow->setContentsMargins(6, 0, 2, 0);
        tbRow->setSpacing(4);

        auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"),
                                m_titleBar);
        grip->setStyleSheet(
            "QLabel { background: transparent; color: #a0b4c8;"
            " font-size: 10px; }");
        tbRow->addWidget(grip);

        auto* tbTitle = new QLabel("AetherDSP Settings", m_titleBar);
        tbTitle->setStyleSheet(
            "QLabel { background: transparent; color: #e0ecf4;"
            " font-size: 10px; font-weight: bold; }");
        tbRow->addWidget(tbTitle);
        tbRow->addStretch();

        const QString btnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; }";
        const QString closeBtnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; background: #cc2030; }";

        auto* minBtn = new QPushButton(QString::fromUtf8("\xe2\x80\x94"), m_titleBar);
        minBtn->setFixedSize(16, 16);
        minBtn->setCursor(Qt::ArrowCursor);
        minBtn->setStyleSheet(btnStyle);
        minBtn->setToolTip("Minimize");
        connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
        tbRow->addWidget(minBtn);

        auto* maxBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xa1"), m_titleBar);
        maxBtn->setFixedSize(16, 16);
        maxBtn->setCursor(Qt::ArrowCursor);
        maxBtn->setStyleSheet(btnStyle);
        maxBtn->setToolTip("Maximize");
        connect(maxBtn, &QPushButton::clicked, this, [this]() {
            if (isMaximized()) showNormal(); else showMaximized();
        });
        tbRow->addWidget(maxBtn);

        auto* closeBtn = new QPushButton(QString::fromUtf8("\xc3\x97"), m_titleBar);
        closeBtn->setFixedSize(16, 16);
        closeBtn->setCursor(Qt::ArrowCursor);
        closeBtn->setStyleSheet(closeBtnStyle);
        closeBtn->setToolTip("Close");
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
        tbRow->addWidget(closeBtn);

        root->addWidget(m_titleBar);
    }

    // Content area — 6 px padding on every side keeps the resize hit zone
    // reachable while the widget carries its own internal margins.
    auto* content = new QWidget(this);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(6, 6, 6, 6);
    body->setSpacing(0);

    m_widget = new AetherDspWidget(audio, this);
    // Scale all internal fonts up to 13 px to match the VFO DSP toggle
    // row.  Applet path leaves this off and renders at the original sizes.
    m_widget->setDialogMode(true);
    body->addWidget(m_widget);
    root->addWidget(content, 1);

    // Forward every parameter-change signal so existing connections to
    // AetherDspDialog::* keep working unchanged.
    connect(m_widget, &AetherDspWidget::nr2GainMaxChanged,
            this,    &AetherDspDialog::nr2GainMaxChanged);
    connect(m_widget, &AetherDspWidget::nr2GainSmoothChanged,
            this,    &AetherDspDialog::nr2GainSmoothChanged);
    connect(m_widget, &AetherDspWidget::nr2QsppChanged,
            this,    &AetherDspDialog::nr2QsppChanged);
    connect(m_widget, &AetherDspWidget::nr2GainMethodChanged,
            this,    &AetherDspDialog::nr2GainMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2NpeMethodChanged,
            this,    &AetherDspDialog::nr2NpeMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2AeFilterChanged,
            this,    &AetherDspDialog::nr2AeFilterChanged);
    connect(m_widget, &AetherDspWidget::mnrEnabledChanged,
            this,    &AetherDspDialog::mnrEnabledChanged);
    connect(m_widget, &AetherDspWidget::mnrStrengthChanged,
            this,    &AetherDspDialog::mnrStrengthChanged);
    connect(m_widget, &AetherDspWidget::dfnrAttenLimitChanged,
            this,    &AetherDspDialog::dfnrAttenLimitChanged);
    connect(m_widget, &AetherDspWidget::dfnrPostFilterBetaChanged,
            this,    &AetherDspDialog::dfnrPostFilterBetaChanged);
    connect(m_widget, &AetherDspWidget::nr4ReductionChanged,
            this,    &AetherDspDialog::nr4ReductionChanged);
    connect(m_widget, &AetherDspWidget::nr4SmoothingChanged,
            this,    &AetherDspDialog::nr4SmoothingChanged);
    connect(m_widget, &AetherDspWidget::nr4WhiteningChanged,
            this,    &AetherDspDialog::nr4WhiteningChanged);
    connect(m_widget, &AetherDspWidget::nr4AdaptiveNoiseChanged,
            this,    &AetherDspDialog::nr4AdaptiveNoiseChanged);
    connect(m_widget, &AetherDspWidget::nr4NoiseMethodChanged,
            this,    &AetherDspDialog::nr4NoiseMethodChanged);
    connect(m_widget, &AetherDspWidget::nr4MaskingDepthChanged,
            this,    &AetherDspDialog::nr4MaskingDepthChanged);
    connect(m_widget, &AetherDspWidget::nr4SuppressionChanged,
            this,    &AetherDspDialog::nr4SuppressionChanged);

    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void AetherDspDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }
    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (wasVisible) {
        show();
    }
}

void AetherDspDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void AetherDspDialog::selectTab(const QString& name)
{
    if (m_widget) m_widget->selectTab(name);
}

// ──────────────────────────────────────────────────────────────────
// Frameless 8-axis resize + drag-to-move
// ──────────────────────────────────────────────────────────────────

Qt::Edges AetherDspDialog::edgesAt(const QPoint& pos) const
{
    if (!(windowFlags() & Qt::FramelessWindowHint)) return {};
    if (isMaximized() || isFullScreen()) return {};
    Qt::Edges edges;
    if (pos.x() <= kResizeMargin)              edges |= Qt::LeftEdge;
    else if (pos.x() >= width() - kResizeMargin) edges |= Qt::RightEdge;
    if (pos.y() <= kResizeMargin)              edges |= Qt::TopEdge;
    else if (pos.y() >= height() - kResizeMargin) edges |= Qt::BottomEdge;
    return edges;
}

void AetherDspDialog::updateResizeCursor(const QPoint& pos)
{
    const Qt::Edges edges = edgesAt(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges & (Qt::LeftEdge | Qt::TopEdge))     == (Qt::LeftEdge | Qt::TopEdge)
        || (edges & (Qt::RightEdge | Qt::BottomEdge)) == (Qt::RightEdge | Qt::BottomEdge)) {
        shape = Qt::SizeFDiagCursor;
    } else if ((edges & (Qt::RightEdge | Qt::TopEdge))    == (Qt::RightEdge | Qt::TopEdge)
        ||     (edges & (Qt::LeftEdge | Qt::BottomEdge)) == (Qt::LeftEdge | Qt::BottomEdge)) {
        shape = Qt::SizeBDiagCursor;
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        shape = Qt::SizeHorCursor;
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        shape = Qt::SizeVerCursor;
    }
    setCursor(shape);
}

void AetherDspDialog::mouseMoveEvent(QMouseEvent* ev)
{
    if (!(ev->buttons() & Qt::LeftButton))
        updateResizeCursor(ev->pos());
    QDialog::mouseMoveEvent(ev);
}

void AetherDspDialog::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        const Qt::Edges edges = edgesAt(ev->pos());
        if (edges) {
            if (auto* h = windowHandle()) {
                h->startSystemResize(edges);
                ev->accept();
                return;
            }
        }
    }
    QDialog::mousePressEvent(ev);
}

void AetherDspDialog::leaveEvent(QEvent* ev)
{
    setCursor(Qt::ArrowCursor);
    QDialog::leaveEvent(ev);
}

bool AetherDspDialog::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_titleBar && ev->type() == QEvent::MouseMove) {
        return FramelessMoveHelper::move(m_titleBar, static_cast<QMouseEvent*>(ev));
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonRelease) {
        return FramelessMoveHelper::finish(m_titleBar, static_cast<QMouseEvent*>(ev));
    }

    // Drag-to-move via the custom title bar.  The trio buttons are their
    // own QPushButtons that consume the press themselves, so this only
    // fires on the bare title-bar background.
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        return FramelessMoveHelper::start(m_titleBar, me);
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonDblClick) {
        if (isMaximized()) showNormal();
        else               showMaximized();
        ev->accept();
        return true;
    }
    return QDialog::eventFilter(obj, ev);
}

} // namespace AetherSDR
