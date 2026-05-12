#include "PanadapterApplet.h"
#include "FramelessMoveHelper.h"
#include "GuardedSlider.h"
#include "SpectrumWidget.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTextEdit>
#include <QWindow>
#include <QGuiApplication>
#include <QClipboard>

namespace AetherSDR {

PanadapterApplet::PanadapterApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── Title bar (16px gradient, matching applet style) ─────────────────
    m_titleBar = new QWidget;
    m_titleBar->setFixedHeight(16);
    m_titleBar->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #3a4a5a, stop:0.5 #2a3a4a, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");
    m_titleBar->installEventFilter(this);  // drag-to-move when floating
    auto* titleBar = m_titleBar;

    auto* barLayout = new QHBoxLayout(titleBar);
    barLayout->setContentsMargins(6, 1, 4, 1);
    barLayout->setSpacing(2);

    // Drag grip dots (matching applet title bar style)
    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"));
    grip->setStyleSheet("QLabel { background: transparent; color: #607080; font-size: 10px; }");
    barLayout->addWidget(grip);

    m_titleLabel = new QLabel("Slice A");
    m_titleLabel->setStyleSheet("QLabel { background: transparent; color: #8aa8c0; "
                                "font-size: 10px; font-weight: bold; }");
    barLayout->addWidget(m_titleLabel);
    barLayout->addStretch();

    const QString btnStyle = QStringLiteral(
        "QPushButton { background: transparent; color: #6a8090; "
        "border: none; font-size: 9px; padding: 0; }"
        "QPushButton:hover { color: #c8d8e8; }");

    // Pop-out / Dock button (⬈ when docked, ↩ when floating)
    m_popOutBtn = new QPushButton("\u2b08");  // ⬈
    m_popOutBtn->setFixedSize(16, 14);
    m_popOutBtn->setStyleSheet(btnStyle + "QPushButton { font-size: 11px; }");
    m_popOutBtn->setToolTip("Pop out panadapter");
    m_popOutBtn->hide();  // hidden in single-pan mode
    connect(m_popOutBtn, &QPushButton::clicked, this, [this]() {
        if (m_isFloating) {
            emit dockClicked();
        } else {
            emit popOutClicked();
        }
    });
    barLayout->addWidget(m_popOutBtn);

    // Maximize button (□)
    m_maxBtn = new QPushButton("\u25a1");  // □
    m_maxBtn->setFixedSize(16, 14);
    m_maxBtn->setStyleSheet(btnStyle + "QPushButton { font-size: 11px; }");
    m_maxBtn->setToolTip("Maximize panadapter");
    m_maxBtn->hide();  // hidden in single-pan mode
    connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
        emit maximizeRequested(m_panId);
    });
    barLayout->addWidget(m_maxBtn);

    // Close button (×)
    m_closeBtn = new QPushButton("\u00D7");
    m_closeBtn->setFixedSize(16, 14);
    m_closeBtn->setStyleSheet(btnStyle + "QPushButton { font-size: 11px; } QPushButton:hover { color: #ff4040; }");
    m_closeBtn->setToolTip("Close panadapter");
    m_closeBtn->hide();  // hidden in single-pan mode
    connect(m_closeBtn, &QPushButton::clicked, this, [this]() {
        emit closeRequested(m_panId);
    });
    barLayout->addWidget(m_closeBtn);

    layout->addWidget(titleBar);

    // ── Spectrum widget (FFT + waterfall) ────────────────────────────────
    m_spectrum = new SpectrumWidget(this);
    m_spectrum->installEventFilter(this);  // detect clicks for pan activation
    layout->addWidget(m_spectrum, 1);

    // ── CW decode panel (hidden by default, shown in CW mode) ─────────
    m_cwPanel = new QWidget(this);
    m_cwPanel->setCursor(Qt::ArrowCursor);  // prevent invisible cursor from native-window parent (#1096)
    m_cwPanel->setFixedHeight(80);
    m_cwPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_cwPanel->setStyleSheet("QWidget { background: #0a0a14; border-top: 1px solid #203040; }");

    auto* cwLayout = new QVBoxLayout(m_cwPanel);
    cwLayout->setContentsMargins(4, 2, 4, 2);
    cwLayout->setSpacing(1);

    // Stats bar: pitch, speed, clear button
    auto* cwBar = new QHBoxLayout;
    cwBar->setSpacing(6);
    auto* cwTitle = new QLabel("CW");
    cwTitle->setStyleSheet("QLabel { color: #00b4d8; font-size: 10px; font-weight: bold; background: transparent; }");
    cwBar->addWidget(cwTitle);
    auto* cwHint = new QLabel("(requires PC Audio)");
    cwHint->setStyleSheet("QLabel { color: #405060; font-size: 9px; background: transparent; }");
    cwBar->addWidget(cwHint);

    m_cwStatsLabel = new QLabel;
    m_cwStatsLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 10px; background: transparent; }");
    m_cwStatsLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_cwStatsLabel->setFixedWidth(m_cwStatsLabel->fontMetrics().horizontalAdvance("1200 Hz  120 WPM"));
    cwBar->addWidget(m_cwStatsLabel);

    // Sensitivity slider — filters low-confidence decodes
    auto* sensLabel = new QLabel("Sens:");
    sensLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 9px; background: transparent; }");
    cwBar->addWidget(sensLabel);
    m_cwSensSlider = new GuardedSlider(Qt::Horizontal);
    m_cwSensSlider->setRange(0, 100);  // 0=show everything, 100=only high confidence
    int savedSens = AppSettings::instance().value("CwDecoderSensitivity", "30").toString().toInt();
    m_cwSensSlider->setValue(savedSens);
    m_cwSensSlider->setFixedWidth(60);
    m_cwSensSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }");
    m_cwCostThreshold = 1.0f - (savedSens / 100.0f) * 0.9f;
    connect(m_cwSensSlider, &QSlider::valueChanged, this, [this](int v) {
        // Map 0-100 slider to 1.0-0.1 cost threshold (inverted: higher sens = lower threshold)
        m_cwCostThreshold = 1.0f - (v / 100.0f) * 0.9f;
        AppSettings::instance().setValue("CwDecoderSensitivity", QString::number(v));
        AppSettings::instance().save();
    });
    cwBar->addWidget(m_cwSensSlider);

    // Lock Pitch button
    m_lockPitchBtn = new QPushButton("\xF0\x9F\x94\x92P");  // 🔒P
    m_lockPitchBtn->setCheckable(true);
    m_lockPitchBtn->setFixedSize(28, 16);
    m_lockPitchBtn->setToolTip("Lock decoder pitch to current frequency");
    m_lockPitchBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #6a8090; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 8px; padding: 0; }"
        "QPushButton:checked { color: #00b4d8; border-color: #00b4d8; }"
        "QPushButton:hover { color: #c8d8e8; }");
    cwBar->addWidget(m_lockPitchBtn);

    // Lock Speed button
    m_lockSpeedBtn = new QPushButton("\xF0\x9F\x94\x92S");  // 🔒S
    m_lockSpeedBtn->setCheckable(true);
    m_lockSpeedBtn->setFixedSize(28, 16);
    m_lockSpeedBtn->setToolTip("Lock decoder speed to current WPM");
    m_lockSpeedBtn->setStyleSheet(m_lockPitchBtn->styleSheet());
    cwBar->addWidget(m_lockSpeedBtn);

    // Pitch range sliders — constrain decoder frequency search
    const QString rangeSliderStyle =
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #6a8090; width: 8px; margin: -3px 0; border-radius: 4px; }";

    auto* minLabel = new QLabel("Lo:");
    minLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    cwBar->addWidget(minLabel);

    m_pitchMinSlider = new GuardedSlider(Qt::Horizontal);
    m_pitchMinSlider->setRange(300, 1200);
    m_pitchMinSlider->setValue(500);
    m_pitchMinSlider->setFixedWidth(50);
    m_pitchMinSlider->setStyleSheet(rangeSliderStyle);
    m_pitchMinSlider->setToolTip("Decoder pitch search minimum (Hz)");
    cwBar->addWidget(m_pitchMinSlider);
    m_pitchMinValLabel = new QLabel(QString::number(m_pitchMinSlider->value()));
    m_pitchMinValLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    m_pitchMinValLabel->setFixedWidth(24);
    cwBar->addWidget(m_pitchMinValLabel);

    auto* maxLabel = new QLabel("Hi:");
    maxLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    cwBar->addWidget(maxLabel);

    m_pitchMaxSlider = new GuardedSlider(Qt::Horizontal);
    m_pitchMaxSlider->setRange(300, 1200);
    m_pitchMaxSlider->setValue(700);
    m_pitchMaxSlider->setFixedWidth(50);
    m_pitchMaxSlider->setStyleSheet(rangeSliderStyle);
    m_pitchMaxSlider->setToolTip("Decoder pitch search maximum (Hz)");
    cwBar->addWidget(m_pitchMaxSlider);
    m_pitchMaxValLabel = new QLabel(QString::number(m_pitchMaxSlider->value()));
    m_pitchMaxValLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 8px; background: transparent; }");
    m_pitchMaxValLabel->setFixedWidth(24);
    cwBar->addWidget(m_pitchMaxValLabel);

    // Update tooltips and emit range change — clamp so min ≤ max
    connect(m_pitchMinSlider, &QSlider::valueChanged, this, [this](int v) {
        if (v > m_pitchMaxSlider->value()) {
            QSignalBlocker b(m_pitchMinSlider);
            m_pitchMinSlider->setValue(m_pitchMaxSlider->value());
            v = m_pitchMaxSlider->value();
        }
        m_pitchMinSlider->setToolTip(QString("%1 Hz").arg(v));
        m_pitchMinValLabel->setText(QString::number(v));
        emit pitchRangeChanged(v, m_pitchMaxSlider->value());
    });
    connect(m_pitchMaxSlider, &QSlider::valueChanged, this, [this](int v) {
        if (v < m_pitchMinSlider->value()) {
            QSignalBlocker b(m_pitchMaxSlider);
            m_pitchMaxSlider->setValue(m_pitchMinSlider->value());
            v = m_pitchMinSlider->value();
        }
        m_pitchMaxSlider->setToolTip(QString("%1 Hz").arg(v));
        m_pitchMaxValLabel->setText(QString::number(v));
        emit pitchRangeChanged(m_pitchMinSlider->value(), v);
    });
    m_pitchMinSlider->setToolTip(QString("%1 Hz").arg(m_pitchMinSlider->value()));
    m_pitchMaxSlider->setToolTip(QString("%1 Hz").arg(m_pitchMaxSlider->value()));

    cwBar->addStretch();

    const QString cwBtnStyle =
        "QPushButton { background: #1a2a3a; color: #8090a0; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 9px; font-weight: bold;"
        " padding: 1px 6px; }"
        "QPushButton:hover { color: #c8d8e8; background: #2a3a4a; }";

    auto* copyAllBtn = new QPushButton("CPY ALL");
    copyAllBtn->setToolTip("Copy all decoded text to clipboard");
    copyAllBtn->setStyleSheet(cwBtnStyle);
    connect(copyAllBtn, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_cwText->toPlainText());
    });
    cwBar->addWidget(copyAllBtn);

    auto* copyVisBtn = new QPushButton("CPY VIS");
    copyVisBtn->setToolTip("Copy visible text to clipboard");
    copyVisBtn->setStyleSheet(cwBtnStyle);
    connect(copyVisBtn, &QPushButton::clicked, this, [this] {
        QRect visibleRect = m_cwText->viewport()->rect();
        QTextCursor topCursor = m_cwText->cursorForPosition(visibleRect.topLeft());
        QTextCursor bottomCursor = m_cwText->cursorForPosition(visibleRect.bottomRight());
        topCursor.setPosition(bottomCursor.position(), QTextCursor::KeepAnchor);
        QGuiApplication::clipboard()->setText(topCursor.selectedText().replace(QChar(0x2029), '\n'));
    });
    cwBar->addWidget(copyVisBtn);

    auto* clearBtn = new QPushButton("CLR");
    clearBtn->setStyleSheet(cwBtnStyle);
    connect(clearBtn, &QPushButton::clicked, this, &PanadapterApplet::clearCwText);
    cwBar->addWidget(clearBtn);

    auto* closeBtn = new QPushButton("\u2715");
    closeBtn->setToolTip("Close CW decoder");
    closeBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #8090a0; border: 1px solid #203040;"
        " border-radius: 2px; font-size: 9px; font-weight: bold;"
        " padding: 1px 6px; }"
        "QPushButton:hover { color: #ff6060; background: #2a3a4a; }");
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        m_cwPanel->hide();
        emit cwPanelCloseRequested();
    });
    cwBar->addWidget(closeBtn);

    cwLayout->addLayout(cwBar);

    // Text area
    m_cwText = new QTextEdit;
    m_cwText->setReadOnly(true);
    m_cwText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_cwText->setStyleSheet(
        "QTextEdit { background: #0a0a14; color: #00ff88; border: none;"
        " font-family: monospace; font-size: 13px; font-weight: bold; }"
        "QScrollBar:vertical { width: 6px; background: #0a0a14; }"
        "QScrollBar::handle:vertical { background: #304050; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    m_cwText->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_cwText->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cwText->setWordWrapMode(QTextOption::WrapAnywhere);
    m_cwText->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_cwText, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = m_cwText->createStandardContextMenu();
        menu->addSeparator();
        menu->addAction(tr("Clear"), this, &PanadapterApplet::clearCwText);
        menu->exec(m_cwText->mapToGlobal(pos));
        delete menu;
    });
    cwLayout->addWidget(m_cwText);

    m_cwPanel->hide();
    layout->addWidget(m_cwPanel);
}

void PanadapterApplet::setMultiPanMode(bool multi)
{
    // In single-pan mode, hide all decorations — nothing to pop out, maximize, or close
    if (m_popOutBtn) m_popOutBtn->setVisible(multi);
    if (m_maxBtn) m_maxBtn->setVisible(multi);
    if (m_closeBtn) m_closeBtn->setVisible(multi);
}

void PanadapterApplet::setFloatingState(bool floating)
{
    m_isFloating = floating;
    if (m_popOutBtn) {
        m_popOutBtn->setText(floating ? "\u21a9" : "\u2b08");  // ↩ or ⬈
        m_popOutBtn->setToolTip(floating ? "Dock panadapter" : "Pop out panadapter");
        m_popOutBtn->setVisible(true);  // always visible when floating
    }
    // When floating, always show close (to dock via window close)
    if (m_closeBtn && floating) m_closeBtn->setVisible(true);
}

void PanadapterApplet::setSliceId(int id)
{
    const char letters[] = "ABCDEFGH";
    const char letter = (id >= 0 && id < 8) ? letters[id] : '?';
    m_titleLabel->setText(QString("Slice %1").arg(letter));
}

void PanadapterApplet::clearSliceTitle()
{
    m_titleLabel->clear();
}

QString PanadapterApplet::sliceTitle() const
{
    return m_titleLabel->text();
}

void PanadapterApplet::setCwPanelVisible(bool visible)
{
    m_cwPanel->setVisible(visible);
}

void PanadapterApplet::appendCwText(const QString& text, float cost)
{
    // Filter by sensitivity threshold — drop low-confidence decodes
    if (cost >= m_cwCostThreshold) return;

    // Strip newlines — ggmorse inserts them on pitch changes, but we want
    // continuous flowing text. Replace with space to preserve word boundaries.
    QString clean = text;
    clean.replace('\n', ' ');

    // Color by confidence: lower cost = higher confidence
    //   < 0.15  green   (high confidence)
    //   < 0.35  yellow  (medium)
    //   < 0.60  orange  (meh)
    //   >= 0.60 red     (low confidence)
    QString color;
    if (cost < 0.15f)      color = "#00ff88";
    else if (cost < 0.35f) color = "#e0e040";
    else if (cost < 0.60f) color = "#ff9020";
    else                   color = "#ff4040";

    m_cwText->moveCursor(QTextCursor::End);
    m_cwText->insertHtml(QString("<span style=\"color:%1\">%2</span>")
        .arg(color, clean.toHtmlEscaped()));
    m_cwText->moveCursor(QTextCursor::End);
}

void PanadapterApplet::setCwStats(float pitchHz, float speedWpm)
{
    if (pitchHz > 0 && speedWpm > 0)
        m_cwStatsLabel->setText(QString("%1 Hz  %2 WPM").arg(pitchHz, 0, 'f', 0).arg(speedWpm, 0, 'f', 0));
}

void PanadapterApplet::clearCwText()
{
    m_cwText->clear();
}

bool PanadapterApplet::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_titleBar && m_isFloating && ev->type() == QEvent::MouseMove) {
        return FramelessMoveHelper::move(m_titleBar, static_cast<QMouseEvent*>(ev));
    }
    if (obj == m_titleBar && m_isFloating && ev->type() == QEvent::MouseButtonRelease) {
        return FramelessMoveHelper::finish(m_titleBar, static_cast<QMouseEvent*>(ev));
    }

    // Title-bar drag in floating mode → move the OS window via Qt 6's
    // cross-platform move helper.  Frameless pop-outs have no native
    // title bar, so the applet's own title strip becomes the drag handle.
    if (obj == m_titleBar && m_isFloating
        && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        return FramelessMoveHelper::start(m_titleBar, me);
    }
    if (ev->type() == QEvent::MouseButtonPress)
        emit activated(m_panId);
    return QWidget::eventFilter(obj, ev);
}

} // namespace AetherSDR
